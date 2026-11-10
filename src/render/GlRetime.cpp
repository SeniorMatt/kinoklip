// SPDX-License-Identifier: GPL-3.0-or-later
// GlRetime implementation: frame blend and the coarse-to-fine symmetric block-matching flow
// (algorithm and costs in GlRetime.h).
#include "render/GlRetime.h"

#include <algorithm>
#include <vector>

namespace kk::render {

namespace {

const char* const kBlend = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uW;
void main() { fragColor = mix(texture(uA, vUv), texture(uB, vUv), uW); }
)";

// Rec. 709 luma of the premultiplied picture (alpha ignored: video is opaque).
const char* const kLuma = R"(
uniform sampler2D uSrc;
void main()
{
    float y = dot(texture(uSrc, vUv).rgb, vec3(0.2126, 0.7152, 0.0722));
    fragColor = vec4(y, y, y, 1.0);
}
)";

// One pyramid level of the symmetric search. rg = motion A -> B in pixels of this level,
// b = matching cost. Offsets are tested around the prediction from the coarser level.
const char* const kSearch = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform sampler2D uPrev;
uniform vec2 uTexel;      // 1 / level size
uniform float uW;         // time of the in-between frame (0 = A, 1 = B)
uniform float uHasPrev;   // 1 when uPrev holds the coarser level
uniform vec2 uPrevScale;  // coarser level px -> this level px
uniform float uSubpixel;  // 1 on level 0: parabolic sub-pixel refinement
const int kRadius = 2;
const float kLambda = 0.02; // cost per pixel of deviation from the prediction (smoothness)

float cost(vec2 v)
{
    vec2 pa = vUv - uW * v * uTexel;
    vec2 pb = vUv + (1.0 - uW) * v * uTexel;
    float s = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 o = vec2(float(x), float(y)) * uTexel;
            s += abs(texture(uA, pa + o).r - texture(uB, pb + o).r);
        }
    }
    return s;
}

void main()
{
    vec2 pred = uHasPrev > 0.5 ? texture(uPrev, vUv).rg * uPrevScale : vec2(0.0);
    vec2 best = pred;
    float bestCost = cost(pred);
    // Zero motion as an extra candidate: static areas recover from a wrong coarse estimate.
    float still = cost(vec2(0.0)) + kLambda * length(pred);
    if (still < bestCost) {
        bestCost = still;
        best = vec2(0.0);
    }
    vec2 centre = best;
    for (int dy = -kRadius; dy <= kRadius; ++dy) {
        for (int dx = -kRadius; dx <= kRadius; ++dx) {
            if (dx == 0 && dy == 0)
                continue;
            vec2 d = vec2(float(dx), float(dy));
            float c = cost(centre + d) + kLambda * length(d);
            if (c < bestCost) {
                bestCost = c;
                best = centre + d;
            }
        }
    }
    if (uSubpixel > 0.5) {
        float c0 = cost(best);
        float xm = cost(best - vec2(1.0, 0.0));
        float xp = cost(best + vec2(1.0, 0.0));
        float ym = cost(best - vec2(0.0, 1.0));
        float yp = cost(best + vec2(0.0, 1.0));
        float dxx = xm - 2.0 * c0 + xp;
        float dyy = ym - 2.0 * c0 + yp;
        if (dxx > 1e-4)
            best.x += clamp(0.5 * (xm - xp) / dxx, -0.5, 0.5);
        if (dyy > 1e-4)
            best.y += clamp(0.5 * (ym - yp) / dyy, -0.5, 0.5);
        bestCost = c0;
    }
    fragColor = vec4(best, bestCost, 1.0);
}
)";

// Cost- and similarity-weighted 3x3 average of the motion: removes isolated mismatches without
// blurring across motion edges much.
const char* const kSmooth = R"(
uniform sampler2D uFlow;
uniform vec2 uTexel;
void main()
{
    vec3 c = texture(uFlow, vUv).rgb;
    vec2 sum = vec2(0.0);
    float weight = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 n = texture(uFlow, vUv + vec2(float(x), float(y)) * uTexel).rgb;
            vec2 d = n.rg - c.rg;
            float w = exp(-dot(d, d) * 0.125) / (n.b + 0.05);
            sum += n.rg * w;
            weight += w;
        }
    }
    fragColor = vec4(sum / weight, c.b, 1.0);
}
)";

