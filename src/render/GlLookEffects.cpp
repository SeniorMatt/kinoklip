// SPDX-License-Identifier: GPL-3.0-or-later
// GlLookEffects — the second batch of Stylize effects on the GPU: Pixelate (hexagons, circle
// dots, diamonds), Halftone, Scanlines, Pencil Sketch, Abstraction and Prism (Duotone, a
// pointwise one, lives in GlGradeEffects.cpp).
//
// Costs per pixel: pixelate, halftone, scanlines = 1 to 4 taps; pencil sketch = a Gaussian
// blur (bounded, GlKit) + 1 tap pair; abstraction = 64 taps (a Kuwahara filter on a 4 x 4
// grid per quadrant, spread over the radius) + 5 taps of outlines; prism = kPrismTaps taps.
#include "core/VideoEffectKeys.h"
#include "render/GlEffectBase.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

double val(const ParamSet& p, const QString& key, int64_t f, double fallback)
{
    return p.value(key, f, fallback);
}

double seconds(const EffectInput& in)
{
    return in.fps > 0 ? double(in.localFrame) / in.fps : 0.0;
}

constexpr int kPrismTaps = 16;

// ---- pixelate ----

class Pixelate final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "pixelate", R"(
uniform sampler2D uTex;
uniform vec2 uSize;     // layer px
uniform vec2 uOrigin;   // content centre, px (the grid is centred on the clip)
uniform float uCell;    // cell size, px
uniform int uShape;     // 0 = hexagons, 1 = circle dots, 2 = diamonds
uniform vec3 uBackground;
vec4 at(vec2 px) { return texture(uTex, clamp(px, vec2(0.5), uSize - 0.5) / uSize); }
void main()
{
    vec2 px = vUv * uSize;
    vec2 p = (px - uOrigin) / uCell;
    if (uShape == 0) {
        // Pointy-top hexagons: the nearer centre of two offset rectangular lattices.
        const vec2 r = vec2(1.0, 1.7320508);
        vec2 a = mod(p, r) - 0.5 * r;
        vec2 b = mod(p - 0.5 * r, r) - 0.5 * r;
        vec2 g = dot(a, a) < dot(b, b) ? a : b;
        fragColor = at((p - g) * uCell + uOrigin);
        return;
    }
    vec2 q = p;
    if (uShape == 2) // diamonds: a square grid turned by 45 degrees
        q = vec2(p.x + p.y, p.y - p.x) * 0.70710678;
    vec2 cell = floor(q) + 0.5;
    vec2 centre = uShape == 2 ? vec2(cell.x - cell.y, cell.x + cell.y) * 0.70710678 : cell;
    vec4 c = at(centre * uCell + uOrigin);
    if (uShape == 2) {
        fragColor = c;
        return;
    }
    // Round dots, antialiased over one pixel, on the background colour.
    float d = length(q - cell) * uCell;
    float cover = 1.0 - smoothstep(0.47 * uCell - 0.5, 0.47 * uCell + 0.5, d);
    fragColor = mix(vec4(uBackground * c.a, c.a), c, cover);
}
)",
                why))
            return false;
        m_size = uniform(kit, "uSize");
        m_origin = uniform(kit, "uOrigin");
        m_cell = uniform(kit, "uCell");
        m_shape = uniform(kit, "uShape");
        m_background = uniform(kit, "uBackground");
        return true;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const double cell = std::max(1.0, val(p, k::Size, in.localFrame, 24) * in.scale);
        const QString shape = choiceParam(p, k::Shape, fxchoice::Hexagons);
        const int mode = shape == fxchoice::Circles ? 1 : shape == fxchoice::Diamonds ? 2 : 0;
        const fxmath::Rgb bg = colorParam(p, k::BackgroundColor, {0.f, 0.f, 0.f});
        const LayerGeometry g = layerGeometry(in);
        GlFunctions* gl = kit.gl();
        PooledTarget out = begin(kit, *in.layer);
        gl->glUniform2f(m_size, g.width, g.height);
        gl->glUniform2f(m_origin, 0.5f * g.width, 0.5f * g.height);
        gl->glUniform1f(m_cell, float(cell));
        gl->glUniform1i(m_shape, mode);
        gl->glUniform3f(m_background, bg.r, bg.g, bg.b);
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_size = -1, m_origin = -1, m_cell = -1, m_shape = -1, m_background = -1;
};

// ---- halftone ----

