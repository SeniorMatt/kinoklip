// SPDX-License-Identifier: GPL-3.0-or-later
// CurveParam — the value of a Curve parameter (ParamType::Curve): tone-curve points, their
// text form stored in a ParamSet static, and the smooth interpolation the Curves effect, its
// CPU reference and the Inspector's curve widget share.
#pragma once

#include <QString>
#include <QVector>

namespace kk::curve {

/// One curve point: input @p x and output @p y, both 0..1.
struct Point
{
    double x = 0.0;
    double y = 0.0;
    friend bool operator==(const Point&, const Point&) = default;
};

/// Points sorted by x, at least two, with distinct x (see normalized()).
using Points = QVector<Point>;

/// Fewest / most points of a curve.
inline constexpr int kMinPoints = 2;
inline constexpr int kMaxPoints = 16;
/// Smallest x distance between neighbouring points.
inline constexpr double kMinGap = 0.01;

/// The identity curve (0,0) - (1,1).
[[nodiscard]] Points identity();
/// Text form of the identity curve: "0,0 1,1".
[[nodiscard]] QString identityText();

/// Sorts, clamps to 0..1, drops points closer than kMinGap to their left neighbour and caps
/// the count at kMaxPoints; fewer than two points give the identity.
[[nodiscard]] Points normalized(Points points);
/// Parses "x,y x,y ..." (C locale numbers). Malformed text gives the identity.
[[nodiscard]] Points parse(const QString& text);
/// Formats @p points as "x,y x,y ..." with at most 4 decimals (parse(format(p)) == p for
/// points on that grid).
[[nodiscard]] QString format(const Points& points);
/// True if @p points map every x to itself.
[[nodiscard]] bool isIdentity(const Points& points);

/// Value of the curve at @p x (clamped to the end points' x range: flat outside), by
/// monotone cubic Hermite interpolation (Fritsch - Carlson tangents): smooth, passes through
/// every point and never overshoots between two points. Result clamped to 0..1.
[[nodiscard]] double evaluate(const Points& points, double x);
/// Fills @p out[0..n) with evaluate() at x = i / (n − 1) (n >= 2): the lookup table the
/// renderer samples.
void sample(const Points& points, float* out, int n);

} // namespace kk::curve
