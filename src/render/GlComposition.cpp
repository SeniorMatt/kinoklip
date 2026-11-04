// SPDX-License-Identifier: GPL-3.0-or-later
// GlComposition implementation: the node passes of the Compositing page on the GPU.
#include "render/GlComposition.h"

#include "core/CompOps.h"
#include "core/NodeRegistry.h"
#include "render/CompMath.h"
#include "render/Generators.h"
#include "render/RenderLog.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

namespace k = nodekeys;

const QTransform kFullscreenNdc(2, 0, 0, 2, -1, -1);

// ---- Shaders (kFragmentHeader of GlKit is prepended: vUv, vQuad, fragColor) ----

/// Merge: foreground (through uFgMatrix: output uv -> foreground uv, antialiased at its
/// edges) times opacity, combined with the background by blend mode; then the mask mixes
/// between the background and the result. Blend functions: compmath::kBlendFunctionGlsl,
/// prepended (the same as compmath::blendChannel()).
const char* const kMergeFragment = R"(
uniform sampler2D uTex;     // background
uniform sampler2D uSecond;  // foreground
uniform sampler2D uMask;
uniform mat3 uFgMatrix;
uniform int uMode;
uniform float uOpacity;
uniform int uHasFg;
uniform int uHasMask;
vec3 straight(vec4 c) { return c.a > 0.0 ? c.rgb / c.a : vec3(0.0); }
void main()
{
    vec4 bg = texture(uTex, vUv);
    vec4 fg = vec4(0.0);
    if (uHasFg == 1) {
        vec2 f = (uFgMatrix * vec3(vUv, 1.0)).xy;
        vec2 px = max(fwidth(f), vec2(1e-6));
        vec2 d = min(f, 1.0 - f) / px;
        float cover = clamp(min(d.x, d.y) + 0.5, 0.0, 1.0);
        fg = texture(uSecond, f) * cover;
    }
    fg *= uOpacity;
    vec4 o;
    if (uMode == 0) {
        o = fg + bg * (1.0 - fg.a);
    } else {
        vec3 B = kkBlend3(uMode, straight(bg), straight(fg));
        o.rgb = fg.rgb * (1.0 - bg.a) + bg.rgb * (1.0 - fg.a) + fg.a * bg.a * B;
        o.a = fg.a + bg.a * (1.0 - fg.a);
    }
    if (uHasMask == 1)
        o = mix(bg, o, texture(uMask, vUv).a);
    fragColor = o;
}
)";

/// mix(a, b, amount · mask alpha): Dissolve, and the mask input of every other node.
const char* const kMixFragment = R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform sampler2D uMask;
uniform float uAmount;
uniform int uHasMask;
void main()
{
    float t = uAmount * (uHasMask == 1 ? texture(uMask, vUv).a : 1.0);
    fragColor = mix(texture(uTex, vUv), texture(uSecond, vUv), t);
}
)";

/// Color Corrector: compmath::applyCorrector() on the straight colour.
const char* const kCorrectorFragment = R"(
uniform sampler2D uTex;
uniform vec4 uLevels;   // in black, in white, levels gamma, (unused)
uniform vec4 uLgg;      // out black, out white, lift, gain
uniform vec2 uSat;      // gamma, saturation
vec3 straight(vec4 c) { return c.a > 0.0 ? c.rgb / c.a : vec3(0.0); }
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    vec3 c = clamp((straight(t) - uLevels.x) / max(uLevels.y - uLevels.x, 1e-4), 0.0, 1.0);
    c = pow(c, vec3(1.0 / uLevels.z));
    c = uLgg.x + c * (uLgg.y - uLgg.x);
    c = uLgg.w * (c + uLgg.z * (1.0 - c));
    c = pow(max(c, vec3(0.0)), vec3(1.0 / uSat.x));
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(l + (c - l) * uSat.y, 0.0, 1.0);
    fragColor = vec4(c * t.a, t.a);
}
)";

