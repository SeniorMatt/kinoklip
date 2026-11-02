// SPDX-License-Identifier: GPL-3.0-or-later
// GlMasks — analytic rectangle / ellipse mattes, triangle-fan path coverage with Gaussian
// feather, and the combine pass (modes, invert, opacity) that multiplies the clip layer.
#include "render/GlMasks.h"

#include "core/Mask.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

// Up to 8 masks per pass (uniform arrays), coverage textures for point paths in uCov0..3
// (GLSL 3.30 cannot index sampler arrays dynamically, hence the explicit selection).
// gl_FragCoord.xy is the layer pixel (row 0 = top, see GlKit's orientation convention).
const char* const kCombineFragment = R"(
uniform sampler2D uLayer;
uniform sampler2D uPrev;
uniform sampler2D uCov0;
uniform sampler2D uCov1;
uniform sampler2D uCov2;
uniform sampler2D uCov3;
uniform int uCount;
uniform int uType[8];      // 0 rectangle, 1 ellipse, 2 coverage texture
uniform int uMode[8];      // MaskMode: 0 add, 1 subtract, 2 intersect, 3 difference
uniform int uInvert[8];
uniform float uOpacity[8];
uniform vec4 uGeom[8];     // centre (layer px), half size (layer px)
uniform vec2 uRot[8];      // cos, sin of the clockwise rotation
uniform vec3 uSoft[8];     // feather inside, feather outside, expand (layer px)
uniform int uCovIndex[8];
uniform int uHasPrev;      // matte of the previous chunk in uPrev.r
uniform float uStart;      // initial matte without a previous chunk
uniform int uApply;        // 1: write layer * matte, 0: write the matte

float sdRect(vec2 q, vec2 h)
{
    vec2 d = abs(q) - h;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

// First-order distance estimate f / |grad f| of f = |q / h| - 1, scaled by |q / h|: exact for
// circles and on the axes, within a few percent elsewhere for moderate aspect ratios.
float sdEllipse(vec2 q, vec2 h)
{
    h = max(h, vec2(1e-3));
    float k1 = length(q / h);
    float k2 = length(q / (h * h));
    if (k2 < 1e-6)
        return -min(h.x, h.y);
    return k1 * (k1 - 1.0) / k2;
}

float coverage(int i, vec2 p)
{
    if (uType[i] == 2) {
        int c = uCovIndex[i];
        vec4 t = c == 0 ? texture(uCov0, vUv)
               : c == 1 ? texture(uCov1, vUv)
               : c == 2 ? texture(uCov2, vUv)
                        : texture(uCov3, vUv);
        return clamp(t.r, 0.0, 1.0);
    }
    vec2 d = p - uGeom[i].xy;
    vec2 cs = uRot[i];
    vec2 q = vec2(cs.x * d.x + cs.y * d.y, -cs.y * d.x + cs.x * d.y);
    float sd = uType[i] == 0 ? sdRect(q, uGeom[i].zw) : sdEllipse(q, uGeom[i].zw);
    sd -= uSoft[i].z;
    float lo = -uSoft[i].x;
    float hi = uSoft[i].y;
    if (hi - lo < 1.0) { // at least one pixel wide: the antialiasing of a hard edge
        float mid = 0.5 * (lo + hi);
        lo = mid - 0.5;
        hi = mid + 0.5;
    }
    return 1.0 - smoothstep(lo, hi, sd);
}

void main()
{
    vec2 p = gl_FragCoord.xy;
    float m = uHasPrev != 0 ? texture(uPrev, vUv).r : uStart;
    for (int i = 0; i < 8; ++i) {
        if (i >= uCount)
            break;
        float c = coverage(i, p);
        if (uInvert[i] != 0)
            c = 1.0 - c;
        c *= uOpacity[i];
        int mode = uMode[i];
        if (mode == 0)
            m = m + c - m * c;
        else if (mode == 1)
            m = m * (1.0 - c);
        else if (mode == 2)
            m = m * c;
        else
            m = abs(m - c);
    }
    fragColor = uApply != 0 ? texture(uLayer, vUv) * m : vec4(m);
}
)";

// The fan writes 1; the blend function ONE_MINUS_DST_COLOR / ZERO turns that into
// dst = 1 - dst, so every pixel ends up toggled once per covering fan triangle: even-odd fill.
const char* const kFanFragment = R"(
void main()
{
    fragColor = vec4(1.0);
}
)";

