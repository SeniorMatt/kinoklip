// SPDX-License-Identifier: GPL-3.0-or-later
// EditOps — pure editing functions on TimelineData (insert, overwrite, split, move, trim, ...).
//
// Every function keeps the track invariants (clips sorted by start, never overlapping, duration
// >= 1, ids unique) and never touches locked tracks. They do not emit signals or create undo
// entries; wrap calls in Project::beginEdit()/endEdit() (see core/readme.md).
#pragma once

#include "core/TimelineData.h"

#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace kk::editops {

// ---- Lookup ----

/// Where a clip lives inside a TimelineData.
struct ClipLocation
{
    TrackRef track;
    int clipIndex = -1; ///< index into TrackData::clips
};

/// Finds the clip with id @p clipId. Returns std::nullopt if it does not exist.
[[nodiscard]] std::optional<ClipLocation> findClip(const TimelineData& data, quint64 clipId);
/// Pointer to the clip with id @p clipId, or nullptr. Invalidated by any structural edit.
[[nodiscard]] ClipData* clipById(TimelineData& data, quint64 clipId);
/// Pointer to the clip with id @p clipId, or nullptr (const overload).
[[nodiscard]] const ClipData* clipById(const TimelineData& data, quint64 clipId);

/// Predicate selecting the tracks an operation applies to.
using TrackFilter = std::function<bool(TrackRef ref, const TrackData& track)>;
/// Filter accepting every track that is not locked (the default for splitAt()).
[[nodiscard]] TrackFilter unlockedTracks();

// ---- Placing clips ----

/// A clip together with its destination track, for multi-track insert/overwrite.
struct PlacedClip
{
    TrackRef track;
    ClipData clip; ///< clip.start = destination frame; clip.id = 0 lets EditOps allocate one
};

/// Which tracks an insert ripples (shifts to the right).
enum class RippleScope {
    TargetTracks, ///< only the tracks receiving clips
    AllTracks ///< every unlocked track, keeping V/A sync (default)
};

/// Removes everything inside [@p startFrame, @p endFrame) on one track, splitting or trimming
/// clips that straddle the range edges (overwrite semantics). No-op on a locked track.
void clearRange(TimelineData& data, TrackRef ref, int64_t startFrame, int64_t endFrame);

/// Overwrite edit: clears the clip's range on @p ref and places @p clip there.
/// Returns the clip's id, or 0 if the track is missing/locked or clip.duration < 1.
quint64 overwriteClip(TimelineData& data, TrackRef ref, ClipData clip);
/// Overwrites several clips at once (e.g. linked video + audio). Returns ids in input order;
/// empty on failure (nothing is changed if any destination is invalid).
QVector<quint64> overwriteClips(TimelineData& data, const QVector<PlacedClip>& clips);

/// Insert (ripple) edit: opens a gap at clip.start by splitting the clip under that frame and
/// shifting later clips right by clip.duration, then places @p clip in the gap.
/// Returns the clip's id, or 0 on failure (missing/locked track, duration < 1).
quint64 insertClip(
    TimelineData& data, TrackRef ref, ClipData clip, RippleScope scope = RippleScope::AllTracks);
/// Inserts several clips at the earliest of their start frames; the gap is as long as the
/// longest span. Returns ids in input order; empty on failure (nothing changed).
QVector<quint64> insertClips(TimelineData& data, const QVector<PlacedClip>& clips,
    RippleScope scope = RippleScope::AllTracks);

// ---- Removing ----

/// Lift: removes the clips and leaves gaps. Clips on locked tracks are kept.
/// Returns the number of clips removed.
int liftClips(TimelineData& data, const QVector<quint64>& clipIds);
/// Ripple delete: removes the clips and shifts later clips on the same track left to close the
/// gaps (other tracks are not touched; include linked clips to keep sync).
/// Returns the number of clips removed.
int rippleDeleteClips(TimelineData& data, const QVector<quint64>& clipIds);

// ---- Splitting ----

/// Splits the given clips at timeline frame @p frame (clips not containing the frame strictly
/// inside are ignored). The left part keeps the id; right parts of clips that were linked
/// together are linked together again. Returns the ids of the new right-hand clips.
QVector<quint64> splitClips(TimelineData& data, const QVector<quint64>& clipIds, int64_t frame);
/// Splits every clip under @p frame on the tracks accepted by @p filter (default: all unlocked
/// tracks). Returns the ids of the new right-hand clips.
QVector<quint64> splitAt(TimelineData& data, int64_t frame, const TrackFilter& filter = {});

