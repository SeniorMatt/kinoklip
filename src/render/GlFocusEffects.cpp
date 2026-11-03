// SPDX-License-Identifier: GPL-3.0-or-later
// GlFocusEffects — Unsharp Mask, Tilt-Shift, Lens Blur and Noise Reduction on the GPU.
//
// Costs (per pixel, bounded for every parameter value): unsharp mask = a Gaussian blur
// (GlKit, downscaled for large radii) + 1 pass; tilt-shift = two Gaussian blurs + 1 pass;
// lens blur = kBokehTaps taps on a copy halved until the radius is at most kMaxBokehRadius
// texels (so 80 taps on at most a quarter of the pixels from a 6 px radius on); noise reduction = a
// (2r + 1)² bilateral kernel, r <= 4 (81 taps).
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

/// Taps of the lens blur disc (a Vogel spiral: even coverage without a visible grid).
constexpr int kBokehTaps = 80;
/// Largest disc radius sampled directly (texels); larger radii run on a halved copy.
constexpr double kMaxBokehRadius = 6.0;
/// Largest noise reduction radius (layer px) — the kernel is (2r + 1)².
constexpr int kMaxDenoiseRadius = 4;

// ---- unsharp_mask ----

class UnsharpMask final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "unsharp_mask", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond; // blurred copy
uniform float uAmount;
uniform float uThreshold;  // luma difference below which nothing is sharpened
void main()
{
    vec4 s = texture(uTex, vUv);
    vec4 b = texture(uSecond, vUv);
    vec3 d = s.rgb - b.rgb;
    // Soft threshold: differences below it (noise, flat areas) fade out over 1/50.
    float w = uThreshold > 0.0 ? smoothstep(uThreshold, uThreshold + 0.02, abs(luma(d))) : 1.0;
    fragColor = vec4(clamp(s.rgb + uAmount * w * d, 0.0, s.a), s.a);
}
)",
                why, true))
            return false;
        m_amount = uniform(kit, "uAmount");
        m_threshold = uniform(kit, "uThreshold");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const double radius
            = std::max(kMinRadiusPx, val(p, k::Radius, in.localFrame, 2) * in.scale);
        PooledTarget blurred = kit.gaussianBlur(*in.layer, radius * kSigmaPerRadius);
        if (!blurred)
            return {};
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform1f(m_amount, float(val(p, k::Amount, in.localFrame, 100) / 100.0));
        kit.gl()->glUniform1f(m_threshold, float(val(p, k::Threshold, in.localFrame, 0) / 100.0));
        draw(kit, *in.layer, blurred->texture);
        return out;
    }

private:
    GLint m_amount = -1, m_threshold = -1;
};

// ---- tilt_shift ----

class TiltShift final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "tilt_shift", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond; // strong blur
uniform sampler2D uThird;  // light blur (a third of the radius)
uniform vec2 uSize;        // layer px
uniform vec2 uLine;        // a point on the focus line (uv)
uniform vec2 uNormal;      // unit normal of the focus line (px space)
uniform vec2 uBand;        // half focus width, transition (px, > 0)
void main()
{
    float d = abs(dot((vUv - uLine) * uSize, uNormal));
    float m = smoothstep(uBand.x, uBand.x + uBand.y, d);
    vec4 s = texture(uTex, vUv);
    vec4 light = texture(uThird, vUv);
    vec4 strong = texture(uSecond, vUv);
    // Three levels so the blur grows gradually away from the band.
    fragColor = m < 0.5 ? mix(s, light, m * 2.0) : mix(light, strong, m * 2.0 - 1.0);
}
)",
                why))
            return false;
        GlFunctions* gl = kit.gl();
        gl->glUseProgram(m_program.id);
        gl->glUniform1i(uniform(kit, "uThird"), 2);
        gl->glUseProgram(0);
        m_size = uniform(kit, "uSize");
        m_line = uniform(kit, "uLine");
        m_normal = uniform(kit, "uNormal");
        m_band = uniform(kit, "uBand");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Radius, f, 12) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        const double radius = val(p, k::Radius, f, 12) * in.scale;
        if (radius < kMinRadiusPx)
            return {};
        PooledTarget strong = kit.gaussianBlur(*in.layer, radius * kSigmaPerRadius);
        PooledTarget light = kit.gaussianBlur(*in.layer, radius * kSigmaPerRadius / 3.0);
        if (!strong)
            return {};
        const LayerGeometry g = layerGeometry(in);
        float u = 0, v = 0;
        g.centre(in, 0, val(p, k::CenterY, f, 0), &u, &v);
        // Angle 0 = a horizontal band; positive turns it counter-clockwise on screen.
        const double a = val(p, k::Angle, f, 0) * std::numbers::pi / 180.0;
        GlFunctions* gl = kit.gl();
        PooledTarget out = begin(kit, *in.layer);
        gl->glUniform2f(m_size, g.width, g.height);
        gl->glUniform2f(m_line, u, v);
        gl->glUniform2f(m_normal, float(std::sin(a)), float(std::cos(a)));
        gl->glUniform2f(m_band,
            float(std::max(0.0, 0.5 * val(p, k::FocusWidth, f, 120) * in.scale)),
            float(std::max(1.0, val(p, k::Transition, f, 200) * in.scale)));
        kit.bindTexture(2, light ? light->texture : in.layer->texture);
        draw(kit, *in.layer, strong->texture);
        kit.bindTexture(2, 0);
        kit.gl()->glActiveTexture(GL_TEXTURE0);
        return out;
    }

private:
    GLint m_size = -1, m_line = -1, m_normal = -1, m_band = -1;
};

// ---- lens_blur ----

