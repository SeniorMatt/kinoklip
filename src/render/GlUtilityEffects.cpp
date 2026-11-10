// SPDX-License-Identifier: GPL-3.0-or-later
// GlUtilityEffects — 1.2 batch 4 (spec 57): Color Palette (dominant colours as swatches), 3D
// Keyer (keys up to three colours in Y'CbCr space) and Video Collage (a grid of tiles).
//
// Color Palette runs k-means (a standard method) on the GPU: a first pass renders one texel per
// swatch; each texel clusters the same 16 x 16 grid of samples of the picture, starting from
// the farthest-point seeds, so every texel reaches the same clusters, and keeps the cluster
// whose size ranks as its own index (biggest first). A second pass paints the swatches.
// Nothing random is involved: the same picture always gives the same palette.
#include "core/VideoEffectKeys.h"
#include "render/GlShaderEffect.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

// ---- color_palette ----

class ColorPalette final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "color_palette", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;  // the palette, one texel per swatch
uniform vec4 uP;   // position (0 bottom, 1 top, 2 left, 3 right), strip size, gap px, opacity
uniform int uCount;
uniform vec2 uSize;  // layer size px
void main()
{
    vec4 t = texture(uTex, vUv);
    float size = uP.y;
    bool horizontal = uP.x < 1.5;
    float across = horizontal ? vUv.y : vUv.x;
    bool inStrip = (uP.x == 0.0 || uP.x == 3.0) ? across > 1.0 - size : across < size;
    if (!inStrip) { fragColor = t; return; }
    float along = horizontal ? vUv.x : vUv.y;
    float lengthPx = horizontal ? uSize.x : uSize.y;
    float cell = along * float(uCount);
    int i = min(int(floor(cell)), uCount - 1);
    float gap = 0.5 * uP.z * float(uCount) / lengthPx;
    if (fract(cell) < gap || fract(cell) > 1.0 - gap) { fragColor = t; return; }
    vec3 col = texture(uSecond, vec2((float(i) + 0.5) / float(uCount), 0.5)).rgb;
    fragColor = vec4(col * uP.w + t.rgb * (1.0 - uP.w), uP.w + t.a * (1.0 - uP.w));
}
)",
                why, true))
            return false;
        m_pos = uniform(kit, "uP");
        m_count = uniform(kit, "uCount");
        m_size = uniform(kit, "uSize");
        if (!m_analyse.build(kit.gl(), "color_palette_analyse", R"(
uniform sampler2D uTex;
uniform int uCount;
const int G = 16;
const int NS = G * G;
const int MAXK = 24;
vec3 sampleColor[NS];
float valid[NS];
vec3 centre[MAXK];
float cnt[MAXK];
float d2(vec3 a, vec3 b) { vec3 d = a - b; return dot(d, d); }
void main()
{
    int idx = int(gl_FragCoord.x);
    vec3 mean = vec3(0.0);
    float n = 0.0;
    for (int j = 0; j < G; ++j) {
        for (int i = 0; i < G; ++i) {
            vec4 q = texture(uTex, (vec2(i, j) + 0.5) / float(G));
            int s = j * G + i;
            valid[s] = q.a > 0.0 ? 1.0 : 0.0;
            sampleColor[s] = q.a > 0.0 ? q.rgb / q.a : vec3(0.0);
            mean += sampleColor[s] * valid[s];
            n += valid[s];
        }
    }
    if (n < 1.0) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    mean /= n;
    // Farthest-point seeds: the first is the sample farthest from the mean, each next one the
    // sample farthest from all chosen so far.
    float dmin[NS];
    int first = 0;
    float best = -1.0;
    for (int s = 0; s < NS; ++s) {
        float d = valid[s] > 0.5 ? d2(sampleColor[s], mean) : -1.0;
        if (d > best) { best = d; first = s; }
    }
    centre[0] = sampleColor[first];
    for (int s = 0; s < NS; ++s)
        dmin[s] = valid[s] > 0.5 ? d2(sampleColor[s], centre[0]) : -1.0;
    for (int c = 1; c < uCount; ++c) {
        int pick = 0;
        best = -1.0;
        for (int s = 0; s < NS; ++s) {
            if (dmin[s] > best) { best = dmin[s]; pick = s; }
        }
        centre[c] = sampleColor[pick];
        for (int s = 0; s < NS; ++s)
            dmin[s] = valid[s] > 0.5 ? min(dmin[s], d2(sampleColor[s], centre[c])) : -1.0;
    }
    for (int iter = 0; iter < 6; ++iter) {
        vec3 sum[MAXK];
        for (int c = 0; c < uCount; ++c) { sum[c] = vec3(0.0); cnt[c] = 0.0; }
        for (int s = 0; s < NS; ++s) {
            if (valid[s] < 0.5)
                continue;
            int nearest = 0;
            float bd = 1e9;
            for (int c = 0; c < uCount; ++c) {
                float d = d2(sampleColor[s], centre[c]);
                if (d < bd) { bd = d; nearest = c; }
            }
            sum[nearest] += sampleColor[s];
            cnt[nearest] += 1.0;
        }
        for (int c = 0; c < uCount; ++c)
            if (cnt[c] > 0.0)
                centre[c] = sum[c] / cnt[c];
    }
    // The swatch with this texel's rank: biggest cluster first, ties by index.
    vec3 outc = centre[0];
    for (int c = 0; c < uCount; ++c) {
        int rank = 0;
        for (int o = 0; o < uCount; ++o)
            rank += (cnt[o] > cnt[c] || (cnt[o] == cnt[c] && o < c)) ? 1 : 0;
        if (rank == idx)
            outc = centre[c];
    }
    fragColor = vec4(outc, 1.0);
}
)",
                why))
            return false;
        kit.gl()->glUseProgram(m_analyse.id);
        kit.gl()->glUniform1i(m_analyse.uniform(kit.gl(), "uTex"), 0);
        kit.gl()->glUseProgram(0);
        m_analyseCount = m_analyse.uniform(kit.gl(), "uCount");
        return true;
    }
    void destroy(GlKit& kit) override
    {
        PassEffect::destroy(kit);
        m_analyse.destroy(kit.gl());
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Opacity, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int count = int(std::clamp(val(p, k::Count, in.localFrame, 8), 2.0, 24.0));
        PooledTarget palette = kit.acquire(QSize(count, 1));
        kit.bindTarget(*palette);
        kit.setBlendOver(false);
        kit.gl()->glUseProgram(m_analyse.id);
        kit.setFullscreen(m_analyse);
        kit.gl()->glUniform1i(m_analyseCount, count);
        kit.bindTexture(0, in.layer->texture);
        kit.drawQuad();

        const QString where = choiceParam(p, k::Position, fxchoice::PosBottom);
        const float pos = where == fxchoice::PosTop ? 1.f
            : where == fxchoice::PosLeft            ? 2.f
            : where == fxchoice::PosRight           ? 3.f
                                                    : 0.f;
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform4f(m_pos, pos, float(val(p, k::Size, in.localFrame, 12) / 100.0),
            float(val(p, k::Gap, in.localFrame, 2) * in.scale),
            float(val(p, k::Opacity, in.localFrame, 100) / 100.0));
        gl->glUniform1i(m_count, count);
        gl->glUniform2f(m_size, float(in.layer->size.width()), float(in.layer->size.height()));
        draw(kit, *in.layer, palette->texture);
        return out;
    }