/// Path flattening tolerance in layer pixels (chord error of the bezier subdivision).
constexpr double kFlattenTolerance = 0.25;
/// Smallest blur of a path coverage: antialiases its edges (pixels).
constexpr double kMinPathSigma = 0.5;
/// Coverage targets are 8-bit (256 levels are plenty for a matte) to halve the bandwidth.
constexpr GLenum kCoverageFormat = GL_RGBA8;
/// Path coverage resolution reduction: at most 1/8, keeping the blur >= 1.5 px there.
constexpr int kMaxCoverageReduction = 8;
constexpr double kMinReducedSigma = 1.5;
/// Feather width → Gaussian sigma: the smoothstep over a width W has slope 1.5 / W at its
/// centre, a Gaussian edge 1 / (σ √(2π)); equal slopes give σ = W / 3.76.
constexpr double kFeatherToSigma = 1.0 / 3.76;

} // namespace

bool GlMasks::initialize(GlKit& kit, QString* why)
{
    GlFunctions* gl = kit.gl();
    if (!m_combine.build(gl, "mask_combine", kCombineFragment, why)
        || !m_fan.build(gl, "mask_fan", kFanFragment, why))
        return false;
    m_uCount = m_combine.uniform(gl, "uCount");
    m_uType = m_combine.uniform(gl, "uType");
    m_uMode = m_combine.uniform(gl, "uMode");
    m_uInvert = m_combine.uniform(gl, "uInvert");
    m_uOpacity = m_combine.uniform(gl, "uOpacity");
    m_uGeom = m_combine.uniform(gl, "uGeom");
    m_uRot = m_combine.uniform(gl, "uRot");
    m_uSoft = m_combine.uniform(gl, "uSoft");
    m_uCovIndex = m_combine.uniform(gl, "uCovIndex");
    m_uHasPrev = m_combine.uniform(gl, "uHasPrev");
    m_uStart = m_combine.uniform(gl, "uStart");
    m_uApply = m_combine.uniform(gl, "uApply");
    gl->glUseProgram(m_combine.id);
    gl->glUniform1i(m_combine.uniform(gl, "uLayer"), 0);
    gl->glUniform1i(m_combine.uniform(gl, "uPrev"), 1);
    gl->glUniform1i(m_combine.uniform(gl, "uCov0"), 2);
    gl->glUniform1i(m_combine.uniform(gl, "uCov1"), 3);
    gl->glUniform1i(m_combine.uniform(gl, "uCov2"), 4);
    gl->glUniform1i(m_combine.uniform(gl, "uCov3"), 5);
    gl->glUseProgram(0);

    // Own VAO for the fan vertices; GlKit's quad VAO is otherwise bound for the context's
    // lifetime, so remember it and rebind it after each fan.
    gl->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_quadVao);
    gl->glGenVertexArrays(1, &m_fanVao);
    gl->glBindVertexArray(m_fanVao);
    gl->glGenBuffers(1, &m_fanVbo);
    gl->glBindBuffer(GL_ARRAY_BUFFER, m_fanVbo);
    gl->glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STREAM_DRAW);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    gl->glBindVertexArray(GLuint(m_quadVao));

    m_masks.reserve(8);
    m_items.reserve(kChunk);
    m_coverage.reserve(kMaxPathsPerChunk);
    return true;
}

void GlMasks::destroy(GlKit& kit)
{
    GlFunctions* gl = kit.gl();
    m_coverage.clear();
    m_combine.destroy(gl);
    m_fan.destroy(gl);
    if (m_fanVbo)
        gl->glDeleteBuffers(1, &m_fanVbo);
    if (m_fanVao)
        gl->glDeleteVertexArrays(1, &m_fanVao);
    m_fanVbo = m_fanVao = 0;
}

bool GlMasks::hasActiveMasks(const ClipData& clip)
{
    return std::any_of(
        clip.masks.cbegin(), clip.masks.cend(), [](const MaskData& m) { return m.isRenderable(); });
}