class LensBlur final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = std::string(R"(
uniform sampler2D uTex;
uniform vec2 uTexel;     // 1 / size of the sampled level
uniform float uRadius;   // disc radius in texels of that level
uniform vec2 uBoost;     // highlight weight boost, threshold (luma)
const int kTaps = )")
            + std::to_string(kBokehTaps) + R"(;
void main()
{
    // Vogel spiral: tap i at radius sqrt((i + 0.5) / n), angle i · golden angle.
    const float kGolden = 2.39996323;
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        float r = sqrt((float(i) + 0.5) / float(kTaps)) * uRadius;
        float a = float(i) * kGolden;
        vec4 c = texture(uTex, vUv + vec2(cos(a), sin(a)) * r * uTexel);
        // Bright taps weigh more: highlights spread into bright discs (bokeh), not grey haze.
        float w = 1.0 + uBoost.x * smoothstep(uBoost.y, 1.0, luma(straight(c)));
        sum += c * w;
        wsum += w;
    }
    fragColor = sum / wsum;
}
)";
        if (!build(kit, "lens_blur", source.c_str(), why, true))
            return false;
        m_texel = uniform(kit, "uTexel");
        m_radius = uniform(kit, "uRadius");
        m_boost = uniform(kit, "uBoost");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Radius, f, 10) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        double radius = val(p, k::Radius, in.localFrame, 10) * in.scale;
        if (radius < kMinRadiusPx)
            return {};
        // Halve (exact 2 x 2 box per step) until the disc fits the direct radius.
        PooledTarget scaled;
        const RenderTarget* level = in.layer;
        while (radius > kMaxBokehRadius && level->size.width() > 8 && level->size.height() > 8) {
            const QSize half((level->size.width() + 1) / 2, (level->size.height() + 1) / 2);
            scaled = kit.resample(*level, half);
            level = scaled.get();
            radius *= 0.5;
        }
        PooledTarget disc = begin(kit, *level);
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(
            m_texel, 1.f / float(level->size.width()), 1.f / float(level->size.height()));
        gl->glUniform1f(m_radius, float(radius));
        // Highlights 100 % → a highlight tap weighs 1 + 8 times a normal one.
        gl->glUniform2f(m_boost, float(8.0 * val(p, k::Highlights, in.localFrame, 40) / 100.0),
            float(std::clamp(val(p, k::Threshold, in.localFrame, 75) / 100.0, 0.0, 0.99)));
        draw(kit, *level);
        if (level == in.layer)
            return disc;
        return kit.resample(*disc, in.layer->size);
    }

private:
    GLint m_texel = -1, m_radius = -1, m_boost = -1;
};

// ---- noise_reduction ----

class NoiseReduction final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = std::string(R"(
uniform sampler2D uTex;
uniform vec2 uTexel;
uniform int uRadius;        // texels, 1..)")
            + std::to_string(kMaxDenoiseRadius) + R"(
uniform float uSpatial;     // 1 / (2 sigma_s^2)
uniform float uRange;       // 1 / (2 sigma_r^2)
uniform float uMix;
void main()
{
    // Bilateral filter: neighbours count by distance and by colour similarity, so edges
    // (large colour differences) keep their sharpness while flat noise averages out.
    vec4 centre = texture(uTex, vUv);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int y = -uRadius; y <= uRadius; ++y) {
        for (int x = -uRadius; x <= uRadius; ++x) {
            vec4 c = texture(uTex, vUv + vec2(x, y) * uTexel);
            vec4 d = c - centre;
            float w = exp(-float(x * x + y * y) * uSpatial - dot(d, d) * uRange);
            sum += c * w;
            wsum += w;
        }
    }
    fragColor = mix(centre, sum / wsum, uMix);
}
)";
        if (!build(kit, "noise_reduction", source.c_str(), why))
            return false;
        m_texel = uniform(kit, "uTexel");
        m_radius = uniform(kit, "uRadius");
        m_spatial = uniform(kit, "uSpatial");
        m_range = uniform(kit, "uRange");
        m_mix = uniform(kit, "uMix");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Strength, f, 40) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const double strength
            = std::clamp(val(p, k::Strength, in.localFrame, 40) / 100.0, 0.0, 1.0);
        const double radiusPx = std::clamp(
            val(p, k::Radius, in.localFrame, 2) * in.scale, 1.0, double(kMaxDenoiseRadius));
        const int radius = int(std::ceil(radiusPx - 1e-6));
        const double sigmaS = std::max(0.5, radiusPx * 0.6);
        // Strength 100 % treats colour differences up to ~0.15 as noise.
        const double sigmaR = 0.01 + 0.14 * strength;
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(
            m_texel, 1.f / float(in.layer->size.width()), 1.f / float(in.layer->size.height()));
        gl->glUniform1i(m_radius, radius);
        gl->glUniform1f(m_spatial, float(1.0 / (2.0 * sigmaS * sigmaS)));
        gl->glUniform1f(m_range, float(1.0 / (2.0 * sigmaR * sigmaR)));
        gl->glUniform1f(m_mix, float(std::min(1.0, 0.5 + strength)));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_texel = -1, m_radius = -1, m_spatial = -1, m_range = -1, m_mix = -1;
};

const GlEffectFactory kFocusTable[] = {
    {fxid::UnsharpMask, &make<UnsharpMask>},
    {fxid::TiltShift, &make<TiltShift>},
    {fxid::LensBlur, &make<LensBlur>},
    {fxid::NoiseReduction, &make<NoiseReduction>},
};

} // namespace

std::span<const GlEffectFactory> focusGlEffects()
{
    return kFocusTable;
}

} // namespace kk::render
