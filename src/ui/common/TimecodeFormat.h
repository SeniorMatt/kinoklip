// SPDX-License-Identifier: GPL-3.0-or-later
// Frame number <-> "HH:MM:SS:FF" timecode text (non-drop-frame).
#pragma once

#include "core/Rational.h"

#include <QString>
#include <QStringView>

#include <cstdint>
#include <optional>

namespace kk {
struct TimelineData;
}

namespace kk::timecode {

/// Whole frames per timecode second for @p fps (rounded: 29.97 → 30, 23.976 → 24).
[[nodiscard]] int framesPerSecond(Rational fps);

/// Formats timeline frame @p frames as "HH:MM:SS:FF" (negative values get a leading '-').
/// Non-drop-frame: for fractional rates the timecode drifts slowly from wall-clock time.
[[nodiscard]] QString format(int64_t frames, Rational fps);

/// Parses "HH:MM:SS:FF", "MM:SS:FF", "SS:FF" or a plain frame count ("125"). ':', ';' and '.'
/// are accepted as separators; a leading '-' negates. Fields may exceed their usual range
/// ("00:00:90:00" = 90 s). Returns std::nullopt for malformed text.
[[nodiscard]] std::optional<int64_t> parse(QStringView text, Rational fps);

/// Record timecode of frame @p frame of timeline @p data: its starting timecode added
/// (TimelineData::startTimecode; spec 25.12). Durations and offsets use format().
[[nodiscard]] QString formatRecord(int64_t frame, const TimelineData& data);

/// Timeline frame of a record timecode @p frames (what parse() returned) typed for @p data:
/// minus the starting timecode. A value below the start is taken as an offset from it, so
/// "00:00:10:00" in a timeline starting at 01:00:00:00 means 01:00:10:00.
[[nodiscard]] int64_t recordToFrame(int64_t frames, const TimelineData& data);

} // namespace kk::timecode
