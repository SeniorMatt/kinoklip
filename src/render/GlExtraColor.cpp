// SPDX-License-Identifier: GPL-3.0-or-later
// GlExtraColor — 1.2 colour effects on the GPU: Despill, Gamut Limiter (pointwise, with CPU
// references in EffectMathExtra.cpp) and Contrast Pop (local contrast from a wide blur).
#include "core/VideoEffectKeys.h"
#include "render/EffectMathExtra.h"
#include "render/GlEffectBase.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

double val(const ParamSet& p, const QString& key, int64_t f, double fallback)
{
    return p.value(key, f, fallback);
}

// ---- despill ----

const char* const kDespill = R"(
uniform int uChannel;   // 0 red, 1 green, 2 blue
uniform float uAmount;  // 0..1
uniform int uMaximum;   // 1 = limit to the larger of the other two channels
vec3 apply(vec3 c)
{
    float a = c[(uChannel + 1) % 3];
    float b = c[(uChannel + 2) % 3];
    float limit = uMaximum == 1 ? max(a, b) : 0.5 * (a + b);
    c[uChannel] -= uAmount * max(0.0, c[uChannel] - limit);
    return c;
}
)";

std::unique_ptr<GlEffect> makeDespill()
{
    return std::make_unique<ColorEffect>(
        "despill", kDespill, ColorEffect::Names {"uChannel", "uAmount", "uMaximum"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const fxmath::DespillParams p = fxmath::despillParams(*in.params, in.localFrame);
            kit.gl()->glUniform1i(loc[0], p.channel);
            kit.gl()->glUniform1f(loc[1], p.amount);
            kit.gl()->glUniform1i(loc[2], p.maximum ? 1 : 0);
        },
        [](const ParamSet& p, int64_t f) { return val(p, k::Amount, f, 100) <= 0.0; });
}

// ---- gamut limiter ----

const char* const kGamutLimiter = R"(
uniform mat3 uTo;       // linear RGB(picture gamut) -> RGB(limit gamut)
uniform mat3 uFrom;     // and back
uniform vec3 uLumaT;    // luminance weights of the limit gamut
uniform vec3 uOpt;      // gamma, method (1 desaturate, 0 clip), amount
vec3 apply(vec3 c)
{
    vec3 lin = pow(max(c, vec3(0.0)), vec3(uOpt.x));
    vec3 t = uTo * lin;
    if (all(greaterThanEqual(t, vec3(0.0))) && all(lessThanEqual(t, vec3(1.0))))
        return c;
    vec3 r;
    if (uOpt.y > 0.5) {
        float y = clamp(dot(uLumaT, t), 0.0, 1.0);
        float s = 1.0;
        for (int i = 0; i < 3; ++i) {
            if (t[i] < 0.0)
                s = min(s, y / (y - t[i]));
            else if (t[i] > 1.0)
                s = min(s, (1.0 - y) / (t[i] - y));
        }
        r = y + s * (t - y);
    } else {
        r = clamp(t, 0.0, 1.0);
    }
    vec3 back = pow(clamp(uFrom * r, 0.0, 1.0), vec3(1.0 / uOpt.x));
    return mix(c, back, uOpt.z);
}
)";

std::unique_ptr<GlEffect> makeGamutLimiter()
{
    return std::make_unique<ColorEffect>(
        "gamut_limiter", kGamutLimiter, ColorEffect::Names {"uTo", "uFrom", "uLumaT", "uOpt"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const fxmath::GamutLimiterParams p
                = fxmath::gamutLimiterParams(*in.params, in.localFrame);
            kit.gl()->glUniformMatrix3fv(loc[0], 1, GL_TRUE, p.toTarget); // rows -> transpose
            kit.gl()->glUniformMatrix3fv(loc[1], 1, GL_TRUE, p.fromTarget);
            kit.gl()->glUniform3fv(loc[2], 1, p.lumaTarget);
            kit.gl()->glUniform3f(loc[3], p.gamma, p.desaturate ? 1.f : 0.f, p.amount);
        },
        [](const ParamSet& p, int64_t f) { return val(p, k::Amount, f, 100) <= 0.0; });
}

