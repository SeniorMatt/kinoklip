// SPDX-License-Identifier: GPL-3.0-or-later
// GradeMath — the colour maths of the Color page as plain CPU functions: primaries (offset,
// temperature / tint, lift / gamma / gain with lum mix, contrast / pivot, hue / saturation),
// curve splines baked into tables, HSL conversion, the qualifier key and the power window
// matte. The GLSL of render/GlGrade.cpp mirrors these functions line by line and
// render/CpuGrade.cpp runs them per pixel; test_graderender compares both.
#pragma once

#include "core/Grade.h"

#include <array>
#include <cstdint>
#include <vector>

namespace kk::grademath {

/// Rec. 709 luma weights (the pictures are display-referred Rec. 709 / sRGB-like RGB).
inline constexpr float kLumaR = 0.2126f;
inline constexpr float kLumaG = 0.7152f;
inline constexpr float kLumaB = 0.0722f;
/// Gamma wheel range: exponent = 2^(-kGammaRange * gamma), so gamma +0.5 halves the exponent.
inline constexpr float kGammaRange = 2.0f;
/// Temperature / tint strength: +1 multiplies red (temperature) / divides green (tint) by
/// 1 + this and the opposite channel by 1 - this.
inline constexpr float kTemperatureRange = 0.25f;
inline constexpr float kTintRange = 0.25f;
/// Samples per baked curve table (inclusive end points; linear interpolation between them).
inline constexpr int kCurveTableSize = 256;
/// Smallest qualifier softness (an edge of zero width would be undefined for smoothstep).
inline constexpr float kMinSoftness = 1e-4f;

/// Rec. 709 luma of straight RGB @p c.
[[nodiscard]] inline float luma(const float c[3])
{
    return kLumaR * c[0] + kLumaG * c[1] + kLumaB * c[2];
}

/// Primaries of a node evaluated at one frame (channel index 0 R, 1 G, 2 B, 3 master).
struct PrimaryValues
{
    float lift[4] = {0, 0, 0, 0};
    float gamma[4] = {0, 0, 0, 0};
    float gain[4] = {1, 1, 1, 1};
    float offset[4] = {0, 0, 0, 0};
    float contrast = 1.0f;
    float pivot = 0.435f;
    float saturation = 1.0f;
    float hueDegrees = 0.0f;
    float temperature = 0.0f;
    float tint = 0.0f;
    float lumMix = 1.0f;
    /// Hue rotation about the grey axis as a row-major 3x3 matrix (from hueDegrees; identity
    /// for 0). The GPU receives exactly these floats.
    float hueMatrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    /// The values of @p node at frame @p localFrame (hueMatrix filled in).
    [[nodiscard]] static PrimaryValues at(const GradeNode& node, int64_t localFrame);
    /// True if applyPrimaries() changes nothing.
    [[nodiscard]] bool isIdentity() const;
};

/// Applies the primaries to straight (un-premultiplied) RGB @p rgb in place. Order: offset,
/// temperature / tint, per-channel lift / gamma / gain, master lift / gamma / gain (mixed
/// between per-channel and luma-only by lumMix), contrast around the pivot, hue rotation about
/// the grey axis, saturation around Rec. 709 luma. No clamping.
void applyPrimaries(const PrimaryValues& p, float rgb[3]);

/// One lift / gamma / gain transfer: pow(max(gain * (x + lift * (1 - x)), 0), 2^(-2 gamma)).
[[nodiscard]] float liftGammaGain(float x, float lift, float gamma, float gain);

/// The curve of @p points (kind @p curve) at @p x: monotone cubic (Fritsch-Carlson, no
/// overshoot) through the points. Channel and lum / sat curves get neutral end points at
/// x = 0 and 1 when the points do not reach them; hue curves are periodic (x wraps). An empty
/// list returns the neutral value.
[[nodiscard]] double evaluateCurve(const CurvePoints& points, GradeCurve curve, double x);

/// Inserts a point at (@p x, @p y) into @p points (kept sorted; a point at the same x is
/// replaced). An empty hue / lum / sat curve also gets two neutral neighbours (1/12 away), so
/// dragging the new point bends the curve locally. Returns its index.
int addCurvePoint(CurvePoints& points, GradeCurve curve, double x, double y);

/// Baked curve tables of a node: sample i is the curve at x = i / (kCurveTableSize - 1).
/// Y/R/G/B tables hold the output value, hue-vs-hue the hue shift in turns (y - 0.5), the
/// saturation and luminance curves a factor (2 y).
struct CurveTables
{
    std::array<std::array<float, kCurveTableSize>, kGradeCurveCount> table {};
    std::array<bool, kGradeCurveCount> identity {};
    /// True if every curve is identity (the curve stage is skipped).
    bool allIdentity = true;

