// SPDX-License-Identifier: GPL-3.0-or-later
// CutEdits — the Cut page's smart edits (smart insert, append, ripple overwrite, close up, place
// on top, source overwrite) and fast review speed, as pure functions on TimelineData.
#pragma once

#include "core/EditOps.h"
#include "core/TimelineData.h"

#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>

namespace kk::cutedits {

/// The material of one Cut page edit: one group per source segment (a clip, or one clip of the
/// source tape), each holding that segment's video and / or audio clip (linked after placing).
/// PlacedClip::clip.start is the offset of the segment from the first one (the first starts at
/// 0, the next where the previous ends); PlacedClip::track is the destination track the edit
/// starts from (the controller's targets).
using SourceGroups = QVector<QVector<editops::PlacedClip>>;

/// Outcome of a smart edit.
struct Result
{
    QVector<quint64> ids; ///< new clip ids, in group order (empty: nothing was edited)
    int64_t start = 0; ///< first timeline frame of the placed material
    int64_t end = 0; ///< frame after it (the playhead goes here, as in Resolve)
    [[nodiscard]] bool ok() const { return !ids.isEmpty(); }
};

/// Zoom factor of a Close Up edit (Resolve detects faces; Kinoklip zooms on the centre).
inline constexpr double kCloseUpZoom = 1.5;

/// Length of the material in @p groups (end of the last clip, from 0).
[[nodiscard]] int64_t span(const SourceGroups& groups);

/// The edit point nearest to @p frame: clip starts and ends on every track plus frame 0 and the
/// timeline end. Ties go to the earlier point. 0 for an empty timeline.
[[nodiscard]] int64_t nearestEditPoint(const TimelineData& data, int64_t frame);

/// Smart Insert: inserts the material at the edit point nearest to @p playhead (never splits a
/// clip); later clips on every unlocked sync-locked track ripple right.
Result smartInsert(TimelineData& data, const SourceGroups& groups, int64_t playhead);
/// Append: puts the material after the last clip of the timeline (nothing moves).
Result append(TimelineData& data, const SourceGroups& groups);
/// Ripple Overwrite: replaces the clip under @p playhead on the destination video track (else
/// the top-most clip under it on any auto-select track) with the material at its own length;
/// everything after it ripples by the length difference. Nothing happens without such a clip.
Result rippleOverwrite(TimelineData& data, const SourceGroups& groups, int64_t playhead);
/// Place On Top: the material starts at @p playhead on the first track above the destination
/// whose range is free (a new track when none is); nothing is overwritten or rippled. All
/// segments of one edit go onto the same track.
Result placeOnTop(TimelineData& data, const SourceGroups& groups, int64_t playhead);
/// Close Up: the video part of the material, zoomed by @p zoom around the picture centre,
/// placed on top at @p playhead (cutaway to a closer shot; the sound below stays).
Result closeUp(
    TimelineData& data, const SourceGroups& groups, int64_t playhead, double zoom = kCloseUpZoom);
/// Source Overwrite: the video part of the material placed on top so that it starts at
/// timeline frame @p start (from sourceOverwriteStart()); a part before frame 0 is cut off.
Result sourceOverwrite(TimelineData& data, const SourceGroups& groups, int64_t start);

/// Start timecode of a media item in µs (nullopt: the item has no usable timecode).
using TimecodeLookup = std::function<std::optional<int64_t>(const QString& mediaId)>;

/// Where a Source Overwrite places the source so it lines up by timecode with the timeline:
/// the timecode of the frame under @p playhead (clip under it on the lowest video track that
/// has one, else on any track) is looked up in the source @p sourceMediaId; the source frame
/// with that timecode lands on the playhead. @p sourceInUs is the source in point (where the
/// material starts) and @p sourceOutUs its end. Returns the timeline frame of the in point, or
/// nullopt when there is no clip under the playhead, a timecode is missing or the source range
/// does not contain that timecode (Resolve refuses such an edit too).
[[nodiscard]] std::optional<int64_t> sourceOverwriteStart(const TimelineData& data,
    int64_t playhead, const QString& sourceMediaId, int64_t sourceInUs, int64_t sourceOutUs,
    const TimecodeLookup& startTimecodeUs);

/// Fast Review speed for a clip of @p clipFrames at @p fps: long clips play faster so each
/// takes about kFastReviewSecondsPerClip, never slower than real time nor faster than
/// kFastReviewMaxRate.
[[nodiscard]] double fastReviewRate(int64_t clipFrames, Rational fps);
inline constexpr double kFastReviewSecondsPerClip = 2.0;
inline constexpr double kFastReviewMaxRate = 8.0;

} // namespace kk::cutedits
