// SPDX-License-Identifier: GPL-3.0-or-later
// CompMath — the pure part of Compositing page node evaluation shared by the GPU
// (GlComposition) and the CPU fallback (CpuComposition): parameter resolution, node geometry
// (normalised, y-up node coordinates -> timeline pixels) and the pointwise colour formulas.
#pragma once

#include "core/Composition.h"
#include "core/Mask.h"
#include "render/Generators.h"
#include "render/TitleRenderer.h"

#include <QSizeF>
#include <QTransform>

#include <array>
#include <cstdint>

namespace kk::render::compmath {

/// Timeline pixel (y down) of normalised node coordinates (@p x, @p y): x 0..1 left to
/// right, y 0..1 bottom to top.
[[nodiscard]] inline QPointF toFrame(double x, double y, QSizeF frame)
{
    return {x * frame.width(), (1.0 - y) * frame.height()};
}

/// Transform node: maps a timeline pixel of the input to the output (centre, pivot, size,
/// aspect, angle counter-clockwise, flips about the frame centre).
[[nodiscard]] QTransform transformMatrix(const ParamSet& p, int64_t frame, QSizeF frameSize);

/// Tracker node: the move of the input at @p frame, relative to the reference frame: the
/// tracked point (CenterX / CenterY) at @p frame minus its place at RefFrame (Match Move), or
/// the opposite (Stabilize). Identity without a track.
[[nodiscard]] QTransform trackerMatrix(const ParamSet& p, int64_t frame, QSizeF frameSize);

/// The Planar Tracker's region at @p frame as four corners (top-left, top-right, bottom-right,
/// bottom-left) in timeline pixels (y down).
[[nodiscard]] std::array<QPointF, 4> planarCorners(
    const ParamSet& p, int64_t frame, QSizeF frameSize);

/// Planar Tracker node: maps a timeline pixel of the input to the output at @p frame.
/// Corner Pin: the input frame's corners go to the tracked corners (a perspective map).
/// Unsteady (Match Move): the move, turn and scale (a similarity, best fit to the four corners)
/// the plane made from the Reference Frame to @p frame; Steady (Stabilize): the opposite. The
/// last two are what other nodes can follow. Identity without a usable region.
[[nodiscard]] QTransform planarMatrix(const ParamSet& p, int64_t frame, QSizeF frameSize);

/// Merge node: maps a timeline pixel of the foreground to the output (the foreground's
/// centre goes to Center, scaled by Size and turned by Angle about its centre).
[[nodiscard]] QTransform mergeForegroundMatrix(const ParamSet& p, int64_t frame, QSizeF frameSize);

/// How Merge and Transform resample the picture: the edge mode (edgemodes:: index) and the
/// filter (filtermethods:: index, Nearest 0, Linear 1, ...). The default (Canvas, Linear) is
/// drawn by the plain textured quad / QPainter path; anything else by the per-pixel resampler
/// (CpuComposition::resample(), the GLSL kResampleGlsl), which gives the same pixels.
struct Resampling
{
    int edges = 0;
    int filter = 1;
    [[nodiscard]] bool isDefault() const { return edges == 0 && filter == 1; }
};
[[nodiscard]] Resampling resampling(const ParamSet& p);
/// Weight of the filter's kernel at distance @p d (pixels) from the sample position; the
/// resampler uses the 4x4 nearest taps and divides by the sum of the weights.
[[nodiscard]] float filterWeight(int filter, float d);
/// The pixel of a row / column @p i of @p n that edge mode @p edges reads (-1: transparent).
[[nodiscard]] int edgeIndex(int i, int n, int edges);
/// GLSL: kkResample(sampler2D, vec2 uv, int edges, int filter) on premultiplied texels,
/// the same as CpuComposition's resampler.
extern const char* const kResampleGlsl;

/// Resize node: the rectangle (timeline px) the input frame is drawn into.
[[nodiscard]] QRectF resizeRect(const ParamSet& p, int64_t frame, QSizeF frameSize);

/// Letterbox node: the picture area kept between the bars (timeline px); the full frame
/// when the aspect ratio matches.
[[nodiscard]] QRectF letterboxRect(const ParamSet& p, int64_t frame, QSizeF frameSize);

/// Crop node: the kept rectangle (timeline px).
[[nodiscard]] QRectF cropRect(const ParamSet& p, int64_t frame, QSizeF frameSize);

/// A mask node as a MaskData of the clip-mask code (core/Mask.h, render/GlMasks), so both
/// renderers reuse its maths: content = the frame (timeline px, origin at its centre).
/// Soft edge = feather in + out of soft · width / 2 each; Level = opacity.
[[nodiscard]] MaskData maskOf(const CompNode& node, int64_t frame, QSizeF frameSize);

/// Color Corrector parameters at one frame.
struct CorrectorParams
{
    float lift = 0, gamma = 1, gain = 1, saturation = 1;
    float inBlack = 0, inWhite = 1, levelGamma = 1, outBlack = 0, outWhite = 1;
    /// True if the node changes nothing.
    [[nodiscard]] bool isIdentity() const;
};
[[nodiscard]] CorrectorParams correctorParams(const ParamSet& p, int64_t frame);

/// The Color Corrector on a straight colour (0..1 per channel), in this order: levels
/// (input black / white, gamma, output black / white), lift / gamma / gain
/// (c = gain · (c + lift · (1 − c)), then c^(1/gamma)), saturation about BT.709 luma, clamp.
/// The GLSL pass (GlComposition) computes exactly the same.
void applyCorrector(const CorrectorParams& p, float rgb[3]);

/// Channel Booleans: for each output channel (R, G, B, A) the index of its source
/// (channelsources:: order: 0 R, 1 G, 2 B, 3 A, 4 luma, 5 black, 6 white).
[[nodiscard]] std::array<int, 4> channelSources(const ParamSet& p);
/// Applies @p src to a straight colour + alpha (in place, rgba 0..1).
void applyChannels(const std::array<int, 4>& src, float rgba[4]);

/// Merge blend function B(cb, cs) of mode index @p mode (blendmodes:: order) on straight
/// colour channels; the composite is co = cs·(1 − ab) + cb·(1 − as) + as·ab·B, ao = as + ab·(1 −
/// as) (premultiplied cs, cb), Normal = plain "over".
[[nodiscard]] float blendChannel(int mode, float cb, float cs);
/// The same blend functions in GLSL: float kkBlend(int mode, float b, float s) and
/// vec3 kkBlend3(int mode, vec3 b, vec3 s), for the Merge node (GlComposition) and the
/// timeline's composite modes (GlRenderer).
extern const char* const kBlendFunctionGlsl;
/// Blend of whole straight colours (all modes, including Hue, Saturation, Color, Luminosity,
/// Darker / Lighter Color which need the three channels together).
void blendColor(int mode, const float cb[3], const float cs[3], float out[3]);

/// The Merge node's compositing parameters at one frame.
struct MergeParams
{
    int mode = 0; ///< blendmodes::index()
    int operation = 0; ///< mergeops::index(), used by Normal only
    float opacity = 1, alphaGain = 1, subAdd = 0, burnIn = 0;
};
[[nodiscard]] MergeParams mergeParams(const ParamSet& p, int64_t frame);
/// The Merge node on premultiplied @p b (background) and @p s (foreground) into @p o, all
/// rgba 0..1. Blend scales the whole foreground, Alpha Gain only its alpha (the colour stays,
/// so a foreground below full gain adds light). The backdrop kept under the foreground is
/// 1 - alpha · ((1 - subAdd) + subAdd · burnIn): Subtractive (0) is the usual "over",
/// Additive (1) leaves the backdrop unchanged under the foreground and Burn In brings the
/// darkening back. The Normal mode applies the Operator (Over, In, Held Out, Atop, XOr, the
/// Porter-Duff operators), the other modes blend as blendPremultiplied(). The GLSL pass
/// (GlComposition) computes exactly the same.
void mergePixel(const MergeParams& m, const float b[4], const float s[4], float o[4]);

/// The Merge composite of premultiplied @p s over premultiplied @p b (rgba 0..1) with mode
/// @p mode into @p o (the formula above; mode 0 = plain "over").
void blendPremultiplied(int mode, const float b[4], const float s[4], float o[4]);

/// Background node as a generator pattern (render/Generators: solid = four equal corners,
/// gradient = linear along the angle across the frame, four corner = TL, TR, BL, BR), so the
/// GPU (GlGenerators) and CPU (renderGeneratorImage()) draw it alike.
[[nodiscard]] GeneratorFrame backgroundFrame(const ParamSet& p, int64_t frame, QSizeF frameSize);

/// Text node: the title style for a canvas of @p canvas px at @p scale px per timeline px
/// (empty text = draws nothing) and, in @p offset, how far (canvas px) the centred raster
/// moves to the node's Center.
[[nodiscard]] TitleStyle textStyle(const ParamSet& p, int64_t frame, QSize canvas, double scale,
    QSizeF frameSize, QPointF* offset);

/// Fast Noise parameters at one frame (the GLSL uniforms of GlComposition).
struct NoiseParams
{
    double centreX = 0, centreY = 0; ///< timeline px
    double cosA = 1, sinA = 0;
    double scale = 10; ///< cells across the frame width
    int detail = 2;
    double contrast = 1, brightness = 0;
    double time = 0; ///< seethe phase
    uint32_t seed = 0;
    std::array<float, 4> color1 {}, color2 {}; ///< premultiplied
};
[[nodiscard]] NoiseParams noiseParams(
    const ParamSet& p, int64_t frame, QSizeF frameSize, double fps);
/// The noise value 0..1 at timeline pixel (@p x, @p y) (identical to the GLSL pass).
[[nodiscard]] float noiseAt(const NoiseParams& n, double x, double y, QSizeF frameSize);

/// Premultiplied RGBA of a colour static @p key (@p fallback when unset).
[[nodiscard]] std::array<float, 4> colorOf(
    const ParamSet& p, const QString& key, const QColor& fallback);

} // namespace kk::render::compmath