    /// Bakes @p curves.
    [[nodiscard]] static CurveTables bake(const GradeCurves& curves);
    /// Bit i set when curve i is not identity (the GPU's uCurveMask).
    [[nodiscard]] int mask() const;
    /// Table @p c at @p x (0..1, clamped), linearly interpolated between samples (the GPU
    /// fetches the two samples and interpolates the same way).
    [[nodiscard]] float sample(GradeCurve c, float x) const;
};

/// Applies the curves to RGB @p rgb in place: values clamped to 0..1, the Y curve on every
/// channel, the R / G / B curves, then the HSL curves (hue vs hue / sat / lum, lum vs sat, sat
/// vs sat; hue, saturation and luma taken from the colour entering this stage).
void applyCurves(const CurveTables& tables, float rgb[3]);

/// RGB (0..1) to HSL: hue in turns [0, 1), saturation and lightness 0..1.
void rgbToHsl(const float rgb[3], float hsl[3]);
/// HSL to RGB, inverse of rgbToHsl().
void hslToRgb(const float hsl[3], float rgb[3]);

/// GLSL-style smoothstep (edge0 < edge1 required).
[[nodiscard]] float smoothStep(float edge0, float edge1, float x);

/// Qualifier key (0..1) of straight RGB @p rgb (clamped to 0..1 first). 1 when the qualifier
/// is disabled; invert applies.
[[nodiscard]] float qualifierKey(const GradeQualifier& q, const float rgb[3]);

/// A window evaluated at one frame in picture pixels (the pixel grid the grade is rendered
/// on): everything the per-pixel coverage needs.
struct WindowValues
{
    int shape = 0; ///< GradeWindowShape
    bool invert = false;
    float opacity = 1.0f;
    float cx = 0, cy = 0; ///< centre, pixels
    float cosR = 1, sinR = 0; ///< rotation (clockwise in y-down pixels)
    float halfW = 0, halfH = 0; ///< half size, pixels
    float softIn = 0.5f, softOut = 0.5f; ///< pixels, at least half a pixel each
    std::vector<float> points; ///< Polygon: x, y pairs in window-local pixels (unrotated)
};

/// Where the graded picture lies in the pixel grid: picture pixel (x, y) (centre at +0.5)
/// maps to picture fractions u = (x + 0.5 - originX) / width, v = (y + 0.5 - originY) / height.
struct PictureGeometry
{
    double originX = 0, originY = 0; ///< top-left corner of the picture, pixels
    double width = 1, height = 1; ///< size of the picture, pixels
};

/// The renderable windows of @p node at @p localFrame (at most kMaxGradeWindows, polygons at
/// most kMaxWindowPoints points) in the pixels of @p geometry.
[[nodiscard]] std::vector<WindowValues> evaluateWindows(
    const GradeNode& node, int64_t localFrame, const PictureGeometry& geometry);

/// Coverage (0..1, before invert and opacity) of window @p w at pixel centre (@p px, @p py):
/// a signed distance (exact for the rectangle and polygon, first-order for the ellipse, the
/// distance to the line for the gradient) mapped through 1 - smoothstep(-softIn, softOut, d).
[[nodiscard]] float windowCoverage(const WindowValues& w, float px, float py);

/// Union matte of @p windows at pixel centre (@p px, @p py): 1 without windows.
[[nodiscard]] float windowsMatte(const std::vector<WindowValues>& windows, float px, float py);

/// Layer mixer step: lays node output @p o (fed from @p in, key @p alpha) over @p result:
/// result = (1 - alpha) result + alpha in + (o - in), per channel, in place. Equals
/// mix(result, graded, alpha) when o = mix(in, graded, alpha), with no division by alpha.
void layerOver(float result[3], const float in[3], const float o[3], float alpha);

} // namespace kk::grademath