// Full-resolution in-between frame along the motion; fades to the plain blend where the two
// samples disagree (occlusion).
const char* const kWarp = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform sampler2D uFlow;
uniform vec2 uScale; // level-0 px -> uv
uniform float uW;
void main()
{
    vec2 v = texture(uFlow, vUv).rg * uScale;
    vec4 a = texture(uA, vUv - uW * v);
    vec4 b = texture(uB, vUv + (1.0 - uW) * v);
    vec4 moved = mix(a, b, uW);
    vec4 plain = mix(texture(uA, vUv), texture(uB, vUv), uW);
    vec3 e = abs(a.rgb - b.rgb);
    float err = max(e.r, max(e.g, e.b));
    fragColor = mix(plain, moved, 1.0 - smoothstep(0.08, 0.3, err));
}
)";

} // namespace

bool GlRetime::initialize(GlKit& kit, QString* why)
{
    GlFunctions* gl = kit.gl();
    if (!m_blend.build(gl, "retime_blend", kBlend, why)
        || !m_luma.build(gl, "retime_luma", kLuma, why)
        || !m_search.build(gl, "retime_search", kSearch, why)
        || !m_smooth.build(gl, "retime_smooth", kSmooth, why)
        || !m_warp.build(gl, "retime_warp", kWarp, why))
        return false;
    gl->glUseProgram(m_blend.id);
    gl->glUniform1i(m_blend.uniform(gl, "uA"), 0);
    gl->glUniform1i(m_blend.uniform(gl, "uB"), 1);
    m_blendW = m_blend.uniform(gl, "uW");
    gl->glUseProgram(m_luma.id);
    gl->glUniform1i(m_luma.uniform(gl, "uSrc"), 0);
    gl->glUseProgram(m_search.id);
    gl->glUniform1i(m_search.uniform(gl, "uA"), 0);
    gl->glUniform1i(m_search.uniform(gl, "uB"), 1);
    gl->glUniform1i(m_search.uniform(gl, "uPrev"), 2);
    m_searchW = m_search.uniform(gl, "uW");
    m_searchTexel = m_search.uniform(gl, "uTexel");
    m_searchHasPrev = m_search.uniform(gl, "uHasPrev");
    m_searchPrevScale = m_search.uniform(gl, "uPrevScale");
    m_searchSubpixel = m_search.uniform(gl, "uSubpixel");
    gl->glUseProgram(m_smooth.id);
    gl->glUniform1i(m_smooth.uniform(gl, "uFlow"), 0);
    m_smoothTexel = m_smooth.uniform(gl, "uTexel");
    gl->glUseProgram(m_warp.id);
    gl->glUniform1i(m_warp.uniform(gl, "uA"), 0);
    gl->glUniform1i(m_warp.uniform(gl, "uB"), 1);
    gl->glUniform1i(m_warp.uniform(gl, "uFlow"), 2);
    m_warpW = m_warp.uniform(gl, "uW");
    m_warpScale = m_warp.uniform(gl, "uScale");
    gl->glUseProgram(0);
    return true;
}

void GlRetime::destroy(GlKit& kit)
{
    for (GlProgram* p : {&m_blend, &m_luma, &m_search, &m_smooth, &m_warp})
        p->destroy(kit.gl());
}