// ---- false color ----

const char* const kFalseColor = R"(
uniform vec4 uStops[10]; // rgb, grey flag
uniform float uPos[10];
uniform vec3 uOpt;       // stop count, banded (1) or smooth (0), amount
vec3 apply(vec3 c)
{
    float y = clamp(luma(c), 0.0, 1.0);
    int count = int(uOpt.x);
    vec3 o;
    if (uOpt.y > 0.5) {
        int i = 0;
        for (int n = 1; n < count; ++n)
            i = y >= uPos[n] ? n : i;
        o = uStops[i].w > 0.5 ? vec3(y) : uStops[i].rgb;
    } else {
        int i = 0;
        for (int n = 1; n < count - 1; ++n)
            i = y >= uPos[n] ? n : i;
        float t = clamp((y - uPos[i]) / (uPos[i + 1] - uPos[i]), 0.0, 1.0);
        o = mix(uStops[i].rgb, uStops[i + 1].rgb, t);
    }
    return mix(c, o, uOpt.z);
}
)";

std::unique_ptr<GlEffect> makeFalseColor()
{
    return std::make_unique<ColorEffect>(
        "false_color", kFalseColor, ColorEffect::Names {"uStops", "uPos", "uOpt"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const fxmath::FalseColorParams p = fxmath::falseColorParams(*in.params, in.localFrame);
            float stops[fxmath::FalseColorParams::kMaxStops * 4] {};
            for (int i = 0; i < p.count; ++i) {
                stops[i * 4] = p.rgb[i][0];
                stops[i * 4 + 1] = p.rgb[i][1];
                stops[i * 4 + 2] = p.rgb[i][2];
                stops[i * 4 + 3] = p.grey[i];
            }
            kit.gl()->glUniform4fv(loc[0], fxmath::FalseColorParams::kMaxStops, stops);
            kit.gl()->glUniform1fv(loc[1], fxmath::FalseColorParams::kMaxStops, p.pos);
            kit.gl()->glUniform3f(loc[2], float(p.count), p.banded ? 1.f : 0.f, p.amount);
        },
        [](const ParamSet& p, int64_t f) { return val(p, k::Amount, f, 100) <= 0.0; });
}

// ---- dehaze ----

/// Dark channel prior (He, Sun, Tang, 2009, a published method): in a haze-free area at least one
/// colour channel is dark somewhere, so the darkest value of a patch (after dividing by the
/// haze colour A) measures the haze there: transmission t = 1 - strength * dark, and the
/// picture is restored as J = (I - A) / max(t, 0.1) + A. The dark channel is taken with the
/// morphology (minimum) filter over the patch and softened with a blur so edges do not halo.
class Dehaze final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        // Mode 0: the haze estimate per pixel (min over channels of c / A) in every channel;
        // mode 1: restore with the transmission from uSecond.
        if (!build(kit, "dehaze", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform int uMode;
uniform vec3 uHaze;     // haze (atmospheric light) colour, straight
uniform float uStrength; // 0..0.95
void main()
{
    vec4 t = texture(uTex, vUv);
    vec3 c = straight(t);
    if (uMode == 0) {
        vec3 n = c / max(uHaze, vec3(0.02));
        fragColor = vec4(vec3(min(n.r, min(n.g, n.b))), 1.0);
        return;
    }
    float dark = texture(uSecond, vUv).r;
    float tr = max(1.0 - uStrength * dark, 0.1);
    vec3 j = (c - uHaze) / tr + uHaze;
    fragColor = vec4(clamp(j, 0.0, 1.0) * t.a, t.a);
}
)",
                why, true))
            return false;
        m_mode = uniform(kit, "uMode");
        m_haze = uniform(kit, "uHaze");
        m_strength = uniform(kit, "uStrength");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 60) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const fxmath::Rgb haze = colorParam(p, k::HazeColor, {0.85f, 0.86f, 0.89f});
        const double strength
            = 0.95 * std::clamp(val(p, k::Amount, in.localFrame, 60), 0.0, 100.0) / 100.0;
        PooledTarget dark = begin(kit, *in.layer);
        kit.gl()->glUniform1i(m_mode, 0);
        kit.gl()->glUniform3f(m_haze, haze.r, haze.g, haze.b);
        kit.gl()->glUniform1f(m_strength, float(strength));
        draw(kit, *in.layer);
        const double patch = std::max(0.0, val(p, k::Radius, in.localFrame, 15) * in.scale);
        if (patch >= 0.5) {
            if (PooledTarget eroded = kit.morphology(*dark, patch, false))
                dark = std::move(eroded);
        }
        const double soft = val(p, k::Softness, in.localFrame, 8) * in.scale * kSigmaPerRadius;
        if (soft >= 0.25) {
            if (PooledTarget blurred = kit.gaussianBlur(*dark, soft))
                dark = std::move(blurred);
        }
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform1i(m_mode, 1);
        kit.gl()->glUniform3f(m_haze, haze.r, haze.g, haze.b);
        kit.gl()->glUniform1f(m_strength, float(strength));
        draw(kit, *in.layer, dark->texture);
        return out;
    }