/// Channel Booleans: each output channel from a source (compmath::applyChannels()).
const char* const kChannelsFragment = R"(
uniform sampler2D uTex;
uniform ivec4 uSources;
vec3 straight(vec4 c) { return c.a > 0.0 ? c.rgb / c.a : vec3(0.0); }
void main()
{
    vec4 t = texture(uTex, vUv);
    vec3 s = straight(t);
    float v[7] = float[7](s.r, s.g, s.b, t.a, dot(s, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    vec4 o = vec4(v[uSources.x], v[uSources.y], v[uSources.z], v[uSources.w]);
    fragColor = vec4(o.rgb * o.a, o.a);
}
)";

/// Fast Noise: fractal value noise in (x, y, time), cells of 1 / uScale frame widths, octaves
/// halving in size and amplitude; an integer hash keeps it identical on every GPU.
const char* const kNoiseFragment = R"(
uniform vec2 uSize;     // frame size (timeline px)
uniform vec2 uCentre;   // timeline px
uniform vec2 uRot;      // cos, sin
uniform float uScale;   // cells across the frame width
uniform int uDetail;
uniform vec2 uShape;    // contrast, brightness
uniform float uTime;
uniform uint uSeed;
uniform vec4 uColor1;   // premultiplied
uniform vec4 uColor2;
uint hash(uvec3 p)
{
    uint h = p.x * 0x8da6b343u ^ p.y * 0xd8163841u ^ p.z * 0xcb1ab31fu ^ uSeed * 0x165667b1u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}
float lattice(ivec3 p) { return float(hash(uvec3(p)) & 0xffffu) / 65535.0; }
float valueNoise(vec3 p)
{
    ivec3 i = ivec3(floor(p));
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float a = mix(mix(lattice(i), lattice(i + ivec3(1, 0, 0)), u.x),
                  mix(lattice(i + ivec3(0, 1, 0)), lattice(i + ivec3(1, 1, 0)), u.x), u.y);
    float b = mix(mix(lattice(i + ivec3(0, 0, 1)), lattice(i + ivec3(1, 0, 1)), u.x),
                  mix(lattice(i + ivec3(0, 1, 1)), lattice(i + ivec3(1, 1, 1)), u.x), u.y);
    return mix(a, b, u.z);
}
void main()
{
    vec2 d = vUv * uSize - uCentre;
    vec2 q = vec2(uRot.x * d.x + uRot.y * d.y, -uRot.y * d.x + uRot.x * d.y) * (uScale / uSize.x);
    float sum = 0.0, amp = 1.0, norm = 0.0;
    for (int o = 0; o < uDetail; ++o) {
        sum += amp * valueNoise(vec3(q, uTime) + vec3(float(o) * 17.0));
        norm += amp;
        q *= 2.0;
        amp *= 0.5;
    }
    float v = clamp((sum / norm - 0.5) * uShape.x + 0.5 + uShape.y, 0.0, 1.0);
    fragColor = mix(uColor1, uColor2, v);
}
)";

bool build(GlKit& kit, GlProgram& program, const char* name, const char* source, QString* why)
{
    if (!program.build(kit.gl(), name, source, why))
        return false;
    GlFunctions* gl = kit.gl();
    gl->glUseProgram(program.id);
    gl->glUniform1i(program.uniform(gl, "uTex"), 0);
    gl->glUniform1i(program.uniform(gl, "uSecond"), 1);
    gl->glUniform1i(program.uniform(gl, "uMask"), 2);
    gl->glUseProgram(0);
    return true;
}

} // namespace

bool GlComposition::initialize(GlKit& kit, QString* why)
{
    GlFunctions* gl = kit.gl();
    const QByteArray merge = QByteArray(compmath::kBlendFunctionGlsl) + kMergeFragment;
    if (!build(kit, m_merge, "comp.merge", merge.constData(), why)
        || !build(kit, m_mix, "comp.mix", kMixFragment, why)
        || !build(kit, m_corrector, "comp.corrector", kCorrectorFragment, why)
        || !build(kit, m_channels, "comp.channels", kChannelsFragment, why)
        || !build(kit, m_noise, "comp.noise", kNoiseFragment, why))
        return false;
    m_mergeFgMatrix = m_merge.uniform(gl, "uFgMatrix");
    m_mergeMode = m_merge.uniform(gl, "uMode");
    m_mergeOpacity = m_merge.uniform(gl, "uOpacity");
    m_mergeHasFg = m_merge.uniform(gl, "uHasFg");
    m_mergeHasMask = m_merge.uniform(gl, "uHasMask");
    m_mixAmount = m_mix.uniform(gl, "uAmount");
    m_mixHasMask = m_mix.uniform(gl, "uHasMask");
    m_ccLevels = m_corrector.uniform(gl, "uLevels");
    m_ccLgg = m_corrector.uniform(gl, "uLgg");
    m_ccSat = m_corrector.uniform(gl, "uSat");
    m_chSources = m_channels.uniform(gl, "uSources");
    m_nSize = m_noise.uniform(gl, "uSize");
    m_nCentre = m_noise.uniform(gl, "uCentre");
    m_nRot = m_noise.uniform(gl, "uRot");
    m_nScale = m_noise.uniform(gl, "uScale");
    m_nDetail = m_noise.uniform(gl, "uDetail");
    m_nShape = m_noise.uniform(gl, "uShape");
    m_nTime = m_noise.uniform(gl, "uTime");
    m_nSeed = m_noise.uniform(gl, "uSeed");
    m_nColor1 = m_noise.uniform(gl, "uColor1");
    m_nColor2 = m_noise.uniform(gl, "uColor2");
    m_maskClip.masks.resize(1);
    m_stack.resize(kMaxDepth);
    for (auto& s : m_stack)
        s.reserve(16);
    return true;
}

