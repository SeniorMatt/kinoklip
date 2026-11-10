// SPDX-License-Identifier: GPL-3.0-or-later
// GlDamageEffects — 1.2 batch 4, worn-picture effects on the GPU (spec 57): Analog Damage, Film
// Damage and JPEG Damage. All randomness comes from an integer hash of the pixel, the seed and
// the clip-local frame number (render/GlShaderEffect.h), so the same frame renders the same
// way every time and exports repeat. Pixel parameters are timeline pixels times the layer scale.
#include "core/VideoEffectKeys.h"
#include "render/GlShaderEffect.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

float frameOf(const EffectInput& in)
{
    return float(std::max<int64_t>(0, in.localFrame));
}

// ---- analog_damage ----

std::unique_ptr<GlEffect> makeAnalogDamage()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"analog_damage", R"(
uniform sampler2D uTex;
uniform vec4 uP1;  // jitter px, colour bleed px, noise, tracking error (fraction of the height)
uniform vec4 uP2;  // scanlines, frame, seed, scanline height px
vec3 toYcc(vec3 c)
{
    float y = luma(c);
    return vec3(y, (c.b - y) / 1.8556, (c.r - y) / 1.5748);
}
vec3 fromYcc(vec3 v)
{
    float r = v.x + 1.5748 * v.z, b = v.x + 1.8556 * v.y;
    return vec3(r, (v.x - 0.2126 * r - 0.0722 * b) / 0.7152, b);
}
void main()
{
    vec2 sz = vec2(textureSize(uTex, 0));
    vec2 px = vUv * sz;
    uint fr = uint(uP2.y), sd = uint(uP2.z);
    uint fs = fr * 7919u + sd * 104729u;
    float line = floor(px.y / uP2.w);
    // Horizontal displacement per scan line: a slow wobble, rare torn lines, and a heavy
    // sideways pull inside the tracking band at the bottom of the picture.
    float off = uP1.x * 0.35 * sin(line * 0.045 + uP2.y * 0.9 + uP2.z);
    float tear = hash01(uint(line) * 2654435761u ^ fs);
    if (tear > 0.96)
        off += uP1.x * (hash01(uint(line) * 40503u ^ fs) - 0.5) * 3.0;
    float band = uP1.w;
    float depth = band > 0.0 ? clamp((vUv.y - (1.0 - band)) / band, 0.0, 1.0) : 0.0;
    off += uP1.x * 3.0 * depth * depth * (0.5 + hash01(uint(line) * 977u ^ fs));
    vec2 uv = vec2(vUv.x + off / sz.x, vUv.y);
    vec4 q = texture(uTex, uv);
    if (q.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec3 ycc = toYcc(straight(q));
    // Colour bleed: the colour of a pixel comes from the pixels to its left, smeared.
    if (uP1.y > 0.0) {
        vec2 cc = vec2(0.0);
        for (int i = 0; i < 7; ++i) {
            vec3 s = toYcc(straight(texture(uTex, uv - vec2(uP1.y * float(i) / 6.0 / sz.x, 0.0))));
            cc += s.yz;
        }
        ycc.yz = cc / 7.0;
    }
    float n = hash2(ivec2(px), fr + sd * 131u) - 0.5;
    ycc.x += n * uP1.z * 0.4;
    ycc.yz += (hash2(ivec2(px / 3.0), fr + 7u + sd * 131u) - 0.5) * uP1.z * 0.15;
    // White noise lines in the tracking band.
    if (depth > 0.0 && hash01(uint(line) * 6151u ^ fs) < depth * 0.5)
        ycc = mix(ycc, vec3(hash2(ivec2(px), fr + 99u), 0.0, 0.0), 0.6 * depth);
    vec3 o = fromYcc(ycc);
    o *= 1.0 - uP2.x * 0.45 * mod(line, 2.0);
    fragColor = vec4(clamp(o, 0.0, 1.0) * q.a, q.a);
}
)",
        {"uP1", "uP2"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            kit.gl()->glUniform4f(loc[0], float(pv(in, k::Jitter, 6) * in.scale),
                float(pv(in, k::ColorBleed, 8) * in.scale), float(pv(in, k::Noise, 20) / 100.0),
                float(pv(in, k::Tracking, 25) / 100.0 * 0.35));
            kit.gl()->glUniform4f(loc[1], float(pv(in, k::ScanlineAmount, 20) / 100.0), frameOf(in),
                float(pv(in, k::Seed, 1)), float(std::max(1.0, 2.0 * in.scale)));
        },
        [](const ParamSet& p, int64_t f) {
            return p.value(k::Jitter, f, 6) <= 0.0 && p.value(k::ColorBleed, f, 8) <= 0.0
                && p.value(k::Noise, f, 20) <= 0.0 && p.value(k::Tracking, f, 25) <= 0.0
                && p.value(k::ScanlineAmount, f, 20) <= 0.0;
        },
        nullptr});
}

