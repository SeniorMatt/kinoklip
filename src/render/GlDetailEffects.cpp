// SPDX-License-Identifier: GPL-3.0-or-later
// GlDetailEffects — 1.2 batch 4, detail and repair effects on the GPU (spec 57): Sharpen Edges,
// Soften and Sharpen, Texture Pop, Detail Recovery, Deband, Dead Pixel Fixer and Beauty. Each is
// one fragment program over the layer, with a Gaussian blur of the layer as `uSecond` where the
// effect separates the picture into a base and detail. Pixel parameters are timeline pixels,
// multiplied by EffectInput::scale.
#include "core/VideoEffectKeys.h"
#include "render/GlShaderEffect.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

double sigmaOf(const ParamSet& p, const QString& key, int64_t f, double fallback, double scale)
{
    return std::max(kMinRadiusPx, p.value(key, f, fallback) * scale) * kSigmaPerRadius;
}

// ---- sharpen_edges ----

std::unique_ptr<GlEffect> makeSharpenEdges()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"sharpen_edges", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform vec4 uP; // strength (1 = 100 %), edge threshold, halo limit, radius px
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec3 c = straight(t);
    vec3 b = straight(texture(uSecond, vUv));
    vec2 tx = max(uP.w, 1.0) / vec2(textureSize(uTex, 0));
    float y = luma(c);
    float e = max(
        max(abs(luma(straight(texture(uTex, vUv + vec2(tx.x, 0.0)))) - y),
            abs(luma(straight(texture(uTex, vUv - vec2(tx.x, 0.0)))) - y)),
        max(abs(luma(straight(texture(uTex, vUv + vec2(0.0, tx.y)))) - y),
            abs(luma(straight(texture(uTex, vUv - vec2(0.0, tx.y)))) - y)));
    float thr = max(uP.y, 1e-4);
    float m = smoothstep(0.5 * thr, thr, e);
    vec3 d = clamp((c - b) * uP.x * m, -uP.z, uP.z);
    fragColor = vec4(clamp(c + d, 0.0, 1.0) * t.a, t.a);
}
)",
        {"uP"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            kit.gl()->glUniform4f(loc[0], float(pv(in, k::Amount, 100) / 100.0),
                float(pv(in, k::Threshold, 10) / 100.0), float(pv(in, k::HaloLimit, 25) / 100.0),
                float(pv(in, k::Radius, 1.5) * in.scale));
        },
        [](const ParamSet& p, int64_t f) { return p.value(k::Amount, f, 100) <= 0.0; },
        [](const ParamSet& p, int64_t f, double s) { return sigmaOf(p, k::Radius, f, 1.5, s); }});
}

// ---- soften_and_sharpen ----

std::unique_ptr<GlEffect> makeSoftenSharpen()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"soften_and_sharpen", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform vec3 uP; // soften, sharpen, texture limit
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec3 c = straight(t);
    vec3 b = straight(texture(uSecond, vUv));
    vec3 d = c - b;
    float thr = max(uP.z, 1e-4);
    float small = 1.0 - smoothstep(thr, 2.0 * thr, maxAbs(d));
    float gain = mix(1.0 + uP.y, 1.0 - uP.x, small);
    fragColor = vec4(clamp(b + d * gain, 0.0, 1.0) * t.a, t.a);
}
)",
        {"uP"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            kit.gl()->glUniform3f(loc[0], float(pv(in, k::Soften, 50) / 100.0),
                float(pv(in, k::Sharpen, 60) / 100.0), float(pv(in, k::Threshold, 6) / 100.0));
        },
        [](const ParamSet& p, int64_t f) {
            return p.value(k::Soften, f, 50) <= 0.0 && p.value(k::Sharpen, f, 60) <= 0.0;
        },
        [](const ParamSet& p, int64_t f, double s) { return sigmaOf(p, k::Radius, f, 2, s); }});
}

// ---- texture_pop ----

std::unique_ptr<GlEffect> makeTexturePop()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"texture_pop", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform vec3 uP; // gain, highlight protection, brightness only (1)
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec3 c = straight(t);
    vec3 b = straight(texture(uSecond, vUv));
    float y = luma(c);
    // Less lift in the highlights (protection) and at black, where noise lives.
    float w = (1.0 - uP.y * smoothstep(0.55, 1.0, y)) * smoothstep(0.0, 0.08, y);
    vec3 o = uP.z > 0.5 ? c + vec3(uP.x * w * (y - luma(b))) : c + uP.x * w * (c - b);
    fragColor = vec4(clamp(o, 0.0, 1.0) * t.a, t.a);
}
)",
        {"uP"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            kit.gl()->glUniform3f(loc[0], float(pv(in, k::Amount, 80) / 100.0),
                float(pv(in, k::Highlights, 40) / 100.0),
                boolParam(*in.params, k::LumaOnly, true) ? 1.f : 0.f);
        },
        [](const ParamSet& p, int64_t f) { return p.value(k::Amount, f, 80) == 0.0; },
        [](const ParamSet& p, int64_t f, double s) { return sigmaOf(p, k::Radius, f, 2, s); }});
}