class Halftone final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "halftone", R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform vec2 uOrigin;
uniform float uCell;
uniform vec2 uRot;      // cos, sin of the screen angle
uniform int uColored;
uniform float uAmount;
void main()
{
    vec2 px = vUv * uSize - uOrigin;
    mat2 toScreen = mat2(uRot.x, -uRot.y, uRot.y, uRot.x);
    mat2 fromScreen = mat2(uRot.x, uRot.y, -uRot.y, uRot.x);
    vec2 q = toScreen * px / uCell;
    vec2 cell = floor(q) + 0.5;
    vec2 centrePx = fromScreen * (cell * uCell) + uOrigin;
    vec4 sampled = texture(uTex, clamp(centrePx, vec2(0.5), uSize - 0.5) / uSize);
    vec3 c = straight(sampled);
    float y = clamp(luma(c), 0.0, 1.0);
    // Dot area proportional to ink: dark = big black dots; colour mode: bright = big dots.
    float ink = uColored == 1 ? y : 1.0 - y;
    float radius = sqrt(ink) * 0.7071 * uCell;
    float d = length(q - cell) * uCell;
    float dot1 = 1.0 - smoothstep(radius - 0.5, radius + 0.5, d);
    vec3 look = uColored == 1 ? c / max(y, 0.05) * 0.8 * dot1 : vec3(1.0 - dot1);
    vec4 src = texture(uTex, vUv);
    vec3 orig = straight(src);
    fragColor = vec4(clamp(mix(orig, look, uAmount), 0.0, 1.0) * src.a, src.a);
}
)",
                why, true))
            return false;
        m_size = uniform(kit, "uSize");
        m_origin = uniform(kit, "uOrigin");
        m_cell = uniform(kit, "uCell");
        m_rot = uniform(kit, "uRot");
        m_colored = uniform(kit, "uColored");
        m_amount = uniform(kit, "uAmount");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        const LayerGeometry g = layerGeometry(in);
        const double a = val(p, k::Angle, f, 45) * std::numbers::pi / 180.0;
        GlFunctions* gl = kit.gl();
        PooledTarget out = begin(kit, *in.layer);
        gl->glUniform2f(m_size, g.width, g.height);
        gl->glUniform2f(m_origin, 0.5f * g.width, 0.5f * g.height);
        gl->glUniform1f(m_cell, float(std::max(1.5, val(p, k::Size, f, 8) * in.scale)));
        gl->glUniform2f(m_rot, float(std::cos(a)), float(std::sin(a)));
        gl->glUniform1i(m_colored, choiceParam(p, k::Mode, fxchoice::Mono) == fxchoice::Colored);
        gl->glUniform1f(m_amount, float(std::clamp(val(p, k::Amount, f, 100) / 100.0, 0.0, 1.0)));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_size = -1, m_origin = -1, m_cell = -1, m_rot = -1, m_colored = -1, m_amount = -1;
};

// ---- scanlines ----

class Scanlines final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "scanlines", R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform vec2 uNormal;     // unit, px space (across the lines)
uniform float uSpacing;   // px
uniform float uThickness; // 0..1 of the spacing
uniform float uOffset;    // px (rolling)
uniform float uDark;      // 0..1
void main()
{
    float s = dot(vUv * uSize, uNormal) + uOffset;
    float phase = fract(s / uSpacing) * uSpacing; // px into the period
    float w = uThickness * uSpacing;
    // A dark band of width w with one-pixel soft edges (antialiased at any angle).
    float line = smoothstep(-0.5, 0.5, phase) * (1.0 - smoothstep(w - 0.5, w + 0.5, phase));
    vec4 c = texture(uTex, vUv);
    fragColor = vec4(c.rgb * (1.0 - uDark * line), c.a);
}
)",
                why))
            return false;
        m_size = uniform(kit, "uSize");
        m_normal = uniform(kit, "uNormal");
        m_spacing = uniform(kit, "uSpacing");
        m_thickness = uniform(kit, "uThickness");
        m_offset = uniform(kit, "uOffset");
        m_dark = uniform(kit, "uDark");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 50) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        const double a = val(p, k::Angle, f, 0) * std::numbers::pi / 180.0;
        const double spacing = std::max(1.0, val(p, k::Spacing, f, 4) * in.scale);
        const double offset = std::fmod(val(p, k::Speed, f, 0) * in.scale * seconds(in), spacing);
        GlFunctions* gl = kit.gl();
        PooledTarget out = begin(kit, *in.layer);
        gl->glUniform2f(m_size, float(in.layer->size.width()), float(in.layer->size.height()));
        // Angle 0 = horizontal lines (normal pointing down the picture).
        gl->glUniform2f(m_normal, float(-std::sin(a)), float(std::cos(a)));
        gl->glUniform1f(m_spacing, float(spacing));
        gl->glUniform1f(
            m_thickness, float(std::clamp(val(p, k::Thickness, f, 50) / 100.0, 0.0, 1.0)));
        gl->glUniform1f(m_offset, float(-offset - in.paddingPx)); // lines start at the content top
        gl->glUniform1f(m_dark, float(std::clamp(val(p, k::Amount, f, 50) / 100.0, 0.0, 1.0)));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_size = -1, m_normal = -1, m_spacing = -1, m_thickness = -1, m_offset = -1, m_dark = -1;
};