PooledTarget GlMasks::pathCoverage(
    GlKit& kit, QSize size, const MaskLayerGeometry& g, const MaskData& mask, int64_t localFrame)
{
    GlFunctions* gl = kit.gl();
    const double s = g.scale;
    const double featherIn = std::max(0.0, mask.value(maskkeys::FeatherIn, localFrame)) * s;
    const double featherOut = std::max(0.0, mask.value(maskkeys::FeatherOut, localFrame)) * s;
    const double expand = mask.value(maskkeys::Expand, localFrame) * s;
    const double sigma = std::max(kMinPathSigma, (featherIn + featherOut) * kFeatherToSigma);

    // A feathered coverage is smooth, so it is rasterised and blurred at a reduced resolution
    // (2^k smaller while the blur stays >= kMinReducedSigma px there) and the combine pass
    // upsamples it for free with bilinear filtering (it samples by uv). Measured on llvmpipe,
    // 1080p, 2 analytic + 2 bezier masks with 30 px feathers: +243 ms at full resolution,
    // +65 ms with the reduction (test_maskrender timing1080p).
    int reduce = 1;
    while (reduce < kMaxCoverageReduction && sigma / (reduce * 2) >= kMinReducedSigma)
        reduce *= 2;
    const QSize covSize(std::max(1, (size.width() + reduce - 1) / reduce),
        std::max(1, (size.height() + reduce - 1) / reduce));
    const double fx = double(covSize.width()) / size.width();
    const double fy = double(covSize.height()) / size.height();

    PooledTarget cov = kit.acquire(covSize, kCoverageFormat);
    kit.bindTarget(*cov);
    kit.clearTransparent();

    // Mask-local → content → layer pixels as one affine map, evaluated once per mask (the
    // parameter lookups are not repeated per vertex).
    const double sx = mask.value(maskkeys::ScaleX, localFrame) * s;
    const double sy = mask.value(maskkeys::ScaleY, localFrame) * s;
    const double rad = mask.value(maskkeys::Rotation, localFrame) * std::numbers::pi / 180.0;
    const double c = std::cos(rad);
    const double sn = std::sin(rad);
    const double ox
        = g.padPx + (mask.value(maskkeys::PosX, localFrame) + g.contentSize.width() / 2) * s;
    const double oy
        = g.padPx + (mask.value(maskkeys::PosY, localFrame) + g.contentSize.height() / 2) * s;
    auto toLayer = [=](QPointF p) {
        const double x = p.x() * sx;
        const double y = p.y() * sy;
        return QPointF(ox + c * x - sn * y, oy + sn * x + c * y);
    };
    mask.path.evaluate(double(localFrame), m_path);
    flattenPath(m_path, true, kFlattenTolerance, toLayer, m_polygon);
    if (m_polygon.size() < 3)
        return cov; // nothing covered

    // The blurred edge is centred where the polygon is, so move the polygon to the middle of
    // the feather ramp [-in, +out] (plus the expansion).
    offsetPolygon(m_polygon, expand + (featherOut - featherIn) * 0.5);

    m_vertices.resize(size_t(m_polygon.size()) * 2);
    for (qsizetype i = 0; i < m_polygon.size(); ++i) {
        m_vertices[size_t(i) * 2] = float(m_polygon[i].x() * fx);
        m_vertices[size_t(i) * 2 + 1] = float(m_polygon[i].y() * fy);
    }
    gl->glUseProgram(m_fan.id);
    kit.setMatrix(m_fan.posMatrix, GlKit::pixelToNdc(covSize));
    kit.setMatrix(m_fan.uvMatrix, QTransform());
    gl->glEnable(GL_BLEND);
    gl->glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ZERO);
    gl->glBindVertexArray(m_fanVao);
    gl->glBindBuffer(GL_ARRAY_BUFFER, m_fanVbo);
    gl->glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(m_vertices.size() * sizeof(float)),
        m_vertices.data(), GL_STREAM_DRAW);
    gl->glDrawArrays(GL_TRIANGLE_FAN, 0, GLsizei(m_polygon.size()));
    gl->glBindVertexArray(GLuint(m_quadVao));
    gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // GlKit's default
    gl->glDisable(GL_BLEND);

    PooledTarget blurred = kit.gaussianBlur(*cov, sigma * fx);
    return blurred ? std::move(blurred) : std::move(cov);
}