// ---- Moving ----

/// Moves clips by @p deltaFrames horizontally and @p deltaTrack tracks vertically (within their
/// own track type; +1 = towards V2/A2). Deltas are clamped so no clip goes before frame 0 or
/// off the track list. Anything under the moved clips is overwritten.
/// Returns false (and changes nothing) if a source or destination track is locked or nothing
/// would move.
bool moveClips(
    TimelineData& data, const QVector<quint64>& clipIds, int64_t deltaFrames, int deltaTrack = 0);

// ---- Trimming ----
// Sign convention: delta is how far the edit point moves on the timeline, +1 = one frame right.
// trimHead(+n) shortens the clip, trimTail(+n) lengthens it. Ripple trims keep the clip's
// start fixed and shift the later clips on the same track instead of leaving/consuming a gap.
// Limits: duration stays >= 1; a clip cannot reveal media outside [0, sourceDurationUs]
// (unbounded when sourceDurationUs == 0); non-ripple trims cannot overlap a neighbour.

/// Moves the head (in point) of clip @p clipId by @p delta frames. Returns the applied delta.
int64_t trimHead(TimelineData& data, quint64 clipId, int64_t delta, bool ripple);
/// Moves the tail (out point) of clip @p clipId by @p delta frames. Returns the applied delta.
int64_t trimTail(TimelineData& data, quint64 clipId, int64_t delta, bool ripple);
/// Slip: changes which part of the source is shown without moving the clip. Positive
/// @p deltaFrames move the in point later in the source. Clamped to the available media.
/// Returns the applied delta.
int64_t slipClip(TimelineData& data, quint64 clipId, int64_t deltaFrames);

/// Largest number of frames the head can be extended to the left by source media alone.
[[nodiscard]] int64_t maxHeadExtension(const ClipData& clip, Rational fps);
/// Largest number of frames the tail can be extended to the right by source media alone.
[[nodiscard]] int64_t maxTailExtension(const ClipData& clip, Rational fps);

// ---- Roll, slide, gaps (timeline agent additions) ----

/// Roll edit: moves the cut between two adjacent clips on one track (left.end() == right.start)
/// by @p delta frames; the left tail and the right head move together, so nothing else shifts.
/// Clamped so both clips keep duration >= 1 and stay within their source media.
/// Returns the applied delta (0 if the clips are not adjacent on one unlocked track).
int64_t rollEdit(TimelineData& data, quint64 leftClipId, quint64 rightClipId, int64_t delta);

/// Slide: moves clip @p clipId by @p delta frames while its neighbours absorb the change, so
/// the clip's content and the track length stay the same. A touching neighbour is trimmed (its
/// tail on the left, its head on the right); a gap on that side grows or shrinks instead.
/// Clamped by neighbour durations (>= 1), their media handles and frame 0.
/// Returns the applied delta.
int64_t slideClip(TimelineData& data, quint64 clipId, int64_t delta);

/// Empty range [first, second) on track @p ref around frame @p frame, bounded by the previous
/// clip end (or 0) and the next clip start. std::nullopt if a clip covers @p frame or there is
/// no clip after the frame (the open end of a track is not a gap).
[[nodiscard]] std::optional<std::pair<int64_t, int64_t>> gapAt(
    const TimelineData& data, TrackRef ref, int64_t frame);

/// Close gap: removes the gap under @p frame on track @p ref by shifting every later clip left.
/// With RippleScope::AllTracks every other unlocked track is shifted by the same amount, which
/// is reduced to the room that is free on all of them in the gap range (keeps sync).
/// Returns the number of frames removed (0 if there is no gap or the track is locked).
int64_t closeGap(
    TimelineData& data, TrackRef ref, int64_t frame, RippleScope scope = RippleScope::TargetTracks);

/// Place on top: for each clip, starts at its PlacedClip::track and walks up its track list to
/// the first unlocked track whose range [start, end) is empty; adds a new track on top if none
/// is. Nothing is overwritten or rippled. Returns ids in input order (empty on invalid input).
QVector<quint64> placeOnTop(TimelineData& data, const QVector<PlacedClip>& clips);

