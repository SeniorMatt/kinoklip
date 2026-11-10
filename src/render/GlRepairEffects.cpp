// SPDX-License-Identifier: GPL-3.0-or-later
// GlRepairEffects — Deflicker, Color Stabilizer, Automatic Dirt Removal, Dust Buster and Patch
// Replacer (spec 60). The first four read other frames of their clip through
// EffectInput::history (the frames timefx::framesRead() lists, decoded by GlRenderer before
// drawing; a frame that could not be rendered is replaced by the nearest available one, see
// FrameFetch), so the viewer, playback and export draw the same picture for the same frame.
//
// Deflicker and Color Stabilizer measure the mean colour of the frames on the GPU (MeanProbe: a
// strip of texels, no read-back) and apply a gain computed in the shader. Dirt Removal and Dust
// Buster take, per pixel and channel, the median of the neighbouring frames (the mean of the
// middle two of four), mark the pixel as dirt when the frame differs from it by more than the
// Dirt Contrast while the middle two neighbours agree within the Motion Tolerance, grow the
// mark (morphology) and replace the marked pixels by the median.
#include "core/VideoEffectKeys.h"
#include "render/GlTemporalBase.h"
#include "render/TimeEffects.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

double val(const ParamSet& p, const QString& key, int64_t f, double fallback)
{
    return p.value(key, f, fallback);
}

std::vector<int64_t> framesOf(const char* type, const EffectInput& in)
{
    std::vector<int64_t> frames;
    timefx::framesRead(QLatin1String(type), *in.params, in.localFrame, &frames);
    return frames;
}

/// The picture area of a layer (without its transparent margin), in uv.
QRectF contentRect(const EffectInput& in)
{
    const LayerGeometry g = layerGeometry(in);
    return QRectF(QPointF(g.x0, g.y0), QPointF(g.x1, g.y1));
}

// ---- deflicker ----

const char* const kDeflicker = R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;  // strip: mean colour of the current frame (texel 0) and the others
uniform int uCount;
uniform vec4 uP;            // strength, limit (extra gain), per channel, -
void main()
{
    vec4 t = texture(uTex, vUv);
    vec3 cur = texelFetch(uSecond, ivec2(0, 0), 0).rgb;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < uCount; ++i)
        sum += texelFetch(uSecond, ivec2(i, 0), 0).rgb;
    vec3 target = sum / float(uCount);
    float lc = luma(cur), lt = luma(target);
    vec3 gain = vec3(1.0);
    if (lc > 1e-4) {
        gain = uP.z > 0.5 ? target / max(cur, vec3(1e-4)) : vec3(lt / lc);
        float m = 1.0 + uP.y;
        gain = mix(vec3(1.0), clamp(gain, vec3(1.0 / m), vec3(m)), uP.x);
    }
    fragColor = vec4(min(t.rgb * gain, vec3(t.a)), t.a);
}
)";

class Deflicker final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "deflicker", kDeflicker, why, true) || !m_probe.initialize(kit, why))
            return false;
        m_count = uniform(kit, "uCount");
        m_params = uniform(kit, "uP");
        return true;
    }
    void destroy(GlKit& kit) override
    {
        m_probe.destroy(kit);
        PassEffect::destroy(kit);
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Strength, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const std::vector<int64_t> frames = framesOf(fxid::Deflicker, in);
        if (frames.empty())
            return {};
        FrameFetch fetch(in, frames);
        std::vector<const RenderTarget*> layers {in.layer};
        for (size_t i = 0; i < fetch.size(); ++i)
            layers.push_back(fetch.at(i));
        PooledTarget stats = m_probe.measure(kit, layers, contentRect(in));
        const ParamSet& p = *in.params;
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform1i(m_count, int(layers.size()));
        kit.gl()->glUniform4f(m_params,
            float(std::clamp(val(p, k::Strength, in.localFrame, 100) / 100.0, 0.0, 1.0)),
            float(std::clamp(val(p, k::MaxCorrection, in.localFrame, 50) / 100.0, 0.0, 1.0)),
            boolParam(p, k::PerChannel, false) ? 1.f : 0.f, 0.f);
        draw(kit, *in.layer, stats->texture);
        return out;
    }

private:
    MeanProbe m_probe;
    GLint m_count = -1, m_params = -1;
};

// ---- color_stabilizer ----