void GlComposition::destroy(GlKit& kit)
{
    for (GlProgram* p : {&m_merge, &m_mix, &m_corrector, &m_channels, &m_noise})
        p->destroy(kit.gl());
    m_stack.clear();
}

// ---- Evaluation ----

int GlComposition::slotOf(quint64 id) const
{
    const std::vector<Slot>& list = m_stack[size_t(m_depth)];
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].id == id)
            return int(i);
    }
    return -1;
}

int GlComposition::inputOwner(const CompNode& node, const QString& port) const
{
    const auto it = node.inputs.constFind(port);
    if (it == node.inputs.constEnd() || !it->isConnected() || it->outputIndex != 0)
        return -1;
    const int s = slotOf(it->nodeId);
    return s < 0 ? -1 : m_stack[size_t(m_depth)][size_t(s)].owner;
}

GLuint GlComposition::textureOf(GlKit& kit, int owner) const
{
    if (owner < 0)
        return kit.transparentTexture();
    const Slot& s = m_stack[size_t(m_depth)][size_t(owner)];
    return s.own ? s.own->texture : kit.transparentTexture();
}

void GlComposition::release(int owner)
{
    if (owner < 0)
        return;
    Slot& s = m_stack[size_t(m_depth)][size_t(owner)];
    if (--s.refs <= 0)
        s.own = PooledTarget(); // back to the pool: reused by the next node's pass
}

void GlComposition::alias(int index, int owner)
{
    std::vector<Slot>& list = m_stack[size_t(m_depth)];
    // The node's readers read the owner instead: its count grows by theirs.
    list[size_t(index)].owner = owner;
    if (owner >= 0 && owner != index)
        list[size_t(owner)].refs += list[size_t(index)].refs;
}

PooledTarget GlComposition::newTarget(GlKit& kit, const CompEval& ev, bool clear)
{
    PooledTarget t = kit.acquire(ev.size);
    kit.bindTarget(*t);
    if (clear)
        kit.clearTransparent();
    return t;
}

PooledTarget GlComposition::evaluate(GlKit& kit, const CompEval& ev, quint64 nodeId)
{
    // Re-entrant (a MediaIn of a nested clip may evaluate another composition): one slot
    // list per depth.
    if (m_depth + 1 >= kMaxDepth || !ev.comp) {
        return newTarget(kit, ev, true);
    }
    ++m_depth;
    std::vector<Slot>& list = m_stack[size_t(m_depth)];
    list.clear();
    const QVector<quint64> order = compops::evaluationOrder(*ev.comp, nodeId);
    for (quint64 id : order)
        list.push_back(Slot {id, {}, -1, 0});
    // Reads: every connected input of every node in the pull (disabled nodes read only
    // their main input), plus the final read of the pulled node.
    for (quint64 id : order) {
        const CompNode* n = ev.comp->node(id);
        const auto count = [&](const NodeLink& l) {
            const int s = slotOf(l.nodeId);
            if (s >= 0 && l.outputIndex == 0)
                ++list[size_t(s)].refs;
        };
        if (!n->enabled) {
            if (const NodeLink* main = compops::mainInputLink(*n))
                count(*main);
        } else {
            for (const NodeLink& l : n->inputs)
                count(l);
        }
    }
    PooledTarget result;
    if (!order.isEmpty()) {
        ++list.back().refs;
        for (int i = 0; i < int(order.size()); ++i)
            evalNode(kit, ev, i);
        const Slot& last = list.back();
        if (last.owner >= 0)
            result = std::move(list[size_t(last.owner)].own);
    }
    list.clear();
    --m_depth;
    if (!result)
        result = newTarget(kit, ev, true);
    return result;
}