private:
    GLint m_mode = -1, m_haze = -1, m_strength = -1;
};

// ---- contrast pop ----

/// Local contrast: the difference between a pixel's luma and the luma of a wide blur of its
/// surroundings is boosted, weighted toward the midtones so highlights and shadows do not clip,
/// and the colour is scaled with the luma so hues hold. Saturation is an optional extra.
class ContrastPop final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "contrast_pop", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond; // wide blur of the layer
uniform float uAmount;     // gain of the local difference (1 = +100 %)
uniform float uSaturation; // -1..1
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    vec4 bt = texture(uSecond, vUv);
    vec3 c = straight(t);
    float y = luma(c);
    float yb = bt.a > 0.0 ? luma(bt.rgb / bt.a) : y;
    float w = 1.0 - 0.5 * abs(2.0 * y - 1.0); // 1 in the midtones, 0.5 at black / white
    float target = max(y + uAmount * w * (y - yb), 0.0);
    vec3 o = c * (target / max(y, 1e-4));
    if (y < 1e-4)
        o = vec3(target);
    float yo = luma(o);
    o = mix(vec3(yo), o, 1.0 + uSaturation);
    fragColor = vec4(clamp(o, 0.0, 1.0) * t.a, t.a);
}
)",
                why, true))
            return false;
        m_amount = uniform(kit, "uAmount");
        m_saturation = uniform(kit, "uSaturation");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 60) == 0.0 && val(p, k::Saturation, f, 0) == 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const double radius
            = std::max(kMinRadiusPx, val(p, k::Radius, in.localFrame, 40) * in.scale);
        PooledTarget blurred = kit.gaussianBlur(*in.layer, radius * kSigmaPerRadius);
        if (!blurred)
            return {};
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform1f(m_amount, float(val(p, k::Amount, in.localFrame, 60) / 100.0));
        kit.gl()->glUniform1f(m_saturation, float(val(p, k::Saturation, in.localFrame, 0) / 100.0));
        draw(kit, *in.layer, blurred->texture);
        return out;
    }

private:
    GLint m_amount = -1, m_saturation = -1;
};

const GlEffectFactory kTable[] = {
    {fxid::Despill, &makeDespill},
    {fxid::GamutLimiter, &makeGamutLimiter},
    {fxid::ContrastPop, &make<ContrastPop>},
    {fxid::FalseColor, &makeFalseColor},
    {fxid::Dehaze, &make<Dehaze>},
};

} // namespace

std::span<const GlEffectFactory> extraColorGlEffects()
{
    return kTable;
}

} // namespace kk::render
