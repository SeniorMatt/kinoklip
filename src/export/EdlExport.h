// SPDX-License-Identifier: GPL-3.0-or-later
// EdlExport — the timeline as a CMX3600 edit decision list, and its markers as an EDL or a
// CSV file (File > Export, spec 18.12 / 18.13).
#pragma once

#include "core/TimelineData.h"

#include <QString>

#include <cstdint>
#include <functional>

namespace kk {
class MediaPool;
}

namespace kk::edl {

/// Options of timelineToEdl().
struct EdlOptions
{
    /// Video track written as the "V" events (0 = V1). CMX3600 has a single video track.
    int videoTrack = 0;
    /// Audio tracks written as channel events: A1 -> "A", A2 -> "A2" (at most two in
    /// CMX3600; -1 leaves one out).
    int audioTrack1 = 0;
    int audioTrack2 = 1;
    /// Source time (µs) that shows as 00:00:00:00 in the source timecode of a clip, e.g. the
    /// media's start timecode (ui/common/SourceTimecode.h). Empty = 0 for every clip.
    std::function<int64_t(const ClipData&)> sourceStartUs;
};

/// CMX3600 EDL of @p data (title = the timeline name, non-drop frame, every timecode in the
/// timeline's rate; record timecodes count from the timeline's starting timecode):
///
/// * one event per clip of the chosen video and audio tracks, numbered in record order, with
///   the reel (the media file name without extension, cut to 8 characters, spaces as '_';
///   "AX" for titles, solids, generators and compound clips), the channel ("V", "A", "A2"),
///   the edit type, source in / out and record in / out, and "* FROM CLIP NAME:";
/// * a cross dissolve (any transition between two touching clips) is written the CMX way:
///   the outgoing clip's event ends where the transition starts, then a zero-length cut of
///   the outgoing clip and a "D <frames>" event of the incoming clip from the transition's
///   start (its source in moves back by the frames before the cut). Fades from / to nothing
///   are written as cuts;
/// * a clip at a constant speed other than 100 % gets an "M2" line (reel, speed in frames per
///   second with its sign, source in); source out is then source in + duration x speed.
///   Ramped (retime curve) clips are written at their average speed with a comment.
///
/// @p pool (may be null) supplies the reel names.
[[nodiscard]] QString timelineToEdl(
    const TimelineData& data, const MediaPool* pool, const EdlOptions& options = {});

/// The timeline markers of @p data as an EDL: one zero-source event per marker on "V"
/// (record in = the marker, record out = its end) followed by "* LOC: <timecode> <COLOR>
/// <name>" and, with a note, "* COMMENT: <note>". Clip markers are not included (Resolve's
/// "Timeline Markers" export).
[[nodiscard]] QString markersToEdl(const TimelineData& data);

/// The timeline markers of @p data as CSV (RFC 4180, header line, CRLF line ends):
/// "#", "Name", "Start", "End", "Duration", "Color", "Notes" (timecodes in the timeline rate,
/// End exclusive like an EDL record out; Color = the nearest named marker colour).
[[nodiscard]] QString markersToCsv(const TimelineData& data);

/// "HH:MM:SS:FF" of @p frames at @p fps (non-drop frame; whole-frame rates rounded).
[[nodiscard]] QString timecode(int64_t frames, Rational fps);

/// Name of the marker colour closest to @p color among the marker palette
/// ("Blue", "Cyan", ... as in the marker editor), upper case for EDL locators when @p upper.
[[nodiscard]] QString markerColorName(const QColor& color, bool upper = false);

/// Writes @p text to @p path (UTF-8, atomically). False with @p error set on failure.
bool writeTextFile(const QString& path, const QString& text, QString* error = nullptr);

} // namespace kk::edl