PooledTarget GlRetime::blend(GlKit& kit, const RenderTarget& a, const RenderTarget& b, float w)
{
    GlFunctions* gl = kit.gl();
    PooledTarget out = kit.acquire(a.size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    gl->glUseProgram(m_blend.id);
    kit.setFullscreen(m_blend);
    gl->glUniform1f(m_blendW, w);
    kit.bindTexture(0, a.texture);
    kit.bindTexture(1, b.texture);
    kit.drawQuad();
    return out;
}

PooledTarget GlRetime::luma(GlKit& kit, const RenderTarget& src, QSize size)
{
    // Halve with bilinear taps (= exact 2x2 box) until one more step reaches the level size,
    // so the luma level is never an aliased point sample of a much larger picture.
    PooledTarget scaled;
    const RenderTarget* from = &src;
    while (from->size.width() >= 2 * size.width() && from->size.height() >= 2 * size.height()) {
        PooledTarget half = kit.resample(*from, from->size / 2);
        scaled = std::move(half);
        from = scaled.get();
    }
    GlFunctions* gl = kit.gl();
    PooledTarget out = kit.acquire(size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    gl->glUseProgram(m_luma.id);
    kit.setFullscreen(m_luma);
    kit.bindTexture(0, from->texture);
    kit.drawQuad();
    return out;
}

PooledTarget GlRetime::estimateFlow(
    GlKit& kit, const RenderTarget& a, const RenderTarget& b, float w)
{
    GlFunctions* gl = kit.gl();
    // ---- Pyramid ----
    QSize level0 = a.size;
    if (level0.width() > kFlowMaxWidth)
        level0 = QSize(kFlowMaxWidth,
            std::max(1, int(double(a.size.height()) * kFlowMaxWidth / a.size.width() + 0.5)));
    std::vector<PooledTarget> pyrA, pyrB;
    pyrA.push_back(luma(kit, a, level0));
    pyrB.push_back(luma(kit, b, level0));
    while (int(pyrA.size()) < kMaxLevels) {
        const QSize s = pyrA.back()->size;
        if (s.width() / 2 < kFlowMinEdge || s.height() / 2 < kFlowMinEdge)
            break;
        PooledTarget na = kit.resample(*pyrA.back(), s / 2);
        PooledTarget nb = kit.resample(*pyrB.back(), s / 2);
        pyrA.push_back(std::move(na));
        pyrB.push_back(std::move(nb));
    }

    // ---- Coarse to fine search ----
    PooledTarget flow;
    for (int level = int(pyrA.size()) - 1; level >= 0; --level) {
        const QSize size = pyrA[size_t(level)]->size;
        PooledTarget raw = kit.acquire(size);
        kit.bindTarget(*raw);
        kit.setBlendOver(false);
        gl->glUseProgram(m_search.id);
        kit.setFullscreen(m_search);
        gl->glUniform1f(m_searchW, w);
        gl->glUniform2f(m_searchTexel, 1.0f / float(size.width()), 1.0f / float(size.height()));
        gl->glUniform1f(m_searchHasPrev, flow ? 1.0f : 0.0f);
        if (flow) {
            gl->glUniform2f(m_searchPrevScale, float(size.width()) / float(flow->size.width()),
                float(size.height()) / float(flow->size.height()));
        } else {
            gl->glUniform2f(m_searchPrevScale, 1.0f, 1.0f);
        }
        gl->glUniform1f(m_searchSubpixel, level == 0 ? 1.0f : 0.0f);
        kit.bindTexture(0, pyrA[size_t(level)]->texture);
        kit.bindTexture(1, pyrB[size_t(level)]->texture);
        kit.bindTexture(2, flow ? flow->texture : kit.transparentTexture());
        kit.drawQuad();

        PooledTarget smooth = kit.acquire(size);
        kit.bindTarget(*smooth);
        gl->glUseProgram(m_smooth.id);
        kit.setFullscreen(m_smooth);
        gl->glUniform2f(m_smoothTexel, 1.0f / float(size.width()), 1.0f / float(size.height()));
        kit.bindTexture(0, raw->texture);
        kit.drawQuad();
        flow = std::move(smooth);
    }

    return flow;
}

PooledTarget GlRetime::interpolate(
    GlKit& kit, const RenderTarget& a, const RenderTarget& b, float w)
{
    GlFunctions* gl = kit.gl();
    PooledTarget flow = estimateFlow(kit, a, b, w);

    // ---- Full resolution ----
    PooledTarget out = kit.acquire(a.size);
    kit.bindTarget(*out);
    kit.setBlendOver(false);
    gl->glUseProgram(m_warp.id);
    kit.setFullscreen(m_warp);
    gl->glUniform1f(m_warpW, w);
    gl->glUniform2f(
        m_warpScale, 1.0f / float(flow->size.width()), 1.0f / float(flow->size.height()));
    kit.bindTexture(0, a.texture);
    kit.bindTexture(1, b.texture);
    kit.bindTexture(2, flow->texture);
    kit.drawQuad();
    return out;
}

} // namespace kk::render