const char* const kStabilizer = R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;  // strip: mean colour of the current frame (texel 0) and the reference (1)
uniform vec4 uP;            // strength, limit (extra gain), match (0 both, 1 exposure, 2 white balance), -
vec3 meanColor(int i)
{
    vec4 m = texelFetch(uSecond, ivec2(i, 0), 0);
    return m.a > 1e-4 ? m.rgb / m.a : vec3(0.0);
}
void main()
{
    vec4 t = texture(uTex, vUv);
    vec3 cs = meanColor(0), rs = meanColor(1);
    float lc = luma(cs), lr = luma(rs);
    vec3 gain = vec3(1.0);
    if (lc > 1e-4 && lr > 1e-4) {
        vec3 perChannel = rs / max(cs, vec3(1e-4));
        if (uP.z < 0.5)
            gain = perChannel;
        else if (uP.z < 1.5)
            gain = vec3(lr / lc);
        else // colour only: the brightness of the current frame stays
            gain = perChannel * (lc / max(luma(cs * perChannel), 1e-4));
        float m = 1.0 + uP.y;
        gain = mix(vec3(1.0), clamp(gain, vec3(1.0 / m), vec3(m)), uP.x);
    }
    fragColor = vec4(min(t.rgb * gain, vec3(t.a)), t.a);
}
)";

class ColorStabilizer final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "color_stabilizer", kStabilizer, why, true)
            || !m_probe.initialize(kit, why))
            return false;
        m_params = uniform(kit, "uP");
        return true;
    }
    void destroy(GlKit& kit) override
    {
        m_probe.destroy(kit);
        PassEffect::destroy(kit);
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Strength, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const std::vector<int64_t> frames = framesOf(fxid::ColorStabilizer, in);
        if (frames.empty())
            return {}; // this is the reference frame itself
        FrameFetch fetch(in, frames);
        const ParamSet& p = *in.params;
        // The analysed region, as a share of the picture area (Region X / Y move its centre).
        const QRectF content = contentRect(in);
        const double w = std::clamp(val(p, k::RegionWidth, in.localFrame, 100) / 100.0, 0.05, 1.0);
        const double h = std::clamp(val(p, k::RegionHeight, in.localFrame, 100) / 100.0, 0.05, 1.0);
        const double cx
            = content.center().x() + content.width() * val(p, k::RegionX, in.localFrame, 0) / 100.0;
        const double cy = content.center().y()
            + content.height() * val(p, k::RegionY, in.localFrame, 0) / 100.0;
        const QRectF region(cx - w * content.width() / 2, cy - h * content.height() / 2,
            w * content.width(), h * content.height());
        PooledTarget stats = m_probe.measure(kit, {in.layer, fetch.at(0)}, region);
        const QString match = choiceParam(p, k::Match, fxchoice::Both);
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform4f(m_params,
            float(std::clamp(val(p, k::Strength, in.localFrame, 100) / 100.0, 0.0, 1.0)),
            float(std::clamp(val(p, k::MaxCorrection, in.localFrame, 100) / 100.0, 0.0, 3.0)),
            match == fxchoice::Exposure ? 1.f : (match == fxchoice::WhiteBalance ? 2.f : 0.f), 0.f);
        draw(kit, *in.layer, stats->texture);
        return out;
    }

private:
    MeanProbe m_probe;
    GLint m_params = -1;
};

// ---- region shape (Dust Buster, Patch Replacer) ----

const char* const kRegionGlsl = R"(
uniform vec4 uRegion;  // centre uv, half size uv
uniform vec4 uShape;   // feather px, shape (0 rectangle, 1 ellipse), layer size px
// 1 inside the shape, 0 outside; a ramp of the feather width inside the edge.
float regionWeight()
{
    vec2 p = (vUv - uRegion.xy) * uShape.zw;
    vec2 h = max(uRegion.zw * uShape.zw, vec2(0.5));
    float d;
    if (uShape.y > 0.5)
        d = (length(p / h) - 1.0) * min(h.x, h.y);
    else {
        vec2 q = abs(p) - h;
        d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0);
    }
    return uShape.x > 0.5 ? clamp(-d / uShape.x, 0.0, 1.0) : clamp(0.5 - d, 0.0, 1.0);
}
)";

/// Sets uRegion / uShape from the Shape, Width, Height and Feather parameters, the shape being
/// centred on (@p cx, @p cy) timeline px from the middle of the picture.
void setRegion(GlKit& kit, GLint region, GLint shape, const EffectInput& in, double cx, double cy)
{
    const ParamSet& p = *in.params;
    const LayerGeometry g = layerGeometry(in);
    float u = 0, v = 0;
    g.centre(in, cx, cy, &u, &v);
    const double w = std::max(1.0, val(p, k::Width, in.localFrame, 100));
    const double h = std::max(1.0, val(p, k::Height, in.localFrame, 100));
    kit.gl()->glUniform4f(
        region, u, v, float(0.5 * w * in.scale / g.width), float(0.5 * h * in.scale / g.height));
    const bool ellipse
        = choiceParam(p, k::Shape, QStringLiteral("rectangle")) == QLatin1String("ellipse");
    kit.gl()->glUniform4f(shape,
        float(std::max(0.0, val(p, k::Feather, in.localFrame, 0)) * in.scale), ellipse ? 1.f : 0.f,
        g.width, g.height);
}

