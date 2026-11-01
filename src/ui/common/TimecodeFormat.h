// SPDX-License-Identifier: GPL-3.0-or-later
// Frame number <-> "HH:MM:SS:FF" timecode text (non-drop-frame).
#pragma once

#include "core/Rational.h"

#include <QString>
#include <QStringView>

#include <cstdint>
#include <optional>

namespace cw::timecode {

/// Whole frames per timecode second for @p fps (rounded: 29.97 → 30, 23.976 → 24).
[[nodiscard]] int framesPerSecond(Rational fps);

/// Formats timeline frame @p frames as "HH:MM:SS:FF" (negative values get a leading '-').
/// Non-drop-frame: for fractional rates the timecode drifts slowly from wall-clock time.
[[nodiscard]] QString format(int64_t frames, Rational fps);

/// Parses "HH:MM:SS:FF", "MM:SS:FF", "SS:FF" or a plain frame count ("125"). ':', ';' and '.'
/// are accepted as separators; a leading '-' negates. Fields may exceed their usual range
/// ("00:00:90:00" = 90 s). Returns std::nullopt for malformed text.
[[nodiscard]] std::optional<int64_t> parse(QStringView text, Rational fps);

} // namespace cw::timecode
