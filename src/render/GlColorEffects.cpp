// SPDX-License-Identifier: GPL-3.0-or-later
// GlColorEffects — the colour effects on the GPU: Exposure, Temperature / Tint, Levels,
// Invert, Black and White, Posterize, Color Balance. Each is one pass over the straight
// colour (alpha unchanged); the formulas are the twins of render/EffectMath.cpp and the
// uniforms come from the same fxmath::*Params resolution.
#include "core/VideoEffectKeys.h"
#include "render/GlEffectBase.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

/// Shared main(): un-premultiply, apply(), clamp, premultiply. Transparent stays transparent.
const char* const kColorMain = R"(
uniform sampler2D uTex;
vec3 apply(vec3 c);
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    fragColor = vec4(clamp(apply(straight(t)), 0.0, 1.0) * t.a, t.a);
}
)";

/// A pointwise colour effect: GLSL `vec3 apply(vec3 c)` plus up to four uniforms, filled by
/// a per-effect setter from the ParamSet.
class ColorEffect final : public PassEffect
{
public:
    static constexpr int kMaxUniforms = 4;
    using Setter = void (*)(GlFunctions* gl, const GLint* loc, const EffectInput& in);
    using Identity = bool (*)(const ParamSet& p, int64_t frame);

    ColorEffect(const char* name, const char* body, std::array<const char*, kMaxUniforms> names,
        Setter setter, Identity identity)
        : m_name(name)
        , m_body(body)
        , m_names(names)
        , m_setter(setter)
        , m_identity(identity)
    { }

    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = std::string(kColorMain) + m_body;
        if (!build(kit, m_name, source.c_str(), why, true))
            return false;
        for (int i = 0; i < kMaxUniforms; ++i)
            m_loc[i] = m_names[size_t(i)] ? uniform(kit, m_names[size_t(i)]) : -1;
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return m_identity && m_identity(p, f);
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        PooledTarget out = begin(kit, *in.layer);
        m_setter(kit.gl(), m_loc, in);
        draw(kit, *in.layer);
        return out;
    }

private:
    const char* m_name;
    const char* m_body;
    std::array<const char*, kMaxUniforms> m_names;
    Setter m_setter;
    Identity m_identity;
    GLint m_loc[kMaxUniforms] = {-1, -1, -1, -1};
};

double val(const ParamSet& p, const QString& key, int64_t f, double fallback)
{
    return p.value(key, f, fallback);
}

// ---- exposure ----

const char* const kExposure = R"(
uniform float uGain;
uniform float uInvGamma;
vec3 apply(vec3 c)
{
    vec3 lin = pow(max(c, 0.0), vec3(2.2)) * uGain;
    return pow(clamp(pow(lin, vec3(1.0 / 2.2)), 0.0, 1.0), vec3(uInvGamma));
}
)";

std::unique_ptr<GlEffect> makeExposure()
{
    return std::make_unique<ColorEffect>(
        "exposure", kExposure, std::array<const char*, 4> {"uGain", "uInvGamma"},
        [](GlFunctions* gl, const GLint* loc, const EffectInput& in) {
            const fxmath::ExposureParams p = fxmath::exposureParams(*in.params, in.localFrame);
            gl->glUniform1f(loc[0], p.gain);
            gl->glUniform1f(loc[1], p.invGamma);
        },
        [](const ParamSet& p, int64_t f) {
            return val(p, k::Exposure, f, 0) == 0.0 && val(p, k::Gamma, f, 1) == 1.0;
        });
}

// ---- temperature_tint ----

const char* const kLinearGains = R"(
uniform vec3 uGains;
vec3 apply(vec3 c)
{
    vec3 lin = pow(max(c, 0.0), vec3(2.2)) * uGains;
    return pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.2));
}
)";

std::unique_ptr<GlEffect> makeTemperatureTint()
{
    return std::make_unique<ColorEffect>(
        "temperature_tint", kLinearGains, std::array<const char*, 4> {"uGains"},
        [](GlFunctions* gl, const GLint* loc, const EffectInput& in) {
            const fxmath::GainParams p = fxmath::temperatureTintParams(*in.params, in.localFrame);
            gl->glUniform3f(loc[0], p.r, p.g, p.b);
        },
        [](const ParamSet& p, int64_t f) {
            return val(p, k::Temperature, f, 0) == 0.0 && val(p, k::Tint, f, 0) == 0.0;
        });
}

// ---- levels ----

const char* const kLevels = R"(
uniform vec3 uIn;  // black, range (white - black, > 0), 1 / gamma
uniform vec2 uOut; // black, white
vec3 apply(vec3 c)
{
    vec3 t = pow(clamp((c - uIn.x) / uIn.y, 0.0, 1.0), vec3(uIn.z));
    return uOut.x + t * (uOut.y - uOut.x);
}
)";