void GlComposition::evalNode(GlKit& kit, const CompEval& ev, int index)
{
    std::vector<Slot>& list = m_stack[size_t(m_depth)];
    const CompNode& n = *ev.comp->node(list[size_t(index)].id);
    const NodeDescriptor* d = NodeRegistry::instance().find(n.type);
    const auto releaseInputs = [&](bool onlyMain) {
        if (onlyMain) {
            if (d && !d->mainInput.isEmpty())
                release(inputOwner(n, d->mainInput));
            return;
        }
        for (auto it = n.inputs.constBegin(); it != n.inputs.constEnd(); ++it)
            release(inputOwner(n, it.key()));
    };
    const int mainOwner = d && !d->mainInput.isEmpty() ? inputOwner(n, d->mainInput) : -1;
    if (!d || !n.enabled || n.type == nodetypes::MediaOut) {
        if (!d && !m_warned.contains(n.type)) {
            m_warned.insert(n.type);
            qCWarning(lcRender) << "composition: unknown node type" << n.type;
        }
        // Pass-through: the node's image is its main input's (the alias takes over the read
        // this node would have made).
        alias(index, mainOwner);
        if (mainOwner >= 0)
            --list[size_t(mainOwner)].refs;
        return;
    }
    const int maskOwner = d->input(nodeports::Mask) ? inputOwner(n, nodeports::Mask) : -1;
    PooledTarget out;
    bool handledMask = false;
    if (n.type == nodetypes::MediaIn) {
        out = newTarget(kit, ev, true);
        if (ev.sources)
            ev.sources->drawMediaIn(n, ev.compFrame, *out);
    } else if (n.type == nodetypes::Merge) {
        const int fg = inputOwner(n, nodeports::Foreground);
        out = merge(kit, ev, n, mainOwner, fg, maskOwner);
        handledMask = true;
    } else if (n.type == nodetypes::Dissolve) {
        const int fg = inputOwner(n, nodeports::Foreground);
        const float t = float(std::clamp(n.params.value(k::Mix, ev.compFrame, 0.0), 0.0, 1.0));
        out = mixPass(kit, ev, textureOf(kit, mainOwner), textureOf(kit, fg),
            maskOwner >= 0 ? textureOf(kit, maskOwner) : 0, t);
        handledMask = true;
    } else if (n.type == nodetypes::Transform) {
        out = transformNode(kit, ev, n, mainOwner);
    } else if (n.type == nodetypes::Crop || n.type == nodetypes::Resize
        || n.type == nodetypes::Letterbox) {
        out = rectNode(kit, ev, n, mainOwner);
    } else if (n.type == nodetypes::Background) {
        out = background(kit, ev, n);
    } else if (n.type == nodetypes::Text) {
        out = text(kit, ev, n);
    } else if (n.type == nodetypes::FastNoise) {
        out = noise(kit, ev, n);
    } else if (d->category == NodeCategory::Mask) {
        out = maskNode(kit, ev, n, mainOwner);
        handledMask = true; // the mask port of a mask node combines (union), see maskNode()
    } else if (n.type == nodetypes::ColorCorrector || n.type == nodetypes::ChannelBooleans) {
        out = colorNode(kit, ev, n, mainOwner);
    } else if (!d->effectId.isEmpty()) {
        out = effectNode(kit, ev, n, mainOwner);
    }
    if (!out) {
        // Identity (or nothing to do): pass the main input through.
        if (maskOwner >= 0)
            release(maskOwner);
        for (auto it = n.inputs.constBegin(); it != n.inputs.constEnd(); ++it) {
            if (it.key() != d->mainInput && it.key() != nodeports::Mask)
                release(inputOwner(n, it.key()));
        }
        alias(index, mainOwner);
        if (mainOwner >= 0)
            --list[size_t(mainOwner)].refs;
        return;
    }
    if (maskOwner >= 0 && !handledMask) {
        // "The effect applies only inside the mask": mix(input, effect, mask).
        const GLuint input = d->mainInput.isEmpty() ? 0 : textureOf(kit, mainOwner);
        out = mixPass(kit, ev, input, out->texture, textureOf(kit, maskOwner), 1.0f);
    }
    list[size_t(index)].own = std::move(out);
    list[size_t(index)].owner = index;
    releaseInputs(false);
}

