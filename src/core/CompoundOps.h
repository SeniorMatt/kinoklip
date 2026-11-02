// SPDX-License-Identifier: GPL-3.0-or-later
// CompoundOps — pure functions for compound clips and nested timelines: collapse a selection
// into a nested timeline, decompose an instance in place, recursion checks, derived data.
#pragma once

#include "core/TimelineData.h"
#include "media/MediaInfo.h"

#include <QSet>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

namespace kk::compound {

/// Looks a timeline up by TimelineData::uid; nullptr if it does not exist.
using TimelineLookup = std::function<const TimelineData*(quint64 uid)>;

/// Uids of the timelines @p data shows through Nested clips (direct references only).
[[nodiscard]] QSet<quint64> directReferences(const TimelineData& data);

/// True if timeline @p from is @p target or contains it, directly or through any depth of
/// nesting. Cycles in damaged data are tolerated (each timeline is visited once).
[[nodiscard]] bool reaches(quint64 from, quint64 target, const TimelineLookup& lookup);

/// True if a clip showing timeline @p inner may be placed into timeline @p outer: @p inner
/// exists (non-zero) and does not reach @p outer (a timeline can never contain itself).
[[nodiscard]] bool canNest(quint64 inner, quint64 outer, const TimelineLookup& lookup);

/// Every timeline reachable from @p root through Nested clips (root excluded), each once.
[[nodiscard]] QVector<quint64> reachableTimelines(quint64 root, const TimelineLookup& lookup);

/// Result of collapse().
struct CollapseResult
{
    TimelineData nested; ///< the new compound timeline (uid, name, tracks, clips)
    QVector<quint64> instanceIds; ///< the new Nested clip(s) in the parent (video, audio)
};

/// Moves the clips @p clipIds out of @p parent into a new compound timeline (uid
/// @p nestedUid, named @p name) and puts one Nested clip in their place: on the lowest video
/// track of the selection (and one on the lowest audio track when audio clips were selected,
/// linked to the video one), starting at the selection's first frame and as long as the
/// selection. Inside, the clips keep their relative timing and track order (lowest selected
/// track = V1 / A1) and everything they carry (effects, transitions, keyframes, markers,
/// links); the compound timeline has at least one video and one audio track. When the
/// selection's range on the lowest track is still taken by an unselected clip, the instance
/// goes on the next free unlocked track above, else on a new track at the top.
/// Fails (parent unchanged, @p error set) for an empty selection or locked tracks.
[[nodiscard]] std::optional<CollapseResult> collapse(TimelineData& parent,
    const QVector<quint64>& clipIds, quint64 nestedUid, const QString& name, QString* error);

/// Replaces the Nested clips @p clipIds of @p parent (all showing the timeline @p nested)
/// by copies of the nested clips they show: the visible window of each instance only (clips
/// cut at its edges), at the same timeline frames, nested track k going to the instance's
/// track + k or the next free unlocked track above (new tracks are added at the top when
/// needed). Effects, keyframes, transitions and links inside are kept; the instance's own
/// transform and effects are dropped (as in Resolve). Fails (parent unchanged) when an
/// instance is not a Nested clip of @p nested, has a speed other than 100 % or the frame
/// rates differ. Returns the ids of the restored clips.
[[nodiscard]] std::optional<QVector<quint64>> decompose(TimelineData& parent,
    const QVector<quint64>& clipIds, const TimelineData& nested, QString* error);

/// Updates ClipData::sourceDurationUs of every Nested clip of @p data to the current length
/// of the timeline it shows (trims are clamped to it like media). Returns true if anything
/// changed.
bool syncNestedSources(TimelineData& data, const TimelineLookup& lookup);

/// True if syncNestedSources() would change @p data (cheap check without copying).
[[nodiscard]] bool nestedSourcesOutdated(const TimelineData& data, const TimelineLookup& lookup);

/// Length in µs a Nested clip of @p nested can show (at least one frame).
[[nodiscard]] int64_t nestedDurationUs(const TimelineData& nested);

/// The media-pool description of a timeline (MediaItem::info of its item): valid, size,
/// frame rate, duration; video/audio = has clips of that type (both when empty).
[[nodiscard]] media::MediaInfo timelineInfo(const TimelineData& data);

/// A Nested clip showing all of @p nested from its start (id 0, start 0). Used for drops of
/// a timeline from the media pool and by collapse().
[[nodiscard]] ClipData makeNestedClip(const TimelineData& nested, const QString& mediaId);

} // namespace kk::compound