std::unique_ptr<GlEffect> makeLevels()
{
    return std::make_unique<ColorEffect>(
        "levels", kLevels, std::array<const char*, 4> {"uIn", "uOut"},
        [](GlFunctions* gl, const GLint* loc, const EffectInput& in) {
            const fxmath::LevelsParams p = fxmath::levelsParams(*in.params, in.localFrame);
            gl->glUniform3f(
                loc[0], p.inBlack, std::max(p.inWhite - p.inBlack, 1.f / 255.f), p.invGamma);
            gl->glUniform2f(loc[1], p.outBlack, p.outWhite);
        },
        [](const ParamSet& p, int64_t f) {
            return val(p, k::InBlack, f, 0) == 0.0 && val(p, k::InWhite, f, 255) == 255.0
                && val(p, k::Gamma, f, 1) == 1.0 && val(p, k::OutBlack, f, 0) == 0.0
                && val(p, k::OutWhite, f, 255) == 255.0;
        });
}

// ---- invert ----

const char* const kInvert = R"(
uniform float uAmount;
vec3 apply(vec3 c) { return mix(c, 1.0 - c, uAmount); }
)";

std::unique_ptr<GlEffect> makeInvert()
{
    return std::make_unique<ColorEffect>(
        "invert", kInvert, std::array<const char*, 4> {"uAmount"},
        [](GlFunctions* gl, const GLint* loc, const EffectInput& in) {
            gl->glUniform1f(loc[0],
                float(
                    std::clamp(val(*in.params, k::Amount, in.localFrame, 100) / 100.0, 0.0, 1.0)));
        },
        [](const ParamSet& p, int64_t f) { return val(p, k::Amount, f, 100) <= 0.0; });
}

// ---- black_white ----

const char* const kBlackWhite = R"(
uniform float uAmount;
uniform vec3 uTint; // tint colour / its luma
uniform float uTintAmount;
vec3 apply(vec3 c)
{
    float y = luma(c);
    vec3 tinted = clamp(y * uTint, 0.0, 1.0);
    return mix(c, mix(vec3(y), tinted, uTintAmount), uAmount);
}
)";

std::unique_ptr<GlEffect> makeBlackWhite()
{
    return std::make_unique<ColorEffect>(
        "black_white", kBlackWhite, std::array<const char*, 4> {"uAmount", "uTint", "uTintAmount"},
        [](GlFunctions* gl, const GLint* loc, const EffectInput& in) {
            const fxmath::BlackWhiteParams p = fxmath::blackWhiteParams(*in.params, in.localFrame);
            gl->glUniform1f(loc[0], p.amount);
            gl->glUniform3f(loc[1], p.tint.r, p.tint.g, p.tint.b);
            gl->glUniform1f(loc[2], p.tintAmount);
        },
        [](const ParamSet& p, int64_t f) { return val(p, k::Amount, f, 100) <= 0.0; });
}

// ---- posterize ----

const char* const kPosterize = R"(
uniform float uLevels; // >= 2
vec3 apply(vec3 c)
{
    return min(floor(clamp(c, 0.0, 1.0) * uLevels), uLevels - 1.0) / (uLevels - 1.0);
}
)";

std::unique_ptr<GlEffect> makePosterize()
{
    return std::make_unique<ColorEffect>(
        "posterize", kPosterize, std::array<const char*, 4> {"uLevels"},
        [](GlFunctions* gl, const GLint* loc, const EffectInput& in) {
            const double n
                = std::max(2.0, std::round(val(*in.params, k::Levels, in.localFrame, 6)));
            gl->glUniform1f(loc[0], float(n));
        },
        nullptr);
}

// ---- color_balance ----

const char* const kColorBalance = R"(
uniform vec3 uLift;
uniform vec3 uInvGamma;
uniform vec3 uGain;
vec3 apply(vec3 c)
{
    vec3 t = uGain * (c + uLift * (1.0 - c));
    return pow(clamp(t, 0.0, 1.0), uInvGamma);
}
)";

std::unique_ptr<GlEffect> makeColorBalance()
{
    return std::make_unique<ColorEffect>(
        "color_balance", kColorBalance, std::array<const char*, 4> {"uLift", "uInvGamma", "uGain"},
        [](GlFunctions* gl, const GLint* loc, const EffectInput& in) {
            const fxmath::ColorBalanceParams p
                = fxmath::colorBalanceParams(*in.params, in.localFrame);
            gl->glUniform3fv(loc[0], 1, p.lift);
            gl->glUniform3fv(loc[1], 1, p.invGamma);
            gl->glUniform3fv(loc[2], 1, p.gain);
        },
        [](const ParamSet& p, int64_t f) {
            for (const QString* key : {&k::Lift, &k::LiftR, &k::LiftG, &k::LiftB, &k::Gamma,
                     &k::GammaR, &k::GammaG, &k::GammaB, &k::GainR, &k::GainG, &k::GainB}) {
                if (val(p, *key, f, 0) != 0.0)
                    return false;
            }
            return val(p, k::Gain, f, 100) == 100.0;
        });
}

const GlEffectFactory kColorTable[] = {
    {fxid::Exposure, &makeExposure},
    {fxid::TemperatureTint, &makeTemperatureTint},
    {fxid::Levels, &makeLevels},
    {fxid::Invert, &makeInvert},
    {fxid::BlackWhite, &makeBlackWhite},
    {fxid::Posterize, &makePosterize},
    {fxid::ColorBalance, &makeColorBalance},
};

} // namespace

std::span<const GlEffectFactory> colorGlEffects()
{
    return kColorTable;
}

} // namespace kk::render
