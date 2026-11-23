// SPDX-License-Identifier: GPL-3.0-or-later
// ColorMatch — the maths behind the Color page's Color Match palette (docs/color-page.md, spec
// 153): Shot Match (balance and contrast of one picture matched to another from image statistics)
// and the chart match (a photographed colour chart matched to its reference values). Both return
// ordinary Primaries and RGB Mixer parameters, so the CPU and GPU grade paths agree by
// construction. Pure functions, no Qt widgets, no project access.
#pragma once

#include "core/Grade.h"

#include <QPointF>
#include <QString>
#include <QVector>

#include <array>
#include <cstddef>
#include <utility>

namespace kk::colormatch {

// ---- Shot Match ----

/// Per-channel statistics of a picture (straight, gamma-encoded RGB 0..1): the 5th, 50th and
/// 95th percentile of each channel. Percentiles instead of mean and standard deviation (Reinhard
/// et al., "Color Transfer between Images", 2001): a few clipped or very bright pixels do not
/// drag the match, and the same numbers describe where the shadows, midtones and highlights are.
struct ShotStats
{
    float low[3] = {0, 0, 0}; ///< 5th percentile
    float mid[3] = {0, 0, 0}; ///< median
    float high[3] = {1, 1, 1}; ///< 95th percentile
    bool valid = false;
};

/// Statistics of @p pixels RGB triples (@p rgb, 0..1). Invalid below 16 pixels.
[[nodiscard]] ShotStats shotStats(const float* rgb, size_t pixels);

/// The match: per channel `y = gain * (x + offset)` (gradekeys gain and offset, applied in this
/// order by the Primaries maths), which moves the source's median onto the target's and scales
/// its 5..95 percentile spread to the target's.
struct ShotMatch
{
    float gain[3] = {1, 1, 1};
    float offset[3] = {0, 0, 0};
    bool valid = false;
};

inline constexpr float kShotGainMin = 0.5f;
inline constexpr float kShotGainMax = 2.0f;
inline constexpr float kShotOffsetLimit = 0.4f;

/// Shot Match of a picture with statistics @p source to the one with @p target. @p strength
/// 0..1 blends from no change to the full match. The gain is limited to 0.5 .. 2 and the offset
/// to +-0.4, so a very different shot is only matched as far as is sensible.
[[nodiscard]] ShotMatch shotMatch(
    const ShotStats& source, const ShotStats& target, double strength = 1.0);

/// Joins two matches that are applied one after the other (@p first, then @p second):
/// `g2 (g1 (x + o1) + o2) = g1 g2 (x + o1 + o2 / g1)`.
[[nodiscard]] ShotMatch composeShotMatch(const ShotMatch& first, const ShotMatch& second);

/// The gain / offset parameters of @p match as (key, value) pairs (gradekeys::GainR ...).
[[nodiscard]] QVector<std::pair<QString, double>> shotMatchParams(const ShotMatch& match);

/// Label of the corrector node a Shot Match writes (a later Shot Match of the same clip joins it).
inline const QString kShotMatchLabel = QStringLiteral("Shot Match");

/// Puts @p match into @p grade: an ungraded clip gets one node with it; otherwise it is composed
/// into the output node when that is a Shot Match node, else a new serial corrector labelled
/// kShotMatchLabel is added after the output (so a grade built by hand is never overwritten and
/// the result stays exact: gain and offset compose). Returns the node id, 0 when @p match is
/// invalid or the grade's output is not a corrector.
quint64 applyShotMatch(ClipGrade& grade, const ShotMatch& match);

// ---- Colour chart ----

enum class ChartType {
    ColorCheckerClassic, ///< X-Rite ColorChecker Classic, 24 patches, 6 x 4
    ColorCheckerPassport ///< ColorChecker Passport Video: its 24 patch panel (same patches)
};
inline constexpr int kChartPatches = 24;
inline constexpr int kChartColumns = 6;
inline constexpr int kChartRows = 4;
/// Index of the first of the six neutral patches (white .. black, the bottom row).
inline constexpr int kChartFirstNeutral = 18;

enum class Gamma {
    Linear,
    Gamma22,
    Gamma24,
    Gamma26,
    Srgb,
    Rec709
};
enum class ColorSpace {
    Rec709,
    DciP3, ///< P3 primaries (the chosen white point)
    Rec2020
};
enum class WhitePoint {
    D65,
    D60,
    D50,
    Dci
};

using PatchColors = std::array<std::array<float, 3>, kChartPatches>;

[[nodiscard]] QString chartName(ChartType type);
[[nodiscard]] QString gammaName(Gamma gamma);
[[nodiscard]] QString colorSpaceName(ColorSpace space);
[[nodiscard]] QString whitePointName(WhitePoint white);
/// Stable ids for project files ("classic24", "srgb", "rec709", "d65").
[[nodiscard]] QString chartId(ChartType type);
[[nodiscard]] ChartType chartFromId(const QString& id);
[[nodiscard]] QString gammaId(Gamma gamma);
[[nodiscard]] Gamma gammaFromId(const QString& id);
[[nodiscard]] QString colorSpaceId(ColorSpace space);
[[nodiscard]] ColorSpace colorSpaceFromId(const QString& id);
[[nodiscard]] QString whitePointId(WhitePoint white);
[[nodiscard]] WhitePoint whitePointFromId(const QString& id);

/// Encoded value of linear light @p linear under @p gamma, and the reverse.
[[nodiscard]] float gammaEncode(Gamma gamma, float linear);
[[nodiscard]] float gammaDecode(Gamma gamma, float encoded);

/// Name of patch @p index (0 .. 23, row by row from the top left; "Dark Skin", ...).
[[nodiscard]] QString chartPatchName(ChartType type, int index);

/// What the chart is matched to.
struct ChartTarget
{
    Gamma sourceGamma = Gamma::Rec709; ///< the clip's encoding (the starting point of the fit)
    Gamma targetGamma = Gamma::Rec709; ///< the encoding the reference values are written in
    ColorSpace space = ColorSpace::Rec709;
    WhitePoint white = WhitePoint::D65;
    friend bool operator==(const ChartTarget&, const ChartTarget&) = default;
};

/// The reference colours of the chart in the target's encoding (what the picture should show):
/// the published sRGB values of the ColorChecker Classic, to linear light, adapted (Bradford) to
/// the target white, into the target's primaries, then encoded with the target gamma.
[[nodiscard]] PatchColors chartTargetColors(ChartType type, const ChartTarget& target);

/// The chart's outline on the picture: corners in fractions of the picture (0..1, origin top
/// left), clockwise from the top left. Any convex quadrilateral (a tilted or rotated chart).
struct ChartQuad
{
    std::array<QPointF, 4> corner {
        QPointF(0.3, 0.3), QPointF(0.7, 0.3), QPointF(0.7, 0.7), QPointF(0.3, 0.7)};
    friend bool operator==(const ChartQuad&, const ChartQuad&) = default;
};

/// The default quad: a centred chart of the chart's 3:2 shape filling about 40 % of the
/// picture width, for a picture of @p aspect (width / height).
[[nodiscard]] ChartQuad defaultChartQuad(double aspect);

/// Point of the chart at chart coordinates (@p u, @p v) in 0..1 (0,0 top left), mapped through
/// the quad's perspective.
[[nodiscard]] QPointF chartPoint(const ChartQuad& quad, double u, double v);

/// Mean colour of the central part of each patch of the picture @p rgb (@p width x @p height
/// triples, 0..1): the measured chart. False when the quad is degenerate or leaves the picture.
bool measureChart(const float* rgb, int width, int height, const ChartQuad& quad, PatchColors& patches);

/// Result of solveChart(): lift / gamma / gain per channel (the Primaries wheels' R, G, B) and
/// the RGB Mixer matrix (row-major, out = M in), applied in that order (Primaries first).
struct ChartMatch
{
    float lift[3] = {0, 0, 0};
    float gamma[3] = {0, 0, 0};
    float gain[3] = {1, 1, 1};
    float mixer[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double errorBefore = 0; ///< RMS distance to the target over the 24 patches, encoded 0..1
    double errorAfter = 0;
    bool valid = false;
};

/// Matches the @p measured patches to the chart's target colours. First the six neutral patches
/// give each channel its lift, gamma and gain (the tone curve: Levenberg-Marquardt on the real
/// Primaries maths, started from the source / target gamma pair); then a 3 x 3 matrix whose rows
/// add up to 1 (so the neutral axis stays put) is fitted by least squares over all patches, with
/// a weak pull to the identity. Invalid when the patches are too flat to fit.
[[nodiscard]] ChartMatch solveChart(
    const PatchColors& measured, ChartType type, const ChartTarget& target);

/// Writes @p match into corrector @p node: the Primaries lift / gamma / gain of R, G and B and
/// the RGB Mixer matrix, with the offset, the master wheels and the mixer's Monochrome set back
/// to neutral (the match is the node's whole colour balance; contrast, saturation and the other
/// values stay as they are).
void applyChartToNode(GradeNode& node, const ChartMatch& match);

/// What the solved match does to colour @p rgb (the real grade maths: Primaries, then mixer).
void applyChartMatch(const ChartMatch& match, float rgb[3]);

/// The match as Primaries and RGB Mixer parameters (gradekeys::LiftR ..., MixerRR ...).
[[nodiscard]] QVector<std::pair<QString, double>> chartMatchParams(const ChartMatch& match);

} // namespace kk::colormatch
