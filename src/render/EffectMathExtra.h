// SPDX-License-Identifier: GPL-3.0-or-later
// EffectMathExtra — CPU reference of the 1.2 pointwise colour effects (Despill, Gamut Limiter;
// spec 56) and of the formulas the spatial ones share with their shaders (Fast Noise blend,
// Transform / Dent source positions). Every formula has a GLSL twin in GlExtraColor.cpp or
// GlExtraEffects.cpp; keep them in step (tests/render/test_effectsgap.cpp compares them).
#pragma once

#include "render/EffectMath.h"

namespace kk::render::fxmath {

// ---- Despill ----

/// Removes the cast of one channel: that channel is limited to the mean (or maximum) of the
/// other two, by `amount`.
struct DespillParams
{
    int channel = 1; ///< 0 red, 1 green, 2 blue
    float amount = 1.f; ///< 0..1
    bool maximum = false; ///< limit to the larger of the other two instead of their mean
};
[[nodiscard]] DespillParams despillParams(const ParamSet& p, int64_t localFrame);
[[nodiscard]] Rgb despillColor(const DespillParams& p, const Rgb& c);

// ---- Gamut Limiter ----

/// The picture's RGB is read as @c source primaries with a plain power-law transfer
/// (@c gamma), converted to the @c target primaries in linear light, brought inside
/// 0..1 there (by moving toward its luminance until every channel fits, or by clipping) and
/// converted back. Colours already inside the target gamut are returned untouched.
struct GamutLimiterParams
{
    float toTarget[9] {}; ///< row-major linear RGB(source) -> RGB(target)
    float fromTarget[9] {}; ///< row-major linear RGB(target) -> RGB(source)
    float lumaTarget[3] {}; ///< luminance weights of the target primaries
    float gamma = 2.4f;
    bool desaturate = true; ///< false = clip each channel
    float amount = 1.f; ///< 0..1 mix with the unlimited colour
};
[[nodiscard]] GamutLimiterParams gamutLimiterParams(const ParamSet& p, int64_t localFrame);
[[nodiscard]] Rgb gamutLimit(const GamutLimiterParams& p, const Rgb& c);

// ---- Fast Noise ----

/// Blends noise value @p n (0..1, already mapped to the two colours as @p noise) over the
/// picture colour @p base: mode "normal", "add", "multiply", "screen" or "overlay" (an index
/// 0..4 in that order), mixed by @p amount.
[[nodiscard]] Rgb blendNoise(int mode, float amount, const Rgb& base, const Rgb& noise);

// ---- False Color ----

/// The brightness (BT.709 luma of the gamma-encoded picture) mapped to colours: either flat
/// bands (each stop holds its colour until the next one; a stop whose `grey` is 1 passes the
/// picture's own grey through) or a smooth ramp between the stops.
struct FalseColorParams
{
    static constexpr int kMaxStops = 10;
    int count = 0;
    bool banded = false;
    float amount = 1.f; ///< 0..1 mix with the picture
    float pos[kMaxStops] {};
    float rgb[kMaxStops][3] {};
    float grey[kMaxStops] {};
};
[[nodiscard]] FalseColorParams falseColorParams(const ParamSet& p, int64_t localFrame);
[[nodiscard]] Rgb falseColor(const FalseColorParams& p, const Rgb& c);

namespace detail {
/// True for the types applyExtraEffect() implements.
[[nodiscard]] bool isExtraEffect(const QString& type);
/// The 1.2 part of applyPointEffect() (@p image is ARGB32_Premultiplied). @return false for
/// other types.
bool applyExtraEffect(
    const QString& type, const ParamSet& params, int64_t localFrame, QImage& image);
} // namespace detail

} // namespace kk::render::fxmath