// ---- Clipboard ----

/// One clip in a clipboard: the clip with a start relative to the clipboard origin and its
/// track as an offset from the lowest copied track of the same type.
struct ClipboardClip
{
    TrackType type = TrackType::Video;
    int trackOffset = 0; ///< 0 = lowest copied track of this type
    ClipData clip; ///< clip.start is relative to the earliest copied clip (>= 0)
};

/// A copied set of clips (value type; safe to keep across edits and undo).
struct Clipboard
{
    QVector<ClipboardClip> clips;
    int baseVideoTrack = 0; ///< index of the lowest copied video track at copy time
    int baseAudioTrack = 0; ///< index of the lowest copied audio track at copy time
    int64_t duration = 0; ///< frames from the earliest start to the latest end

    [[nodiscard]] bool isEmpty() const { return clips.isEmpty(); }
};

/// Copies the given clips (unknown ids ignored) into a Clipboard.
[[nodiscard]] Clipboard copyClips(const TimelineData& data, const QVector<quint64>& clipIds);

/// Pastes @p clipboard with its origin at timeline frame @p frame. Clips go to
/// base + trackOffset, where the base is @p videoBase / @p audioBase (-1 = the tracks they were
/// copied from); missing tracks are added. Every pasted clip gets fresh clip, link-group and
/// effect ids (linked clips stay linked to each other). Overwrites by default; with
/// @p insert = true the timeline ripples (RippleScope::AllTracks) to make room.
/// Returns the new clip ids (empty if nothing could be pasted, e.g. a locked destination).
QVector<quint64> pasteClips(TimelineData& data, const Clipboard& clipboard, int64_t frame,
    int videoBase = -1, int audioBase = -1, bool insert = false);

// ---- Tracks ----

/// Adds an empty track of @p type at @p index (-1 = on top). Returns the index used.
int addTrack(TimelineData& data, TrackType type, int index = -1);
/// Removes a track and its clips. Returns false if it does not exist or is locked.
bool removeTrack(TimelineData& data, TrackRef ref);

// ---- Linking ----

/// Puts the given clips into one new link group. Returns the group id (0 if fewer than 2 clips).
quint64 linkClips(TimelineData& data, const QVector<quint64>& clipIds);
/// Removes the given clips from their link groups (a group left with one member is dissolved).
void unlinkClips(TimelineData& data, const QVector<quint64>& clipIds);
/// Ids of all clips linked with @p clipId, including itself (just {clipId} if unlinked).
[[nodiscard]] QVector<quint64> linkedClips(const TimelineData& data, quint64 clipId);
/// @p clipIds plus every clip linked to one of them, without duplicates.
[[nodiscard]] QVector<quint64> withLinkedClips(
    const TimelineData& data, const QVector<quint64>& clipIds);

// ---- Snapping & navigation ----

/// Sorted, unique frames worth snapping to: 0, the playhead, in/out marks, markers and the
/// start/end of every clip not listed in @p excludeClipIds (typically the clips being dragged).
[[nodiscard]] std::vector<int64_t> snapCandidates(
    const TimelineData& data, int64_t playheadFrame, const QVector<quint64>& excludeClipIds = {});
/// Candidate closest to @p frame within @p toleranceFrames, if any.
[[nodiscard]] std::optional<int64_t> snapToNearest(
    const std::vector<int64_t>& candidates, int64_t frame, int64_t toleranceFrames);
/// First clip start/end strictly after @p frame on any track.
[[nodiscard]] std::optional<int64_t> nextEditPoint(const TimelineData& data, int64_t frame);
/// Last clip start/end strictly before @p frame on any track.
[[nodiscard]] std::optional<int64_t> previousEditPoint(const TimelineData& data, int64_t frame);

// ---- Invariants ----

/// Checks the structural invariants (sorted, non-overlapping clips, duration >= 1, start >= 0,
/// speed > 0, unique non-zero ids below nextId, track types matching their list).
/// Returns false and describes the first violation in @p why.
[[nodiscard]] bool checkInvariants(const TimelineData& data, QString* why = nullptr);
/// Raises nextId above every id used in @p data (repairs files edited by hand).
void normalizeIds(TimelineData& data);

} // namespace kk::editops
