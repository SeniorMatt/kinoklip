// SPDX-License-Identifier: GPL-3.0-or-later
// GlDistortEffects — Lens Distortion, Ripple, Wave (Distort) and Feathered Crop (Transform)
// on the GPU: one pass each, a coordinate remap (or an alpha ramp) per pixel.
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

/// Seconds since the clip start (animated waves).
double seconds(const EffectInput& in)
{
    return in.fps > 0 ? double(in.localFrame) / in.fps : 0.0;
}

// ---- lens_distortion ----

class LensDistortion final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "lens_distortion", R"(
uniform sampler2D uTex;
uniform vec2 uCentre;   // uv
uniform vec2 uSize;     // layer px
uniform float uNorm;    // content half diagonal, px
uniform float uK;       // r^2 coefficient (positive = barrel)
uniform float uInvZoom;
uniform vec4 uContent;  // content rectangle in uv (x0, y0, x1, y1)
void main()
{
    vec2 p = (vUv - uCentre) * uSize;
    float r2 = dot(p, p) / (uNorm * uNorm);
    vec2 uv = uCentre + p * (1.0 + uK * r2) * uInvZoom / uSize;
    // Outside the picture is transparent (corners of a barrel distortion).
    if (any(lessThan(uv, uContent.xy)) || any(greaterThan(uv, uContent.zw)))
        fragColor = vec4(0.0);
    else
        fragColor = texture(uTex, uv);
}
)",
                why))
            return false;
        m_centre = uniform(kit, "uCentre");
        m_size = uniform(kit, "uSize");
        m_norm = uniform(kit, "uNorm");
        m_k = uniform(kit, "uK");
        m_invZoom = uniform(kit, "uInvZoom");
        m_content = uniform(kit, "uContent");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Distortion, f, 30) == 0.0 && val(p, k::Zoom, f, 100) == 100.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const LayerGeometry g = layerGeometry(in);
        float u = 0, v = 0;
        g.centre(in, 0, 0, &u, &v);
        const double norm = 0.5 * std::hypot((g.x1 - g.x0) * g.width, (g.y1 - g.y0) * g.height);
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(m_centre, u, v);
        gl->glUniform2f(m_size, g.width, g.height);
        gl->glUniform1f(m_norm, float(std::max(1.0, norm)));
        // Distortion ±100 → ±0.5 relative displacement at the corners.
        gl->glUniform1f(m_k, float(val(*in.params, k::Distortion, in.localFrame, 30) / 200.0));
        gl->glUniform1f(
            m_invZoom, float(100.0 / std::max(1.0, val(*in.params, k::Zoom, in.localFrame, 100))));
        gl->glUniform4f(m_content, g.x0, g.y0, g.x1, g.y1);
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_centre = -1, m_size = -1, m_norm = -1, m_k = -1, m_invZoom = -1, m_content = -1;
};

// ---- ripple / wave: one displacement shader ----

const char* const kWaveFragment = R"(
uniform sampler2D uTex;
uniform int uRadial;      // 1 = ripple (rings around uCentre), 0 = straight wave
uniform vec2 uCentre;     // uv
uniform vec2 uSize;       // layer px
uniform vec2 uDir;        // wave: direction of travel (unit)
uniform float uAmplitude; // px
uniform float uWavelength;// px
uniform float uPhase;     // radians (phase parameter + speed · time)
void main()
{
    const float kTau = 6.28318530718;
    vec2 p = (vUv - uCentre) * uSize;
    vec2 offset;
    if (uRadial == 1) {
        float r = length(p);
        vec2 n = r > 1e-4 ? p / r : vec2(0.0);
        offset = n * uAmplitude * sin(kTau * r / uWavelength - uPhase);
    } else {
        float s = dot(p, uDir);
        offset = vec2(-uDir.y, uDir.x) * uAmplitude * sin(kTau * s / uWavelength - uPhase);
    }
    fragColor = texture(uTex, vUv + offset / uSize);
}
)";