PooledTarget GlMasks::apply(GlKit& kit, const RenderTarget& layer, const MaskLayerGeometry& g,
    const ClipData& clip, int64_t localFrame)
{
    m_masks.clear();
    for (const MaskData& m : clip.masks) {
        if (m.isRenderable())
            m_masks.push_back(&m);
    }
    if (m_masks.empty())
        return {};
    GlFunctions* gl = kit.gl();
    const MaskMode firstMode = m_masks.front()->mode;
    // An empty matte for add / difference, a full one for subtract / intersect (core/Mask.h).
    const float start
        = (firstMode == MaskMode::Add || firstMode == MaskMode::Difference) ? 0.0f : 1.0f;
    const double s = g.scale;

    PooledTarget prev;
    size_t i = 0;
    while (i < m_masks.size()) {
        m_items.clear();
        m_coverage.clear();
        size_t j = i;
        while (j < m_masks.size() && m_items.size() < size_t(kChunk)) {
            const MaskData& m = *m_masks[j];
            if (m.isPathShape() && m_coverage.size() == size_t(kMaxPathsPerChunk))
                break;
            Item it;
            it.mode = int(m.mode);
            it.invert = m.invert ? 1 : 0;
            it.opacity
                = float(std::clamp(m.value(maskkeys::Opacity, localFrame) / 100.0, 0.0, 1.0));
            if (m.isPathShape()) {
                it.type = 2;
                it.coverage = int(m_coverage.size());
                m_coverage.push_back(pathCoverage(kit, layer.size, g, m, localFrame));
            } else {
                it.type = m.shape == MaskShape::Ellipse ? 1 : 0;
                const double rad
                    = m.value(maskkeys::Rotation, localFrame) * std::numbers::pi / 180.0;
                it.geom[0] = float(g.padPx
                    + (m.value(maskkeys::PosX, localFrame) + g.contentSize.width() / 2) * s);
                it.geom[1] = float(g.padPx
                    + (m.value(maskkeys::PosY, localFrame) + g.contentSize.height() / 2) * s);
                it.geom[2] = float(std::abs(m.value(maskkeys::ScaleX, localFrame))
                    * std::max(0.0, m.value(maskkeys::Width, localFrame)) * 0.5 * s);
                it.geom[3] = float(std::abs(m.value(maskkeys::ScaleY, localFrame))
                    * std::max(0.0, m.value(maskkeys::Height, localFrame)) * 0.5 * s);
                it.rot[0] = float(std::cos(rad));
                it.rot[1] = float(std::sin(rad));
                it.soft[0] = float(std::max(0.0, m.value(maskkeys::FeatherIn, localFrame)) * s);
                it.soft[1] = float(std::max(0.0, m.value(maskkeys::FeatherOut, localFrame)) * s);
                it.soft[2] = float(m.value(maskkeys::Expand, localFrame) * s);
            }
            m_items.push_back(it);
            ++j;
        }

        // Uniform arrays of this chunk.
        GLint type[kChunk] = {}, mode[kChunk] = {}, invert[kChunk] = {}, covIndex[kChunk] = {};
        GLfloat opacity[kChunk] = {}, geom[kChunk * 4] = {}, rot[kChunk * 2] = {},
                soft[kChunk * 3] = {};
        const int count = int(m_items.size());
        for (int k = 0; k < count; ++k) {
            const Item& it = m_items[size_t(k)];
            type[k] = it.type;
            mode[k] = it.mode;
            invert[k] = it.invert;
            covIndex[k] = std::max(0, it.coverage);
            opacity[k] = it.opacity;
            std::copy(it.geom, it.geom + 4, geom + k * 4);
            std::copy(it.rot, it.rot + 2, rot + k * 2);
            std::copy(it.soft, it.soft + 3, soft + k * 3);
        }
        const bool last = j == m_masks.size();
        PooledTarget out = kit.acquire(layer.size);
        kit.bindTarget(*out);
        kit.setBlendOver(false);
        gl->glUseProgram(m_combine.id);
        kit.setFullscreen(m_combine);
        gl->glUniform1i(m_uCount, count);
        gl->glUniform1iv(m_uType, kChunk, type);
        gl->glUniform1iv(m_uMode, kChunk, mode);
        gl->glUniform1iv(m_uInvert, kChunk, invert);
        gl->glUniform1iv(m_uCovIndex, kChunk, covIndex);
        gl->glUniform1fv(m_uOpacity, kChunk, opacity);
        gl->glUniform4fv(m_uGeom, kChunk, geom);
        gl->glUniform2fv(m_uRot, kChunk, rot);
        gl->glUniform3fv(m_uSoft, kChunk, soft);
        gl->glUniform1i(m_uHasPrev, prev ? 1 : 0);
        gl->glUniform1f(m_uStart, start);
        gl->glUniform1i(m_uApply, last ? 1 : 0);
        kit.bindTexture(0, layer.texture);
        kit.bindTexture(1, prev ? prev->texture : kit.transparentTexture());
        for (int k = 0; k < kMaxPathsPerChunk; ++k) {
            kit.bindTexture(2 + k,
                size_t(k) < m_coverage.size() ? m_coverage[size_t(k)]->texture
                                              : kit.transparentTexture());
        }
        kit.drawQuad();
        gl->glActiveTexture(GL_TEXTURE0);
        prev = std::move(out);
        i = j;
    }
    m_coverage.clear();
    return prev;
}

} // namespace kk::render