// ---- automatic_dirt_removal, dust_buster ----

// The temporal consensus of a pixel: per channel the median of the neighbouring frames (the mean
// of the middle two of four; with two neighbours they are doubled), and how far the middle two
// are apart (the neighbours disagree where something moves).
const char* const kConsensus = R"(
uniform sampler2D uTex;
uniform sampler2D uN0;
uniform sampler2D uN1;
uniform sampler2D uN2;
uniform sampler2D uN3;
vec4 sorted4(vec4 v)
{
    float a = v.x, b = v.y, c = v.z, d = v.w, t;
    if (a > b) { t = a; a = b; b = t; }
    if (c > d) { t = c; c = d; d = t; }
    if (a > c) { t = a; a = c; c = t; }
    if (b > d) { t = b; b = d; d = t; }
    if (b > c) { t = b; b = c; c = t; }
    return vec4(a, b, c, d);
}
void consensus(out vec4 median, out float spread)
{
    vec4 a = texture(uN0, vUv), b = texture(uN1, vUv), c = texture(uN2, vUv), d = texture(uN3, vUv);
    vec4 r = sorted4(vec4(a.r, b.r, c.r, d.r));
    vec4 g = sorted4(vec4(a.g, b.g, c.g, d.g));
    vec4 bl = sorted4(vec4(a.b, b.b, c.b, d.b));
    vec4 al = sorted4(vec4(a.a, b.a, c.a, d.a));
    median = 0.5 * vec4(r.y + r.z, g.y + g.z, bl.y + bl.z, al.y + al.z);
    spread = max(max(r.z - r.y, g.z - g.y), max(bl.z - bl.y, al.z - al.y));
}
)";

const char* const kDirtDetect = R"(
uniform vec2 uLimits;  // dirt contrast, motion tolerance
void main()
{
    vec4 c = texture(uTex, vUv);
    vec4 median;
    float spread;
    consensus(median, spread);
    vec4 d = abs(c - median);
    float contrast = max(max(d.r, d.g), max(d.b, d.a));
    fragColor = vec4((contrast > uLimits.x && spread < uLimits.y) ? 1.0 : 0.0);
}
)";

const char* const kDirtApply = R"(
uniform sampler2D uMask;
uniform vec4 uP;   // strength, whole region (1) or detected dirt (0), limited to the region, -
void main()
{
    vec4 c = texture(uTex, vUv);
    vec4 median;
    float spread;
    consensus(median, spread);
    float mask = uP.y > 0.5 ? 1.0 : texture(uMask, vUv).r;
    float inside = uP.z > 0.5 ? regionWeight() : 1.0;
    fragColor = mix(c, median, mask * inside * uP.x);
}
)";