// ---- pencil_sketch ----

class PencilSketch final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "pencil_sketch", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond; // blurred copy
uniform float uStrength;
uniform float uColor;      // 0 = grey pencil, 1 = coloured pencil
uniform float uAmount;
void main()
{
    vec4 s = texture(uTex, vUv);
    if (s.a <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    vec3 c = straight(s);
    vec4 b = texture(uSecond, vUv);
    float g = luma(c);
    float blurred = b.a > 0.0 ? luma(b.rgb / b.a) : g;
    // Colour dodge of the grey picture with its blurred inverse = g / blur(g): flat areas
    // become paper white, edges (where the picture is darker than its surroundings) lines.
    float sketch = clamp(g / max(blurred, 1e-3), 0.0, 1.0);
    sketch = clamp(1.0 - (1.0 - sketch) * uStrength, 0.0, 1.0);
    vec3 pencil = mix(vec3(sketch), clamp(c / max(g, 0.05), 0.0, 1.5) * sketch, uColor);
    fragColor = vec4(clamp(mix(c, pencil, uAmount), 0.0, 1.0) * s.a, s.a);
}
)",
                why, true))
            return false;
        m_strength = uniform(kit, "uStrength");
        m_color = uniform(kit, "uColor");
        m_amount = uniform(kit, "uAmount");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        const double radius = std::max(kMinRadiusPx, val(p, k::Radius, f, 4) * in.scale);
        PooledTarget blurred = kit.gaussianBlur(*in.layer, radius * kSigmaPerRadius);
        if (!blurred)
            return {};
        GlFunctions* gl = kit.gl();
        PooledTarget out = begin(kit, *in.layer);
        // Strength 100 % = 3 x the dodge's line darkness (the plain dodge is very faint).
        gl->glUniform1f(
            m_strength, float(3.0 * std::max(0.0, val(p, k::Strength, f, 100)) / 100.0));
        gl->glUniform1f(m_color, float(std::clamp(val(p, k::ColorAmount, f, 0) / 100.0, 0.0, 1.0)));
        gl->glUniform1f(m_amount, float(std::clamp(val(p, k::Amount, f, 100) / 100.0, 0.0, 1.0)));
        draw(kit, *in.layer, blurred->texture);
        return out;
    }

private:
    GLint m_strength = -1, m_color = -1, m_amount = -1;
};

// ---- abstraction ----