// ---- detail_recovery ----

std::unique_ptr<GlEffect> makeDetailRecovery()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"detail_recovery", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform vec3 uP; // shadows, highlights, detail gain
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec3 c = straight(t);
    float y = luma(c);
    float yb = luma(straight(texture(uSecond, vUv)));
    // The slowly changing tone is pulled toward the middle at both ends of the range; the fast
    // detail on top of it is kept and amplified, so what was crushed shows again.
    float lift = uP.x * (1.0 - smoothstep(0.0, 0.5, yb)) * (0.5 - yb) * 0.8;
    float drop = uP.y * smoothstep(0.5, 1.0, yb) * (yb - 0.5) * 0.8;
    float yo = yb + lift - drop + (y - yb) * (1.0 + uP.z);
    fragColor = vec4(clamp(c + vec3(yo - y), 0.0, 1.0) * t.a, t.a);
}
)",
        {"uP"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            kit.gl()->glUniform3f(loc[0], float(pv(in, k::Shadows, 40) / 100.0),
                float(pv(in, k::Highlights, 40) / 100.0), float(pv(in, k::Detail, 40) / 100.0));
        },
        [](const ParamSet& p, int64_t f) {
            return p.value(k::Shadows, f, 40) == 0.0 && p.value(k::Highlights, f, 40) == 0.0
                && p.value(k::Detail, f, 40) == 0.0;
        },
        [](const ParamSet& p, int64_t f, double s) { return sigmaOf(p, k::Radius, f, 30, s); }});
}

// ---- deband ----

std::unique_ptr<GlEffect> makeDeband()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"deband", R"(
uniform sampler2D uTex;
uniform vec4 uP;   // threshold (fraction of full scale), dither (0..1), frame, radius px
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec3 c = straight(t);
    // The local average is 9 x 9 tent-weighted taps over the radius, shrunk symmetrically near the frame
    // edges: on a smooth gradient it is exactly the pixel itself, so nothing changes there.
    vec2 sz = vec2(textureSize(uTex, 0));
    vec2 px = vUv * sz;
    vec2 ext = vec2(min(uP.w, min(px.x, sz.x - px.x)), min(uP.w, min(px.y, sz.y - px.y)));
    vec3 b = vec3(0.0);
    float wsum = 0.0;
    for (int j = -4; j <= 4; ++j) {
        for (int i = -4; i <= 4; ++i) {
            float w = float((5 - abs(i)) * (5 - abs(j))); // tent weights: a smooth average
            b += w * straight(texture(uTex, vUv + vec2(i, j) * ext * 0.26 / sz));
            wsum += w;
        }
    }
    b /= wsum;
    // Where the pixel is close to its wide average the area is a gradient (or flat): use the
    // average. Where it differs more it is a real edge or texture: keep it.
    float thr = max(uP.x, 1e-5);
    float m = 1.0 - smoothstep(0.5 * thr, thr, maxAbs(c - b));
    ivec2 pix = ivec2(gl_FragCoord.xy);
    uint f = uint(uP.z);
    vec3 n = vec3(hash2(pix, f * 3u), hash2(pix, f * 3u + 1u), hash2(pix, f * 3u + 2u)) - 0.5;
    vec3 o = mix(c, b, m) + n * (2.0 / 255.0) * uP.y * m;
    fragColor = vec4(clamp(o, 0.0, 1.0) * t.a, t.a);
}
)",
        {"uP"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            kit.gl()->glUniform4f(loc[0], float(pv(in, k::Threshold, 8) / 255.0),
                float(pv(in, k::Dither, 40) / 100.0), float(std::max<int64_t>(0, in.localFrame)),
                float(std::max(1.0, pv(in, k::Radius, 16) * in.scale)));
        },
        [](const ParamSet& p, int64_t f) { return p.value(k::Threshold, f, 8) <= 0.0; }, nullptr});
}

// ---- dead_pixel_fixer ----