private:
    static double val(const ParamSet& p, const QString& key, int64_t f, double fallback)
    {
        return p.value(key, f, fallback);
    }
    GlProgram m_analyse;
    GLint m_pos = -1, m_count = -1, m_size = -1, m_analyseCount = -1;
};

// ---- keyer_3d ----

std::unique_ptr<GlEffect> makeKeyer3d()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"keyer_3d", R"(
uniform sampler2D uTex;
uniform vec3 uK[3];   // key colours, straight
uniform vec4 uP;      // tolerance, softness, brightness weight, used colours (1..3)
uniform vec4 uQ;      // clip black, clip white, invert, view
vec3 space(vec3 c)
{
    float y = luma(c);
    return vec3((c.b - y) / 1.8556, (c.r - y) / 1.5748, y * sqrt(uP.z));
}
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec3 c = straight(t);
    vec3 v = space(c);
    float d = 1e9;
    for (int i = 0; i < 3; ++i) {
        if (float(i) >= uP.w)
            break;
        d = min(d, length(v - space(uK[i])));
    }
    float m = smoothstep(uP.x, uP.x + max(uP.y, 1e-3), d);
    m = clamp((m - uQ.x) / max(uQ.y - uQ.x, 1e-3), 0.0, 1.0);
    if (uQ.z > 0.5)
        m = 1.0 - m;
    fragColor = keyOutput(int(uQ.w), c, t.a, m);
}
)",
        {"uK", "uP", "uQ"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const ParamSet& p = *in.params;
            const fxmath::Rgb a = colorParam(p, k::KeyColor, {0.f, 0.69f, 0.25f});
            const fxmath::Rgb b = colorParam(p, k::Color2, {0.f, 0.28f, 0.73f});
            const fxmath::Rgb c = colorParam(p, k::Color3, {0.86f, 0.16f, 0.16f});
            // The enabled colours, packed from the front (colour 1 is always used).
            float keys[9] = {a.r, a.g, a.b};
            float used = 1.f;
            if (boolParam(p, k::UseColor2, false)) {
                keys[size_t(used) * 3] = b.r, keys[size_t(used) * 3 + 1] = b.g,
                                    keys[size_t(used) * 3 + 2] = b.b;
                used += 1.f;
            }
            if (boolParam(p, k::UseColor3, false)) {
                keys[size_t(used) * 3] = c.r, keys[size_t(used) * 3 + 1] = c.g,
                                    keys[size_t(used) * 3 + 2] = c.b;
                used += 1.f;
            }
            kit.gl()->glUniform3fv(loc[0], 3, keys);
            kit.gl()->glUniform4f(loc[1], float(pv(in, k::Tolerance, 25) * 0.005),
                float(pv(in, k::Softness, 15) * 0.005), float(pv(in, k::LumaWeight, 30) / 100.0),
                used);
            const fxmath::KeyView view = fxmath::keyView(p);
            kit.gl()->glUniform4f(loc[2], float(pv(in, k::ClipBlack, 0) / 100.0),
                float(pv(in, k::ClipWhite, 100) / 100.0),
                boolParam(p, k::Invert, false) ? 1.f : 0.f, float(int(view)));
        },
        nullptr, nullptr});
}