// ---- film_damage ----

std::unique_ptr<GlEffect> makeFilmDamage()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"film_damage", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;  // the layer, softened by the Softness blur
uniform vec4 uP1;  // dirt, scratches, vignette, gate weave px
uniform vec4 uP2;  // flicker, frame, seed, layer scale
void main()
{
    vec2 sz = vec2(textureSize(uTex, 0));
    uint fr = uint(uP2.y), sd = uint(uP2.z);
    // Gate weave: the whole picture shifts a little, differently every frame.
    vec2 w = (vec2(hash01(fr * 2u + sd * 977u), hash01(fr * 2u + 1u + sd * 977u)) - 0.5) * 2.0
        * uP1.w;
    vec2 uv = vUv + w / sz;
    vec4 q = texture(uSecond, uv);
    if (q.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec3 c = straight(q);
    vec2 px = vUv * sz;
    float sc = uP2.w;
    c *= 1.0 + (hash01(fr * 31u + sd * 131u + 5u) - 0.5) * 2.0 * uP2.x;
    vec2 d = (vUv - 0.5) * 2.0;
    c *= 1.0 - uP1.z * smoothstep(0.35, 1.0, length(d) * 0.7071);
    // Dirt: a grid of cells, some holding one speck; specks stay for two frames.
    float cell = 24.0 * sc;
    uint ff = fr / 2u;
    vec3 dirt = vec3(1.0);
    float bright = 0.0;
    ivec2 cc = ivec2(floor(px / cell));
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            ivec2 ce = cc + ivec2(i, j);
            if (hash2(ce, ff * 5u + sd * 131u) >= uP1.x * 0.15)
                continue;
            vec2 centre = (vec2(ce) + vec2(hash2(ce, ff + 11u + sd), hash2(ce, ff + 13u + sd))) * cell;
            float rad = (0.7 + 2.3 * hash2(ce, ff + 17u + sd)) * sc;
            float cov = 1.0 - smoothstep(0.55 * rad, rad, length(px - centre));
            if (hash2(ce, ff + 19u + sd) < 0.2)
                bright = max(bright, cov);
            else
                dirt *= 1.0 - 0.9 * cov;
        }
    }
    c = mix(c * dirt, vec3(0.92), bright * 0.85);
    // Scratches: thin vertical lines, kept for three frames.
    uint sf = fr / 3u;
    for (int i = 0; i < 5; ++i) {
        uint id = sf * 8u + uint(i) + sd * 977u;
        if (hash01(id * 3u + 1u) >= uP1.y)
            continue;
        float x = hash01(id * 3u + 2u) * sz.x;
        float y0 = hash01(id * 3u + 3u) * sz.y * 0.5;
        float y1 = y0 + (0.3 + 0.7 * hash01(id * 5u + 7u)) * sz.y;
        float wob = 0.6 * sc * sin(px.y * 0.03 + float(i));
        float cov = (1.0 - smoothstep(0.2 * sc, 1.1 * sc, abs(px.x - x - wob)))
            * step(y0, px.y) * step(px.y, y1);
        c = mix(c, vec3(0.9), 0.7 * cov);
    }
    fragColor = vec4(clamp(c, 0.0, 1.0) * q.a, q.a);
}
)",
        {"uP1", "uP2"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            kit.gl()->glUniform4f(loc[0], float(pv(in, k::Dirt, 30) / 100.0),
                float(pv(in, k::Scratches, 25) / 100.0), float(pv(in, k::Vignette, 25) / 100.0),
                float(pv(in, k::Weave, 1.5) * in.scale));
            kit.gl()->glUniform4f(loc[1], float(pv(in, k::FlickerAmount, 5) / 100.0), frameOf(in),
                float(pv(in, k::Seed, 1)), float(in.scale));
        },
        [](const ParamSet& p, int64_t f) {
            return p.value(k::Dirt, f, 30) <= 0.0 && p.value(k::Scratches, f, 25) <= 0.0
                && p.value(k::Vignette, f, 25) <= 0.0 && p.value(k::Blur, f, 0.8) <= 0.0
                && p.value(k::Weave, f, 1.5) <= 0.0 && p.value(k::FlickerAmount, f, 5) <= 0.0;
        },
        [](const ParamSet& p, int64_t f, double s) {
            return p.value(k::Blur, f, 0.8) * s * kSigmaPerRadius;
        }});
}