// ---- Passes ----

PooledTarget GlComposition::mixPass(
    GlKit& kit, const CompEval& ev, GLuint a, GLuint b, GLuint mask, float amount)
{
    PooledTarget out = newTarget(kit, ev, false);
    kit.setBlendOver(false);
    GlFunctions* gl = kit.gl();
    gl->glUseProgram(m_mix.id);
    kit.setFullscreen(m_mix);
    gl->glUniform1f(m_mixAmount, amount);
    gl->glUniform1i(m_mixHasMask, mask ? 1 : 0);
    kit.bindTexture(0, a ? a : kit.transparentTexture());
    kit.bindTexture(1, b ? b : kit.transparentTexture());
    kit.bindTexture(2, mask ? mask : kit.transparentTexture());
    kit.drawQuad();
    return out;
}

PooledTarget GlComposition::merge(
    GlKit& kit, const CompEval& ev, const CompNode& n, int bg, int fg, int mask)
{
    if (fg < 0)
        return {}; // nothing to put over the background: pass it through
    const ParamSet& p = n.params;
    // Output uv -> foreground uv: the inverse of foreground uv -> output uv.
    const QTransform toUv = QTransform::fromScale(1.0 / ev.frame.width(), 1.0 / ev.frame.height());
    const QTransform fgToOut
        = toUv.inverted() * compmath::mergeForegroundMatrix(p, ev.compFrame, ev.frame) * toUv;
    bool invertible = false;
    const QTransform outToFg = fgToOut.inverted(&invertible);
    PooledTarget out = newTarget(kit, ev, false);
    kit.setBlendOver(false);
    GlFunctions* gl = kit.gl();
    gl->glUseProgram(m_merge.id);
    kit.setFullscreen(m_merge);
    kit.setMatrix(m_mergeFgMatrix, outToFg);
    gl->glUniform1i(m_mergeMode, blendmodes::index(p.staticValue(k::BlendMode).toString()));
    gl->glUniform1f(
        m_mergeOpacity, float(std::clamp(p.value(k::Opacity, ev.compFrame, 1.0), 0.0, 1.0)));
    gl->glUniform1i(m_mergeHasFg, invertible ? 1 : 0);
    gl->glUniform1i(m_mergeHasMask, mask >= 0 ? 1 : 0);
    kit.bindTexture(0, textureOf(kit, bg));
    kit.bindTexture(1, textureOf(kit, fg));
    kit.bindTexture(2, textureOf(kit, mask));
    kit.drawQuad();
    return out;
}

void GlComposition::blendLayers(GlKit& kit, GLuint background, GLuint layer, int mode)
{
    kit.setBlendOver(false);
    GlFunctions* gl = kit.gl();
    gl->glUseProgram(m_merge.id);
    kit.setFullscreen(m_merge);
    kit.setMatrix(m_mergeFgMatrix, QTransform());
    gl->glUniform1i(m_mergeMode, mode);
    gl->glUniform1f(m_mergeOpacity, 1.0f);
    gl->glUniform1i(m_mergeHasFg, 1);
    gl->glUniform1i(m_mergeHasMask, 0);
    kit.bindTexture(0, background);
    kit.bindTexture(1, layer);
    kit.bindTexture(2, kit.transparentTexture());
    kit.drawQuad();
}

PooledTarget GlComposition::transformNode(
    GlKit& kit, const CompEval& ev, const CompNode& n, int input)
{
    if (input < 0)
        return {};
    const QTransform m = compmath::transformMatrix(n.params, ev.compFrame, ev.frame);
    if (m.isIdentity())
        return {};
    PooledTarget out = newTarget(kit, ev, true);
    kit.setBlendOver(false);
    // Unit quad -> timeline px -> transformed -> target px -> NDC.
    const QTransform quad = QTransform::fromScale(ev.frame.width(), ev.frame.height()) * m
        * QTransform::fromScale(ev.scale, ev.scale) * GlKit::pixelToNdc(ev.size);
    kit.blit(textureOf(kit, input), quad);
    return out;
}