class DirtEffect final : public PassEffect
{
public:
    explicit DirtEffect(bool region)
        : m_region(region)
    { }
    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string detect = std::string(kConsensus) + kDirtDetect;
        const std::string apply = std::string(kConsensus) + kRegionGlsl + kDirtApply;
        GlFunctions* gl = kit.gl();
        if (!build(kit, m_region ? "dust_buster" : "automatic_dirt_removal", apply.c_str(), why)
            || !m_detect.build(gl, "dirt_detect", detect.c_str(), why))
            return false;
        for (GlProgram* program : {&m_program, &m_detect}) {
            gl->glUseProgram(program->id);
            gl->glUniform1i(program->uniform(gl, "uTex"), 0);
            for (int i = 0; i < 4; ++i)
                gl->glUniform1i(program->uniform(gl, ("uN" + std::to_string(i)).c_str()), 1 + i);
        }
        gl->glUseProgram(m_program.id);
        gl->glUniform1i(m_program.uniform(gl, "uMask"), 5);
        gl->glUseProgram(0);
        m_limits = m_detect.uniform(gl, "uLimits");
        m_params = uniform(kit, "uP");
        m_regionLoc = uniform(kit, "uRegion");
        m_shapeLoc = uniform(kit, "uShape");
        return true;
    }
    void destroy(GlKit& kit) override
    {
        m_detect.destroy(kit.gl());
        PassEffect::destroy(kit);
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const std::vector<int64_t> frames
            = framesOf(m_region ? fxid::DustBuster : fxid::DirtRemoval, in);
        if (frames.size() < 2)
            return {};
        FrameFetch fetch(in, frames); // previous, next, then the two a frame further out
        const RenderTarget* n[4] = {fetch.at(0), fetch.at(1), fetch.at(frames.size() > 3 ? 2 : 0),
            fetch.at(frames.size() > 3 ? 3 : 1)};
        const ParamSet& p = *in.params;
        GlFunctions* gl = kit.gl();
        const bool wholeRegion = m_region
            && choiceParam(p, k::RegionMode, fxchoice::DetectedDirt) == fxchoice::WholeRegion;

        PooledTarget mask;
        if (!wholeRegion) {
            mask = kit.acquire(in.layer->size, in.layer->format);
            kit.bindTarget(*mask);
            kit.setBlendOver(false);
            gl->glUseProgram(m_detect.id);
            kit.setFullscreen(m_detect);
            gl->glUniform2f(m_limits,
                float(std::clamp(val(p, k::Threshold, in.localFrame, 15) / 100.0, 0.0, 1.0)),
                float(std::clamp(val(p, k::MotionThreshold, in.localFrame, 8) / 100.0, 0.0, 1.0)));
            bindFrames(kit, *in.layer, n);
            kit.drawQuad();
            if (PooledTarget grown
                = kit.morphology(*mask, val(p, k::Dilate, in.localFrame, 1) * in.scale, true))
                mask = std::move(grown);
        }

        PooledTarget out = begin(kit, *in.layer);
        gl->glUniform4f(m_params,
            float(std::clamp(val(p, k::Amount, in.localFrame, 100) / 100.0, 0.0, 1.0)),
            wholeRegion ? 1.f : 0.f, m_region ? 1.f : 0.f, 0.f);
        if (m_region)
            setRegion(kit, m_regionLoc, m_shapeLoc, in, val(p, k::CenterX, in.localFrame, 0),
                val(p, k::CenterY, in.localFrame, 0));
        bindFrames(kit, *in.layer, n);
        kit.bindTexture(5, mask ? mask->texture : kit.transparentTexture());
        kit.drawQuad();
        return out;
    }

private:
    static void bindFrames(GlKit& kit, const RenderTarget& layer, const RenderTarget* const n[4])
    {
        kit.bindTexture(0, layer.texture);
        for (int i = 0; i < 4; ++i)
            kit.bindTexture(1 + i, n[i]->texture);
    }

    bool m_region;
    GlProgram m_detect;
    GLint m_limits = -1, m_params = -1, m_regionLoc = -1, m_shapeLoc = -1;
};

// ---- patch_replacer ----

const char* const kPatch = R"(
uniform sampler2D uTex;
uniform vec3 uShift;   // source minus target in uv, blend
void main()
{
    vec4 t = texture(uTex, vUv);
    vec4 patch = texture(uTex, vUv + uShift.xy);
    fragColor = mix(t, patch, regionWeight() * uShift.z);
}
)";

class PatchReplacer final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = std::string(kRegionGlsl) + kPatch;
        if (!build(kit, "patch_replacer", source.c_str(), why))
            return false;
        m_shift = uniform(kit, "uShift");
        m_regionLoc = uniform(kit, "uRegion");
        m_shapeLoc = uniform(kit, "uShape");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Opacity, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const double tx = val(p, k::TargetX, in.localFrame, 0),
                     ty = val(p, k::TargetY, in.localFrame, 0);
        const double sx = val(p, k::SourceX, in.localFrame, -250),
                     sy = val(p, k::SourceY, in.localFrame, 0);
        const LayerGeometry g = layerGeometry(in);
        PooledTarget out = begin(kit, *in.layer);
        setRegion(kit, m_regionLoc, m_shapeLoc, in, tx, ty);
        kit.gl()->glUniform3f(m_shift, float((sx - tx) * in.scale / g.width),
            float((sy - ty) * in.scale / g.height),
            float(std::clamp(val(p, k::Opacity, in.localFrame, 100) / 100.0, 0.0, 1.0)));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_shift = -1, m_regionLoc = -1, m_shapeLoc = -1;
};

const GlEffectFactory kTable[] = {
    {fxid::Deflicker, &make<Deflicker>},
    {fxid::ColorStabilizer, &make<ColorStabilizer>},
    {fxid::DirtRemoval, [] { return std::unique_ptr<GlEffect>(new DirtEffect(false)); }},
    {fxid::DustBuster, [] { return std::unique_ptr<GlEffect>(new DirtEffect(true)); }},
    {fxid::PatchReplacer, &make<PatchReplacer>},
};

} // namespace

std::span<const GlEffectFactory> repairGlEffects()
{
    return kTable;
}

} // namespace kk::render