// ---- jpeg_damage ----

std::unique_ptr<GlEffect> makeJpegDamage()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"jpeg_damage", R"(
uniform sampler2D uTex;
uniform vec3 uP;   // block size px, luma quantizer, colour quantizer
vec3 blockMean(vec2 origin, float size, vec2 sz)
{
    vec3 m = vec3(0.0);
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            m += straight(texture(uTex, (origin + (vec2(i, j) + 0.5) * size * 0.25) / sz));
    return m / 16.0;
}
float roundTo(float v, float q) { return floor(v / q + 0.5) * q; }
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec2 sz = vec2(textureSize(uTex, 0));
    vec2 px = vUv * sz;
    float B = uP.x;
    vec3 c = straight(t);
    vec3 mean = blockMean(floor(px / B) * B, B, sz);
    vec3 cmean = blockMean(floor(px / (2.0 * B)) * 2.0 * B, 2.0 * B, sz);
    float y = luma(c), my = luma(mean);
    // Brightness: the block average is coarsely quantized (the DC term), the detail around it
    // more finely. Colour comes from a block twice as large and is quantized harder.
    float yq = roundTo(my, uP.y * 1.5) + roundTo(y - my, uP.y);
    float yc = luma(cmean);
    float cb = roundTo((cmean.b - yc) / 1.8556, uP.z), cr = roundTo((cmean.r - yc) / 1.5748, uP.z);
    float r = yq + 1.5748 * cr, b = yq + 1.8556 * cb;
    vec3 o = vec3(r, (yq - 0.2126 * r - 0.0722 * b) / 0.7152, b);
    fragColor = vec4(clamp(o, 0.0, 1.0) * t.a, t.a);
}
)",
        {"uP"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const double q = 1.0 - std::clamp(pv(in, k::Quality, 30), 1.0, 100.0) / 100.0;
            const double qy = 0.002 + 0.12 * q * q;
            kit.gl()->glUniform3f(loc[0], float(std::max(2.0, pv(in, k::BlockSize, 8) * in.scale)),
                float(qy), float(qy * 1.5));
        },
        [](const ParamSet& p, int64_t f) { return p.value(k::Quality, f, 30) >= 100.0; }, nullptr});
}

const GlEffectFactory kTable[] = {
    {fxid::AnalogDamage, &makeAnalogDamage},
    {fxid::FilmDamage, &makeFilmDamage},
    {fxid::JpegDamage, &makeJpegDamage},
};

} // namespace

std::span<const GlEffectFactory> damageGlEffects()
{
    return kTable;
}

} // namespace kk::render