PooledTarget GlComposition::rectNode(GlKit& kit, const CompEval& ev, const CompNode& n, int input)
{
    if (input < 0)
        return {};
    const QSizeF frame = ev.frame;
    const QRectF full(QPointF(0, 0), frame);
    const GLuint tex = textureOf(kit, input);
    const auto toTarget = [&](const QRectF& r) {
        return QRectF(
            r.x() * ev.scale, r.y() * ev.scale, r.width() * ev.scale, r.height() * ev.scale);
    };
    if (n.type == nodetypes::Crop) {
        const QRectF keep = compmath::cropRect(n.params, ev.compFrame, frame);
        if (keep == full)
            return {};
        PooledTarget out = newTarget(kit, ev, true);
        kit.setBlendOver(false);
        if (!keep.isEmpty()) {
            const QRectF crop(keep.x() / frame.width(), keep.y() / frame.height(),
                keep.width() / frame.width(), keep.height() / frame.height());
            kit.blit(tex, kFullscreenNdc, 1.0f, crop);
        }
        return out;
    }
    if (n.type == nodetypes::Resize) {
        const QRectF r = compmath::resizeRect(n.params, ev.compFrame, frame);
        if (r == full)
            return {};
        PooledTarget out = newTarget(kit, ev, true);
        kit.setBlendOver(false);
        kit.blit(tex, GlKit::quadToNdc(toTarget(r), ev.size));
        return out;
    }
    // Letterbox: the picture, then opaque bars outside the kept area.
    const QRectF keep = compmath::letterboxRect(n.params, ev.compFrame, frame);
    if (keep == full)
        return {};
    PooledTarget out = newTarget(kit, ev, false);
    kit.setBlendOver(false);
    kit.blit(tex, kFullscreenNdc);
    const std::array<float, 4> c = compmath::colorOf(n.params, k::Color, Qt::black);
    const QRectF bars[2] = {keep.width() < frame.width() ? QRectF(0, 0, keep.x(), frame.height())
                                                         : QRectF(0, 0, frame.width(), keep.y()),
        keep.width() < frame.width()
            ? QRectF(keep.right(), 0, frame.width() - keep.right(), frame.height())
            : QRectF(0, keep.bottom(), frame.width(), frame.height() - keep.bottom())};
    for (const QRectF& b : bars) {
        if (!b.isEmpty())
            kit.fill(GlKit::quadToNdc(toTarget(b), ev.size), c.data());
    }
    return out;
}

PooledTarget GlComposition::background(GlKit& kit, const CompEval& ev, const CompNode& n)
{
    if (!m_generators)
        return {};
    PooledTarget out = newTarget(kit, ev, false);
    m_generators->drawPattern(
        kit, compmath::backgroundFrame(n.params, ev.compFrame, ev.frame), ev.frame);
    return out;
}

PooledTarget GlComposition::text(GlKit& kit, const CompEval& ev, const CompNode& n)
{
    PooledTarget out = newTarget(kit, ev, true);
    QPointF offset;
    const TitleStyle style
        = compmath::textStyle(n.params, ev.compFrame, ev.size, ev.scale, ev.frame, &offset);
    if (style.text.isEmpty() || !ev.sources)
        return out;
    QRectF rect;
    const GLuint tex = ev.sources->titleTexture(style, &rect);
    if (!tex)
        return out;
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    kit.blit(tex, GlKit::quadToNdc(rect.translated(offset), ev.size));
    return out;
}

PooledTarget GlComposition::noise(GlKit& kit, const CompEval& ev, const CompNode& n)
{
    const compmath::NoiseParams np
        = compmath::noiseParams(n.params, ev.compFrame, ev.frame, ev.fps);
    PooledTarget out = newTarget(kit, ev, false);
    kit.setBlendOver(false);
    GlFunctions* gl = kit.gl();
    gl->glUseProgram(m_noise.id);
    kit.setFullscreen(m_noise);
    gl->glUniform2f(m_nSize, float(ev.frame.width()), float(ev.frame.height()));
    gl->glUniform2f(m_nCentre, float(np.centreX), float(np.centreY));
    gl->glUniform2f(m_nRot, float(np.cosA), float(np.sinA));
    gl->glUniform1f(m_nScale, float(np.scale));
    gl->glUniform1i(m_nDetail, np.detail);
    gl->glUniform2f(m_nShape, float(np.contrast), float(np.brightness));
    gl->glUniform1f(m_nTime, float(np.time));
    gl->glUniform1ui(m_nSeed, GLuint(np.seed));
    gl->glUniform4fv(m_nColor1, 1, np.color1.data());
    gl->glUniform4fv(m_nColor2, 1, np.color2.data());
    kit.drawQuad();
    return out;
}