class WaveEffect final : public PassEffect
{
public:
    explicit WaveEffect(bool radial)
        : m_radial(radial)
    { }
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, m_radial ? "ripple" : "wave", kWaveFragment, why))
            return false;
        m_radialLoc = uniform(kit, "uRadial");
        m_centre = uniform(kit, "uCentre");
        m_size = uniform(kit, "uSize");
        m_dir = uniform(kit, "uDir");
        m_amplitude = uniform(kit, "uAmplitude");
        m_wavelength = uniform(kit, "uWavelength");
        m_phase = uniform(kit, "uPhase");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amplitude, f, 10) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const LayerGeometry g = layerGeometry(in);
        float u = 0, v = 0;
        if (m_radial)
            g.centre(in, val(p, k::CenterX, in.localFrame, 0), val(p, k::CenterY, in.localFrame, 0),
                &u, &v);
        else
            g.centre(in, 0, 0, &u, &v);
        const double amplitude = val(p, k::Amplitude, in.localFrame, 10) * in.scale;
        if (amplitude < 0.05)
            return {};
        const double defLength = m_radial ? 60 : 120;
        const double wavelength
            = std::max(1.0, val(p, k::Wavelength, in.localFrame, defLength) * in.scale);
        constexpr double kTau = 2.0 * std::numbers::pi;
        const double phase = val(p, k::Phase, in.localFrame, 0) * std::numbers::pi / 180.0
            + kTau * val(p, k::Speed, in.localFrame, 1) * seconds(in);
        const double a = val(p, k::Angle, in.localFrame, 0) * std::numbers::pi / 180.0;
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform1i(m_radialLoc, m_radial ? 1 : 0);
        gl->glUniform2f(m_centre, u, v);
        gl->glUniform2f(m_size, g.width, g.height);
        gl->glUniform2f(m_dir, float(std::cos(a)), float(-std::sin(a)));
        gl->glUniform1f(m_amplitude, float(amplitude));
        gl->glUniform1f(m_wavelength, float(wavelength));
        gl->glUniform1f(m_phase, float(std::fmod(phase, kTau)));
        draw(kit, *in.layer);
        return out;
    }

private:
    bool m_radial;
    GLint m_radialLoc = -1, m_centre = -1, m_size = -1, m_dir = -1, m_amplitude = -1,
          m_wavelength = -1, m_phase = -1;
};

// ---- feathered_crop ----

class FeatheredCrop final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "feathered_crop", R"(
uniform sampler2D uTex;
uniform vec4 uCrop;    // kept rectangle in uv (x0, y0, x1, y1)
uniform vec2 uFeather; // feather width in uv (>= one pixel)
float ramp(float d, float f) { return smoothstep(0.0, 1.0, clamp(d / f, 0.0, 1.0)); }
void main()
{
    float m = ramp(vUv.x - uCrop.x, uFeather.x) * ramp(uCrop.z - vUv.x, uFeather.x)
            * ramp(vUv.y - uCrop.y, uFeather.y) * ramp(uCrop.w - vUv.y, uFeather.y);
    fragColor = texture(uTex, vUv) * m;
}
)",
                why))
            return false;
        m_crop = uniform(kit, "uCrop");
        m_feather = uniform(kit, "uFeather");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        for (const QString* key : {&k::Left, &k::Right, &k::Top, &k::Bottom, &k::Feather}) {
            if (val(p, *key, f, *key == k::Feather ? 20 : 0) > 0.0)
                return false;
        }
        return true;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const LayerGeometry g = layerGeometry(in);
        auto pct = [&](const QString& key) {
            return std::clamp(val(p, key, in.localFrame, 0) / 100.0, 0.0, 1.0);
        };
        const double cw = g.x1 - g.x0, ch = g.y1 - g.y0;
        // The feather runs inwards from the crop line; at least one layer pixel (antialiasing).
        const double feather = std::max(1.0, val(p, k::Feather, in.localFrame, 20) * in.scale);
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform4f(m_crop, float(g.x0 + pct(k::Left) * cw), float(g.y0 + pct(k::Top) * ch),
            float(g.x1 - pct(k::Right) * cw), float(g.y1 - pct(k::Bottom) * ch));
        gl->glUniform2f(m_feather, float(feather / g.width), float(feather / g.height));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_crop = -1, m_feather = -1;
};

const GlEffectFactory kDistortTable[] = {
    {fxid::LensDistortion, &make<LensDistortion>},
    {fxid::Ripple, [] { return std::unique_ptr<GlEffect>(std::make_unique<WaveEffect>(true)); }},
    {fxid::Wave, [] { return std::unique_ptr<GlEffect>(std::make_unique<WaveEffect>(false)); }},
    {fxid::FeatheredCrop, &make<FeatheredCrop>},
};

} // namespace

std::span<const GlEffectFactory> distortGlEffects()
{
    return kDistortTable;
}

} // namespace kk::render