// ---- video_collage ----

std::unique_ptr<GlEffect> makeVideoCollage()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"video_collage", R"(
uniform sampler2D uTex;
uniform vec4 uP;   // columns, rows, spacing px, corner radius px
uniform vec4 uQ;   // fit (0 stretch, 1 fill, 2 fit), shadow, background opacity, -
uniform vec3 uBg;
float roundedBox(vec2 q, vec2 half_, float r)
{
    vec2 d = abs(q) - half_ + r;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}
void main()
{
    vec2 sz = vec2(textureSize(uTex, 0));
    vec2 px = vUv * sz;
    vec2 grid = uP.xy;
    vec2 cellSz = sz / grid;
    vec2 cell = min(floor(px / cellSz), grid - 1.0);
    vec2 q = px - (cell + 0.5) * cellSz;
    vec2 tileSz = max(cellSz - vec2(uP.z), vec2(1.0));
    float r = min(uP.w, 0.5 * min(tileSz.x, tileSz.y));
    float cover = clamp(0.5 - roundedBox(q, 0.5 * tileSz, r), 0.0, 1.0);
    vec2 tuv = q / tileSz + 0.5;
    float picAspect = sz.x / sz.y, tileAspect = tileSz.x / tileSz.y;
    if (uQ.x > 0.5 && uQ.x < 1.5) {          // fill: crop the picture to the tile
        if (tileAspect > picAspect) tuv.y = (tuv.y - 0.5) * picAspect / tileAspect + 0.5;
        else tuv.x = (tuv.x - 0.5) * tileAspect / picAspect + 0.5;
    } else if (uQ.x > 1.5) {                 // fit: the whole picture inside the tile
        if (tileAspect > picAspect) tuv.x = (tuv.x - 0.5) * tileAspect / picAspect + 0.5;
        else tuv.y = (tuv.y - 0.5) * picAspect / tileAspect + 0.5;
    }
    vec4 tex = texture(uTex, clamp(tuv, 0.0, 1.0));
    if (any(lessThan(tuv, vec2(0.0))) || any(greaterThan(tuv, vec2(1.0))))
        tex = vec4(0.0);
    // Shadow: the tile's outline moved down-right and softened.
    float soft = uP.z * 0.4 + 1.0;
    float sd = roundedBox(q - vec2(uP.z * 0.2), 0.5 * tileSz, r);
    float shadow = uQ.y * (1.0 - smoothstep(-soft, soft, sd));
    vec4 base = vec4(0.0, 0.0, 0.0, shadow);
    vec4 bg = vec4(uBg * uQ.z, uQ.z);
    base = base + bg * (1.0 - shadow);
    fragColor = tex * cover + base * (1.0 - tex.a * cover);
}
)",
        {"uP", "uQ", "uBg"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const ParamSet& p = *in.params;
            const QString fit = choiceParam(p, k::Fit, fxchoice::Stretch);
            const fxmath::Rgb bg = colorParam(p, k::BackgroundColor, {0.f, 0.f, 0.f});
            kit.gl()->glUniform4f(loc[0], float(std::clamp(pv(in, k::Columns, 3), 1.0, 12.0)),
                float(std::clamp(pv(in, k::Rows, 3), 1.0, 12.0)),
                float(pv(in, k::Spacing, 8) * in.scale), float(pv(in, k::Radius, 0) * in.scale));
            kit.gl()->glUniform4f(loc[1],
                fit == fxchoice::Fill ? 1.f : (fit == fxchoice::FitInside ? 2.f : 0.f),
                float(pv(in, k::ShadowOpacity, 30) / 100.0), float(pv(in, k::Opacity, 0) / 100.0),
                0.f);
            kit.gl()->glUniform3f(loc[2], bg.r, bg.g, bg.b);
        },
        [](const ParamSet& p, int64_t f) {
            return p.value(k::Columns, f, 3) <= 1.0 && p.value(k::Rows, f, 3) <= 1.0
                && p.value(k::Spacing, f, 8) <= 0.0 && p.value(k::Radius, f, 0) <= 0.0
                && choiceParam(p, k::Fit, fxchoice::Stretch) == fxchoice::Stretch;
        },
        nullptr});
}

const GlEffectFactory kTable[] = {
    {fxid::ColorPalette, &make<ColorPalette>},
    {fxid::Keyer3d, &makeKeyer3d},
    {fxid::VideoCollage, &makeVideoCollage},
};

} // namespace

std::span<const GlEffectFactory> utilityGlEffects()
{
    return kTable;
}

} // namespace kk::render