PooledTarget GlComposition::maskNode(GlKit& kit, const CompEval& ev, const CompNode& n, int input)
{
    PooledTarget out = newTarget(kit, ev, false);
    kit.setBlendOver(false);
    const float white[4] = {1, 1, 1, 1};
    kit.fill(kFullscreenNdc, white);
    m_maskClip.masks[0] = compmath::maskOf(n, ev.compFrame, ev.frame);
    if (m_masks && m_maskClip.masks[0].isRenderable()) {
        const MaskLayerGeometry geometry {ev.frame, ev.scale, 0.0};
        if (PooledTarget matte = m_masks->apply(kit, *out, geometry, m_maskClip, ev.compFrame))
            out = std::move(matte);
    } else {
        kit.bindTarget(*out);
        kit.clearTransparent();
    }
    if (input >= 0) {
        // Masks plugged into a mask: the union (premultiplied "over" of white mattes).
        kit.bindTarget(*out);
        kit.setBlendOver(true);
        kit.blit(textureOf(kit, input), kFullscreenNdc);
        kit.setBlendOver(false);
    }
    return out;
}

PooledTarget GlComposition::colorNode(GlKit& kit, const CompEval& ev, const CompNode& n, int input)
{
    if (input < 0)
        return {};
    GlFunctions* gl = kit.gl();
    if (n.type == nodetypes::ColorCorrector) {
        const compmath::CorrectorParams c = compmath::correctorParams(n.params, ev.compFrame);
        if (c.isIdentity())
            return {};
        PooledTarget out = newTarget(kit, ev, false);
        kit.setBlendOver(false);
        gl->glUseProgram(m_corrector.id);
        kit.setFullscreen(m_corrector);
        gl->glUniform4f(m_ccLevels, c.inBlack, c.inWhite, c.levelGamma, 0.0f);
        gl->glUniform4f(m_ccLgg, c.outBlack, c.outWhite, c.lift, c.gain);
        gl->glUniform2f(m_ccSat, c.gamma, c.saturation);
        kit.bindTexture(0, textureOf(kit, input));
        kit.drawQuad();
        return out;
    }
    const std::array<int, 4> src = compmath::channelSources(n.params);
    if (src == std::array<int, 4> {0, 1, 2, 3})
        return {};
    PooledTarget out = newTarget(kit, ev, false);
    kit.setBlendOver(false);
    gl->glUseProgram(m_channels.id);
    kit.setFullscreen(m_channels);
    gl->glUniform4i(m_chSources, src[0], src[1], src[2], src[3]);
    kit.bindTexture(0, textureOf(kit, input));
    kit.drawQuad();
    return out;
}

PooledTarget GlComposition::effectNode(GlKit& kit, const CompEval& ev, const CompNode& n, int input)
{
    if (input < 0 || !m_effects)
        return {};
    const auto it = m_effects->constFind(n.type.mid(nodetypes::EffectPrefix.size()));
    if (it == m_effects->constEnd()) {
        if (!m_warned.contains(n.type)) {
            m_warned.insert(n.type);
            qCWarning(lcRender) << "composition: no GPU implementation for" << n.type;
        }
        return {};
    }
    GlEffect* effect = it->get();
    if (effect->isIdentity(n.params, ev.compFrame))
        return {};
    const Slot& s = m_stack[size_t(m_depth)][size_t(input)];
    if (!s.own)
        return {};
    EffectInput in;
    in.layer = s.own.get();
    in.scale = ev.scale;
    in.localFrame = ev.compFrame;
    in.params = &n.params;
    in.fps = ev.fps;
    // Time effects (Echo, Strobe) get no history inside a composition: they show the
    // current frame (documented gap).
    return effect->apply(kit, in);
}

} // namespace kk::render