std::unique_ptr<GlEffect> makeDeadPixelFixer()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"dead_pixel_fixer", R"(
uniform sampler2D uTex;
uniform vec3 uP;   // threshold, ring distance in layer px, detect (0 both, 1 bright, 2 dark)
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec3 c = straight(t);
    float y = luma(c);
    vec2 tx = 1.0 / vec2(textureSize(uTex, 0));
    float d = uP.y;
    float r = 0.70710678 * d;
    vec2 off[8] = vec2[8](vec2(d, 0.0), vec2(-d, 0.0), vec2(0.0, d), vec2(0.0, -d),
        vec2(r, r), vec2(-r, r), vec2(r, -r), vec2(-r, -r));
    vec3 sum = vec3(0.0);
    float mn = 1e9, mx = -1e9;
    for (int i = 0; i < 8; ++i) {
        vec3 s = straight(texture(uTex, vUv + off[i] * tx));
        sum += s;
        float l = luma(s);
        mn = min(mn, l);
        mx = max(mx, l);
    }
    bool bright = y > mx + uP.x;
    bool dark = y < mn - uP.x;
    bool dead;
    if (uP.z == 2.0)
        dead = dark;
    else if (uP.z == 1.0)
        dead = bright;
    else
        dead = bright || dark;
    vec3 o = dead ? sum / 8.0 : c;
    fragColor = vec4(clamp(o, 0.0, 1.0) * t.a, t.a);
}
)",
        {"uP"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const QString mode = choiceParam(*in.params, k::Detect, fxchoice::Both);
            const float detect
                = mode == fxchoice::Bright ? 1.f : (mode == fxchoice::Dark ? 2.f : 0.f);
            kit.gl()->glUniform3f(loc[0], float(pv(in, k::Threshold, 25) / 100.0),
                float(std::max(1.0, pv(in, k::Size, 1) * in.scale)), detect);
        },
        nullptr, nullptr});
}

// ---- beauty ----

std::unique_ptr<GlEffect> makeBeauty()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"beauty", R"(
uniform sampler2D uTex;
uniform vec4 uP;   // smoothing, tap spacing px, range sigma, detail recover
uniform float uSkin;
float skinMask(vec3 c)
{
    float cb = -0.168736 * c.r - 0.331264 * c.g + 0.5 * c.b + 0.5;
    float cr = 0.5 * c.r - 0.418688 * c.g - 0.081312 * c.b + 0.5;
    float m = smoothstep(0.28, 0.32, cb) * (1.0 - smoothstep(0.48, 0.52, cb));
    m *= smoothstep(0.50, 0.54, cr) * (1.0 - smoothstep(0.66, 0.70, cr));
    return m * smoothstep(0.08, 0.2, luma(c));
}
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec3 c = straight(t);
    vec2 tx = uP.y / vec2(textureSize(uTex, 0));
    float inv2s = 1.0 / (2.0 * uP.z * uP.z);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    // Bilateral filter: neighbours count by distance and by how close their colour is, so a
    // blotchy area is averaged but an edge, whose colours are far apart, is not crossed.
    for (int j = -3; j <= 3; ++j) {
        for (int i = -3; i <= 3; ++i) {
            vec4 q = texture(uTex, vUv + vec2(i, j) * tx);
            if (q.a <= 0.0)
                continue;
            vec3 s = straight(q);
            vec3 dc = s - c;
            float w = exp(-float(i * i + j * j) / 4.5) * exp(-dot(dc, dc) * inv2s);
            sum += s * w;
            wsum += w;
        }
    }
    vec3 smooth_ = sum / max(wsum, 1e-6);
    vec3 soft = smooth_ + uP.w * (c - smooth_); // give back a share of the fine texture
    float k = uP.x * (uSkin > 0.5 ? skinMask(c) : 1.0);
    fragColor = vec4(clamp(mix(c, soft, k), 0.0, 1.0) * t.a, t.a);
}
)",
        {"uP", "uSkin"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            kit.gl()->glUniform4f(loc[0], float(pv(in, k::Smoothing, 50) / 100.0),
                float(std::max(0.5, pv(in, k::Radius, 6) * in.scale / 3.0)),
                float(std::max(0.005, pv(in, k::EdgePreserve, 20) * 0.25 / 100.0)),
                float(pv(in, k::DetailRecover, 20) / 100.0));
            kit.gl()->glUniform1f(loc[1], boolParam(*in.params, k::SkinOnly, false) ? 1.f : 0.f);
        },
        [](const ParamSet& p, int64_t f) { return p.value(k::Smoothing, f, 50) <= 0.0; }, nullptr});
}

const GlEffectFactory kTable[] = {
    {fxid::SharpenEdges, &makeSharpenEdges},
    {fxid::SoftenSharpen, &makeSoftenSharpen},
    {fxid::TexturePop, &makeTexturePop},
    {fxid::DetailRecovery, &makeDetailRecovery},
    {fxid::Deband, &makeDeband},
    {fxid::DeadPixelFixer, &makeDeadPixelFixer},
    {fxid::Beauty, &makeBeauty},
};

} // namespace

std::span<const GlEffectFactory> detailGlEffects()
{
    return kTable;
}

} // namespace kk::render
