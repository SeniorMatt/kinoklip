// SPDX-License-Identifier: GPL-3.0-or-later
// EffectMath — the per-pixel formulas of the v4 video effects on the CPU: the reference the
// GPU shaders are tested against (tests/render/test_videoeffects.cpp) and the CPU fallback's
// implementation of the pointwise effects (CpuRenderer). Each *Params struct is resolved
// once per frame from the effect's ParamSet; the GPU passes upload the same values as
// uniforms, so both paths share one parameter mapping.
#pragma once

#include "core/Animation.h"

#include <QImage>
#include <QString>

#include <cstdint>

namespace kk::render {
class CubeLut;
}

namespace kk::render::fxmath {

/// Straight (not premultiplied) gamma-encoded RGB, 0..1.
struct Rgb
{
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
};

/// BT.709 luma weights (the compositor's working space is gamma-encoded BT.709 RGB).
inline constexpr float kLumaR = 0.2126f;
inline constexpr float kLumaG = 0.7152f;
inline constexpr float kLumaB = 0.0722f;

/// GLSL smoothstep(): 0 below @p e0, 1 above @p e1, a cubic ramp between (e1 > e0).
[[nodiscard]] float smoothstep(float e0, float e1, float x);
/// BT.709 luma of @p c.
[[nodiscard]] float luma(const Rgb& c);

// ---- Keyers ----

/// The keyers' View parameter.
enum class KeyView {
    Final, ///< the keyed picture
    Matte, ///< the alpha as an opaque greyscale picture
    Status ///< opaque: white = solid, black = transparent, grey = partly transparent
};

/// Chroma key parameters of one frame (see chromaKeyMatte()).
struct ChromaKeyParams
{
    Rgb key; ///< the key colour (screen preset or the Key Color parameter)
    float tolerance = 0.3f; ///< 0..1: distance still fully transparent
    float softness = 0.2f; ///< 0..1: width of the ramp to fully opaque
    float clipBlack = 0.f; ///< 0..1: matte values below become 0
    float clipWhite = 1.f; ///< 0..1: matte values above become 1
    bool invert = false;
    float spillAmount = 0.5f; ///< 0..1
    bool spillMaximum = false; ///< limit the screen channel to max (true) / mean of the others
    int spillChannel = 1; ///< 1 = green, 2 = blue (the screen's dominant channel)
    float shrinkGrowPx = 0.f; ///< timeline px, negative = shrink (GPU only)
    float edgeBlurPx = 0.f; ///< timeline px (GPU only)
    KeyView view = KeyView::Final;
};
/// Resolves the chroma key parameters at @p localFrame (screen presets, percentages).
[[nodiscard]] ChromaKeyParams chromaKeyParams(const ParamSet& p, int64_t localFrame);
/// Key colour of the green / blue screen presets.
[[nodiscard]] Rgb screenPresetColor(bool blue);

/// Distance of @p c from the key colour in a luma-normalised chroma plane, ~0 on the screen
/// and ~1 for neutral colours. The plane is YCbCr's CbCr divided by (Y + 0.1), so a darker or
/// brighter part of the screen (shading, wrinkles) keeps about the same position. The
/// distance is split into "along the key's chroma direction" and "across it": anything at
/// least as saturated as the key in its direction counts as screen (distance 0), so a
/// brighter, purer green is keyed as well.
[[nodiscard]] float chromaDistance(const ChromaKeyParams& p, const Rgb& c);
/// Matte (1 = keep, 0 = transparent) of straight colour @p c before shrink / grow and edge
/// blur: smoothstep over tolerance..tolerance + softness, clip black / white, invert.
[[nodiscard]] float chromaKeyMatte(const ChromaKeyParams& p, const Rgb& c);
/// Spill suppression: the screen channel is limited to the mean (or maximum) of the other
/// two, by the spill amount.
[[nodiscard]] Rgb despill(const ChromaKeyParams& p, const Rgb& c);

/// Luma key parameters (0..1).
struct LumaKeyParams
{
    float low = 0.2f;
    float high = 1.f;
    float softness = 0.1f;
    bool invert = false;
    KeyView view = KeyView::Final;
};
[[nodiscard]] LumaKeyParams lumaKeyParams(const ParamSet& p, int64_t localFrame);
/// Matte: 1 inside [low, high] with soft ramps of width softness outside, else 0.
[[nodiscard]] float lumaKeyMatte(const LumaKeyParams& p, const Rgb& c);

/// HSL key parameters (hue in degrees, the rest 0..1).
struct HslKeyParams
{
    float hue = 120.f;
    float hueWidth = 60.f; ///< full width of the selected hue range
    float hueSoftness = 20.f;
    float satLow = 0.2f, satHigh = 1.f, satSoftness = 0.1f;
    float lumLow = 0.f, lumHigh = 1.f, lumSoftness = 0.1f;
    bool invert = false;
    KeyView view = KeyView::Final;
};
[[nodiscard]] HslKeyParams hslKeyParams(const ParamSet& p, int64_t localFrame);
/// HSL of @p c: hue in degrees 0..360, saturation and lightness 0..1.
void toHsl(const Rgb& c, float* h, float* s, float* l);
/// Matte: 0 (transparent) inside the selected hue / saturation / luminance range, 1 outside.
[[nodiscard]] float hslKeyMatte(const HslKeyParams& p, const Rgb& c);

/// Applies a keyer's view to straight colour @p c with original alpha @p alpha and matte
/// @p matte: returns premultiplied RGBA (Final: c × alpha × matte; Matte / Status: opaque).
void keyOutput(KeyView view, const Rgb& c, float alpha, float matte, float out[4]);

// ---- Colour ----

/// Exposure: linear-light gain of 2^stops (gamma 2.2 approximation), then a display gamma.
struct ExposureParams
{
    float gain = 1.f; ///< 2^exposure
    float invGamma = 1.f; ///< 1 / gamma
};
[[nodiscard]] ExposureParams exposureParams(const ParamSet& p, int64_t localFrame);
[[nodiscard]] Rgb exposure(const ExposureParams& p, const Rgb& c);

/// Temperature / tint as linear-light channel gains, normalised to keep the luma.
struct GainParams
{
    float r = 1.f, g = 1.f, b = 1.f;
};
[[nodiscard]] GainParams temperatureTintParams(const ParamSet& p, int64_t localFrame);
[[nodiscard]] Rgb linearGains(const GainParams& p, const Rgb& c);

/// Levels (0..1 values; gamma > 0).
struct LevelsParams
{
    float inBlack = 0.f, inWhite = 1.f, invGamma = 1.f, outBlack = 0.f, outWhite = 1.f;
};
[[nodiscard]] LevelsParams levelsParams(const ParamSet& p, int64_t localFrame);
[[nodiscard]] Rgb levels(const LevelsParams& p, const Rgb& c);

/// Invert: mix(c, 1 − c, amount).
[[nodiscard]] Rgb invert(float amount, const Rgb& c);

/// Black and white with an optional tint (colour normalised to luma 1).
struct BlackWhiteParams
{
    float amount = 1.f;
    Rgb tint {1.f, 1.f, 1.f}; ///< tint colour divided by its luma
    float tintAmount = 0.f;
};
[[nodiscard]] BlackWhiteParams blackWhiteParams(const ParamSet& p, int64_t localFrame);
[[nodiscard]] Rgb blackWhite(const BlackWhiteParams& p, const Rgb& c);

/// Posterize to @p levels steps per channel (>= 2).
[[nodiscard]] Rgb posterize(int levels, const Rgb& c);

/// Lift / gamma / gain per channel: v = gain · (c + lift · (1 − c)), out = v^(1 / gamma).
struct ColorBalanceParams
{
    float lift[3] = {0.f, 0.f, 0.f};
    float invGamma[3] = {1.f, 1.f, 1.f};
    float gain[3] = {1.f, 1.f, 1.f};
};
[[nodiscard]] ColorBalanceParams colorBalanceParams(const ParamSet& p, int64_t localFrame);
[[nodiscard]] Rgb colorBalance(const ColorBalanceParams& p, const Rgb& c);

// ---- Batch 2: colour (EffectMathGrade.cpp) ----

/// Inverse of toHsl(): hue in degrees (any value, wrapped), saturation and lightness 0..1.
[[nodiscard]] Rgb fromHsl(float h, float s, float l);

/// Curves: composite tables (master, then the channel's own curve) of kCurveTableSize
/// entries per channel, sampled with linear interpolation (like the GPU's linear filter).
inline constexpr int kCurveTableSize = 256;
struct CurvesParams
{
    float table[3][kCurveTableSize]; ///< red, green, blue
    float amount = 1.f;
    bool identity = true; ///< every curve is the diagonal
};
/// Builds the tables from the four Curve statics.
void curvesParams(const ParamSet& p, int64_t localFrame, CurvesParams* out);
/// Linear interpolation of @p table (kCurveTableSize entries) at @p x (0..1).
[[nodiscard]] float curveLookup(const float* table, float x);
[[nodiscard]] Rgb curves(const CurvesParams& p, const Rgb& c);

/// HSL adjust: hue shift (degrees), saturation and lightness (−1..1), weighted by a soft
/// hue range (all colours when `all`).
struct HslAdjustParams
{
    bool all = true;
    float centre = 0.f; ///< range centre hue, degrees
    float halfWidth = 30.f; ///< half range width, degrees
    float softness = 30.f; ///< degrees
    float hueShift = 0.f;
    float saturation = 0.f;
    float lightness = 0.f;
};
[[nodiscard]] HslAdjustParams hslAdjustParams(const ParamSet& p, int64_t localFrame);
/// Weight 0..1 of colour hue @p h with saturation @p s in the range of @p p.
[[nodiscard]] float hslRangeWeight(const HslAdjustParams& p, float h, float s);
[[nodiscard]] Rgb hslAdjust(const HslAdjustParams& p, const Rgb& c);

/// Vibrance (−1..1, muted colours more, skin tones half) and saturation (−1..1).
struct VibranceParams
{
    float vibrance = 0.3f;
    float saturation = 0.f;
};
[[nodiscard]] VibranceParams vibranceParams(const ParamSet& p, int64_t localFrame);
[[nodiscard]] Rgb vibrance(const VibranceParams& p, const Rgb& c);

/// Channel mixer: out = m · c (row-major, rows = output red, green, blue).
struct ChannelMixerParams
{
    float m[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
};
[[nodiscard]] ChannelMixerParams channelMixerParams(const ParamSet& p, int64_t localFrame);
[[nodiscard]] Rgb channelMixer(const ChannelMixerParams& p, const Rgb& c);

/// Duotone: luma mapped from the shadow to the highlight colour, mixed by amount.
struct DuotoneParams
{
    Rgb shadow;
    Rgb highlight {1.f, 1.f, 1.f};
    float amount = 1.f;
};
[[nodiscard]] DuotoneParams duotoneParams(const ParamSet& p, int64_t localFrame);
[[nodiscard]] Rgb duotone(const DuotoneParams& p, const Rgb& c);

/// Flicker: brightness gain of frame @p localFrame (a sine at the frequency blended with
/// smooth value noise by the randomness, deterministic per seed), around 1 ± amount.
[[nodiscard]] float flickerGain(const ParamSet& p, int64_t localFrame, double fps);
/// Smooth 1D value noise in −1..1 at @p t (lattice step 1), pattern @p seed.
[[nodiscard]] float valueNoise1d(double t, uint32_t seed);

/// Camera shake at @p seconds (clip time; the motion blur passes sub-frame times) with the
/// parameters of @p localFrame: offset in timeline px and rotation in radians, smooth
/// deterministic noise (two octaves of valueNoise1d()) per seed.
void cameraShake(const ParamSet& p, int64_t localFrame, double seconds, double* dx, double* dy,
    double* rotation);

/// Apply LUT: the LUT's colour mixed with the original by @p amount (0..1).
[[nodiscard]] Rgb applyLut(const CubeLut& lut, float amount, const Rgb& c);

// ---- Shared helpers ----

/// Static Choice value @p key of @p p (@p fallback if absent).
[[nodiscard]] QString choiceParam(const ParamSet& p, const QString& key, const QString& fallback);
/// Static Bool value.
[[nodiscard]] bool boolParam(const ParamSet& p, const QString& key, bool fallback);
/// Static Color value as straight Rgb.
[[nodiscard]] Rgb colorParam(const ParamSet& p, const QString& key, const Rgb& fallback);
/// View parameter of a keyer.
[[nodiscard]] KeyView keyView(const ParamSet& p);

/// Film grain value noise in −0.5..0.5 at layer pixel (@p x, @p y): bilinear interpolation of
/// a hashed lattice with @p cellPx spacing; @p seed selects the pattern (frame, channel).
/// Same integer hash as the shader.
[[nodiscard]] float grainNoise(float x, float y, float cellPx, uint32_t seed);

/// CPU fallback: applies effect @p type to @p image (ARGB32_Premultiplied) in place if it is a
/// pointwise effect this file implements (keyers without shrink / grow and edge blur, every
/// colour effect, Duotone, Flicker). @p fps is the timeline rate (animated effects).
/// @return false (image unchanged) for other effects (and Apply LUT without a readable file).
bool applyPointEffect(const QString& type, const ParamSet& params, int64_t localFrame,
    QImage& image, double fps = 25.0);
/// True if applyPointEffect() implements @p type.
[[nodiscard]] bool isPointEffect(const QString& type);

namespace detail {
/// The batch-2 part of applyPointEffect() (EffectMathGrade.cpp). @p image is
/// ARGB32_Premultiplied. @return false for other types.
bool applyGradeEffect(
    const QString& type, const ParamSet& params, int64_t localFrame, QImage& image, double fps);
/// True for the types applyGradeEffect() implements.
[[nodiscard]] bool isGradeEffect(const QString& type);
} // namespace detail

} // namespace kk::render::fxmath