class Abstraction final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "abstraction", R"(
uniform sampler2D uTex;
uniform vec2 uTexel;
uniform float uRadius; // texels
uniform float uLevels;
uniform float uEdges;
void main()
{
    // Kuwahara filter: the mean of the quadrant (around the pixel) with the least variance.
    // Flat regions become smooth patches, edges stay sharp. 4 x 4 samples per quadrant,
    // spread over the radius, keep the cost fixed.
    vec2 step = uTexel * uRadius / 3.0;
    vec4 best = texture(uTex, vUv);
    float bestVar = 1e9;
    for (int q = 0; q < 4; ++q) {
        vec2 dir = vec2(q == 0 || q == 3 ? -1.0 : 1.0, q < 2 ? -1.0 : 1.0);
        vec4 sum = vec4(0.0);
        float sum2 = 0.0;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                vec4 c = texture(uTex, vUv + dir * vec2(x, y) * step);
                float l = luma(c.rgb);
                sum += c;
                sum2 += l * l;
            }
        }
        vec4 mean = sum / 16.0;
        float m = luma(mean.rgb);
        float var = sum2 / 16.0 - m * m;
        if (var < bestVar) {
            bestVar = var;
            best = mean;
        }
    }
    if (best.a <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    vec3 c = best.rgb / best.a;
    c = min(floor(clamp(c, 0.0, 1.0) * uLevels), uLevels - 1.0) / (uLevels - 1.0);
    // Dark outlines where the luma gradient is strong.
    float l = luma(straight(texture(uTex, vUv + vec2(uTexel.x, 0.0))))
        - luma(straight(texture(uTex, vUv - vec2(uTexel.x, 0.0))));
    float m = luma(straight(texture(uTex, vUv + vec2(0.0, uTexel.y))))
        - luma(straight(texture(uTex, vUv - vec2(0.0, uTexel.y))));
    float edge = smoothstep(0.08, 0.3, length(vec2(l, m)));
    c *= 1.0 - uEdges * edge;
    fragColor = vec4(c * best.a, best.a);
}
)",
                why, true))
            return false;
        m_texel = uniform(kit, "uTexel");
        m_radius = uniform(kit, "uRadius");
        m_levels = uniform(kit, "uLevels");
        m_edges = uniform(kit, "uEdges");
        return true;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        GlFunctions* gl = kit.gl();
        PooledTarget out = begin(kit, *in.layer);
        gl->glUniform2f(
            m_texel, 1.f / float(in.layer->size.width()), 1.f / float(in.layer->size.height()));
        gl->glUniform1f(m_radius, float(std::max(1.0, val(p, k::Radius, f, 4) * in.scale)));
        gl->glUniform1f(m_levels, float(std::max(2.0, std::round(val(p, k::Levels, f, 8)))));
        gl->glUniform1f(m_edges, float(std::clamp(val(p, k::Edges, f, 40) / 100.0, 0.0, 1.0)));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_texel = -1, m_radius = -1, m_levels = -1, m_edges = -1;
};

// ---- prism ----

class Prism final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = std::string(R"(
uniform sampler2D uTex;
uniform vec2 uSpan;  // whole spread in uv
const int kTaps = )")
            + std::to_string(kPrismTaps) + R"(;
uniform vec3 uWeights[kTaps]; // spectral weights, each channel sums to 1
void main()
{
    vec3 rgb = vec3(0.0);
    float a = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        float t = (float(i) + 0.5) / float(kTaps) - 0.5;
        vec4 c = texture(uTex, vUv + uSpan * t);
        rgb += c.rgb * uWeights[i];
        a += c.a;
    }
    a /= float(kTaps);
    fragColor = vec4(min(rgb, vec3(a)), a);
}
)";
        if (!build(kit, "prism", source.c_str(), why))
            return false;
        m_span = uniform(kit, "uSpan");
        m_weights = uniform(kit, "uWeights");
        // Spectral weights: tap t gets the colour of hue 0 (red) .. 270 degrees (violet),
        // normalised so a uniform picture keeps its colour.
        float w[kPrismTaps * 3];
        float sum[3] = {0, 0, 0};
        for (int i = 0; i < kPrismTaps; ++i) {
            const float t = (i + 0.5f) / kPrismTaps;
            const fxmath::Rgb c = fxmath::fromHsl(270.f * t, 1.f, 0.5f);
            w[i * 3] = c.r;
            w[i * 3 + 1] = c.g;
            w[i * 3 + 2] = c.b;
            sum[0] += c.r;
            sum[1] += c.g;
            sum[2] += c.b;
        }
        for (int i = 0; i < kPrismTaps * 3; ++i)
            w[i] /= sum[i % 3];
        kit.gl()->glUseProgram(m_program.id);
        kit.gl()->glUniform3fv(m_weights, kPrismTaps, w);
        kit.gl()->glUseProgram(0);
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 12) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const double spread = val(*in.params, k::Amount, in.localFrame, 12) * in.scale;
        if (spread < kMinRadiusPx)
            return {};
        const double a = val(*in.params, k::Angle, in.localFrame, 0) * std::numbers::pi / 180.0;
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform2f(m_span, float(std::cos(a) * spread / in.layer->size.width()),
            float(-std::sin(a) * spread / in.layer->size.height()));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_span = -1, m_weights = -1;
};

const GlEffectFactory kLookTable[] = {
    {fxid::Pixelate, &make<Pixelate>},
    {fxid::Halftone, &make<Halftone>},
    {fxid::Scanlines, &make<Scanlines>},
    {fxid::PencilSketch, &make<PencilSketch>},
    {fxid::Abstraction, &make<Abstraction>},
    {fxid::Prism, &make<Prism>},
};

} // namespace

std::span<const GlEffectFactory> lookGlEffects()
{
    return kLookTable;
}

} // namespace kk::render
