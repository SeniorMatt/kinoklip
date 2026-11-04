// SPDX-License-Identifier: GPL-3.0-or-later
// QualifierPick — the qualifier eyedropper's maths: HSL ranges from picked colours (pick, add
// to the range, remove from it). Pure functions on core/Grade.h values (tests call them).
#pragma once

#include "core/Grade.h"

#include <QVector>

#include <array>

namespace kk::qualifierpick {

/// One picked colour as HSL (grademath::rgbToHsl(): hue in turns, saturation, lightness).
using Hsl = std::array<float, 3>;

/// The pick tool's modes (the Qualifier palette's three eyedroppers).
enum class Mode {
    Pick, ///< replace the range with the picked colours
    Add, ///< widen the range to include them
    Remove ///< narrow the range to leave them out
};

/// Margin added around the picked values (a single click still selects a small range).
inline constexpr double kHueMargin = 0.025;
inline constexpr double kRangeMargin = 0.08;

/// HSL of straight RGB @p rgb (0..1; clamped), through grademath::rgbToHsl().
[[nodiscard]] Hsl toHsl(const float rgb[3]);

/// The qualifier after picking @p samples in @p mode from @p current (enabled afterwards;
/// the per-channel enables, invert and softness are kept). No samples: @p current.
[[nodiscard]] GradeQualifier apply(
    const GradeQualifier& current, const QVector<Hsl>& samples, Mode mode);

/// The smallest hue arc (centre, full width; turns) containing every hue of @p hues.
[[nodiscard]] std::pair<double, double> hueArc(QVector<double> hues);

} // namespace kk::qualifierpick
