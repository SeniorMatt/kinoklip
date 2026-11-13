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
/// Replaces the Compositing page composition of clip @p clipId (std::nullopt removes it).
/// False if the clip does not exist or is on a locked track.
bool setClipComposition(TimelineData& data, quint64 clipId, std::optional<Composition> composition);

/// Predicate selecting the tracks an operation applies to.
using TrackFilter = std::function<bool(TrackRef ref, const TrackData& track)>;
/// Filter accepting every track that is not locked (the default for splitAt()).
[[nodiscard]] TrackFilter unlockedTracks();
/// Filter accepting the unlocked tracks whose auto track selector is on (TrackData::autoSelect);
/// used by the playhead-based commands when nothing is selected (docs/behavior-spec.md 6.4).
[[nodiscard]] TrackFilter autoSelectTracks();

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
    /// the receiving tracks plus every other unlocked track whose auto track selector is on,
    /// keeping V/A sync (default; a track with TrackData::autoSelect off is left alone)
    AllTracks
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
/// gaps. With RippleScope::AllTracks every other unlocked auto-select track shifts by the same
/// amount for each removed time range in which it is empty (spec 10.4: keeps sync; a track
/// with content inside the range stays where it is). Include linked clips to keep them in
/// sync. Locked tracks never change. Returns the number of clips removed.
int rippleDeleteClips(TimelineData& data, const QVector<quint64>& clipIds,
    RippleScope scope = RippleScope::TargetTracks);

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
/// Ripple sync (spec 8.18): a ripple trim of @p trimmedIds also shifts the clips that start at
/// or after @p point (the primary clip's end before the trim) on every other unlocked
/// auto-select track, so the tracks stay in sync. Tracks holding a trimmed clip are left to the
/// trims themselves; a clip that spans @p point stays where it is.
/// Largest number of frames those clips can shift left before one meets the clip before it.
int64_t rippleSyncRoom(const TimelineData& data, const QVector<quint64>& trimmedIds, int64_t point);
/// Shifts those clips by @p shift frames (negative = left; the caller limits it with rippleSyncRoom).
void rippleSyncShift(
    TimelineData& data, const QVector<quint64>& trimmedIds, int64_t point, int64_t shift);
/// Ripple-trims the head (@p head) or tail of the first clip of @p ids by @p delta, the other
/// clips of @p ids (linked edges) by the same applied amount, and shifts the other auto-select
/// tracks along (rippleSyncShift). @p delta is first limited so no other track collides.
/// Returns the applied delta.
int64_t rippleTrimSynced(TimelineData& data, const QVector<quint64>& ids, bool head, int64_t delta);
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
/// With RippleScope::AllTracks every other unlocked auto-select track is shifted by the same
/// amount, which
/// is reduced to the room that is free on all of them in the gap range (keeps sync).
/// Returns the number of frames removed (0 if there is no gap or the track is locked).
/// Linked clips never drift apart (spec 10.16): the track of a moving clip's partner moves too if
/// it has room; otherwise the tracks that would split a link stay. If that leaves the clicked
/// track unable to move, nothing changes and @p brokeSync (if given) is set to true.
int64_t closeGap(TimelineData& data, TrackRef ref, int64_t frame,
    RippleScope scope = RippleScope::TargetTracks, bool* brokeSync = nullptr);

/// True if closeGap() with the same arguments would remove at least one frame. False after the
/// last clip (open end), on a locked track, or when other auto-select tracks leave no room.
[[nodiscard]] bool canCloseGap(const TimelineData& data, TrackRef ref, int64_t frame,
    RippleScope scope = RippleScope::TargetTracks);

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
    int baseSubtitleTrack = 0; ///< index of the lowest copied subtitle track at copy time
    int64_t duration = 0; ///< frames from the earliest start to the latest end

    [[nodiscard]] bool isEmpty() const { return clips.isEmpty(); }
};

/// Copies the given clips (unknown ids ignored) into a Clipboard. Transitions travel with
/// them: those between two copied clips and those at a copied clip's head stay where they
/// are; one at the cut after a copied clip whose right neighbour is not copied (stored on that
/// neighbour) becomes the copy's tail transition.
[[nodiscard]] Clipboard copyClips(const TimelineData& data, const QVector<quint64>& clipIds);

/// Pastes @p clipboard with its origin at timeline frame @p frame. Clips go to
/// base + trackOffset, where the base is @p videoBase / @p audioBase (-1 = the tracks they were
/// copied from; captions always go back to the subtitle tracks they came from); missing tracks
/// are added. Every pasted clip gets fresh clip, link-group and
/// effect ids (linked clips stay linked to each other); their transitions are fitted to the
/// new neighbours (fitTransitions()). Overwrites by default; with
/// @p insert = true the timeline ripples (RippleScope::AllTracks) to make room.
/// Returns the new clip ids (empty if nothing could be pasted, e.g. a locked destination).
QVector<quint64> pasteClips(TimelineData& data, const Clipboard& clipboard, int64_t frame,
    int videoBase = -1, int audioBase = -1, bool insert = false);

// ---- In / out ranges (EditOpsRipple.cpp) ----

/// Lift: clears [@p in, @p out) on every track accepted by @p filter (default: every
/// unlocked track), splitting clips at the range edges and leaving a gap.
/// Returns whether anything was removed.
bool liftRange(TimelineData& data, int64_t in, int64_t out, const TrackFilter& filter = {});
/// Extract (ripple delete of a range): like liftRange(), then every later clip on the same
/// tracks moves left by out - in. Returns whether anything changed.
bool extractRange(TimelineData& data, int64_t in, int64_t out, const TrackFilter& filter = {});
/// Clipboard holding the parts of the clips inside [@p in, @p out) on the tracks accepted by
/// @p filter (clips straddling the edges are cut at them). Its origin is @p in, so pasting
/// keeps empty room at the start of the range; duration = out - in.
[[nodiscard]] Clipboard copyRange(
    const TimelineData& data, int64_t in, int64_t out, const TrackFilter& filter = {});

/// Predicate accepting exactly the tracks in @p tracks (locked ones are still refused by the
/// range operations above).
[[nodiscard]] TrackFilter trackSetFilter(const std::vector<TrackRef>& tracks);
/// @p tracks plus the tracks of every clip linked to a clip that touches [@p in, @p out) on one
/// of them (the Vim operators with linked selection on: the same frames go from the partner
/// tracks, so picture and sound stay together). Sorted by type and index, without duplicates.
[[nodiscard]] std::vector<TrackRef> tracksWithLinkedPartners(const TimelineData& data,
    const std::vector<TrackRef>& tracks, int64_t in, int64_t out);

/// Delete gaps: removes every time range in which all tracks accepted by @p filter (default:
/// unlocked auto-select tracks) are empty, up to the end of the last clip, shifting later clips
/// on those tracks left, so sync between them is kept. Returns the frames removed in total.
int64_t deleteGaps(TimelineData& data, const TrackFilter& filter = {});

// ---- Joining, swapping, ripple overwrite (EditOpsRipple.cpp) ----

/// True if @p leftClipId and @p rightClipId touch on one unlocked track and form a through
/// edit: same media / kind, same speed, the right clip continues the source exactly where the
/// left one ends, and no transition sits on the cut.
[[nodiscard]] bool canJoin(const TimelineData& data, quint64 leftClipId, quint64 rightClipId);
/// Join: merges a through edit (canJoin()) into the left clip; the right clip's keyframes and
/// tail transition move over. Returns whether the clips were joined.
bool joinClips(TimelineData& data, quint64 leftClipId, quint64 rightClipId);

/// Swap (spec 21.6): exchanges clip @p clipId with its previous (@p direction < 0) or next
/// (> 0) neighbour on its track; a gap between them stays between them. Transitions on the
/// two clips are removed (their neighbours change). Returns the id of the neighbour that was
/// swapped, or 0 (no neighbour, locked track).
quint64 swapWithNeighbour(TimelineData& data, quint64 clipId, int direction);
/// Shuffle move (spec 21.6, Ctrl+Shift drag): takes clip @p clipId out of its track (closing
/// the hole) and inserts it at @p frame (frame of the track after removal; snapped by the
/// caller to an edit point), shifting the clips after it. Only its own track changes.
/// Returns whether the clip moved.
bool shuffleClip(TimelineData& data, quint64 clipId, int64_t frame);

/// Ripple overwrite (spec 4.11): replaces [@p rangeStart, @p rangeEnd) on the clips' tracks
/// with @p clips (all starting at rangeStart) and ripples everything after the range by the
/// length difference, on the receiving tracks and (scope AllTracks) the other unlocked
/// auto-select tracks; when the new material is shorter, another track only follows if the
/// room it gives up is empty. Returns the new ids in input order (empty on invalid input).
QVector<quint64> rippleOverwriteClips(TimelineData& data, const QVector<PlacedClip>& clips,
    int64_t rangeStart, int64_t rangeEnd, RippleScope scope = RippleScope::AllTracks);

// ---- Transitions (EditOpsTransitions.cpp) ----

/// Frames of media a transition still needs beyond the clips' handles (spec 13.7).
struct HandleShortfall
{
    int64_t outgoing = 0; ///< frames missing after the outgoing clip's out point
    int64_t incoming = 0; ///< frames missing before the incoming clip's in point
    [[nodiscard]] bool any() const { return outgoing > 0 || incoming > 0; }
};

/// Timeline frames [first, second) covered by the transition at the head (@p head) or tail of
/// clip @p clipId (placed by its alignment at a cut, a fade inside the clip otherwise).
/// std::nullopt if there is no such transition.
[[nodiscard]] std::optional<std::pair<int64_t, int64_t>> transitionWindow(
    const TimelineData& data, quint64 clipId, bool head);
/// Longest transition that fits at that edge with @p alignment: within the clip lengths and,
/// with @p limitByHandles, within the media handles of both clips (a fade needs no handle).
/// 0 if the clip is unknown.
[[nodiscard]] int64_t maxTransitionDuration(const TimelineData& data, quint64 clipId, bool head,
    TransitionAlignment alignment, bool limitByHandles);
/// Handle frames missing for a transition of @p duration frames with @p alignment at that edge.
[[nodiscard]] HandleShortfall transitionShortfall(const TimelineData& data, quint64 clipId,
    bool head, int64_t duration, TransitionAlignment alignment);
/// Sets the length of the existing transition at that edge (clamped to [1, the clip-length
/// limit]). Returns the new length, or 0 if there is no transition or the track is locked.
int64_t setTransitionDuration(TimelineData& data, quint64 clipId, bool head, int64_t frames);
/// Changes the alignment of the transition at that edge (shortened if it no longer fits the
/// clips). Returns whether anything changed.
bool setTransitionAlignment(
    TimelineData& data, quint64 clipId, bool head, TransitionAlignment alignment);
/// The neighbour of clip @p clipId that shares its head (@p head) or tail cut, or 0.
[[nodiscard]] quint64 cutNeighbour(const TimelineData& data, quint64 clipId, bool head);
/// Puts a copy of @p transition at the head (@p head) or tail of clip @p clipId, replacing
/// the one there. A tail that touches the next clip is stored as that clip's head (the cut's
/// one transition); the length is shortened to fit the clips. Returns false (nothing changed)
/// for an unknown clip, a locked track or an edge with no room.
bool placeTransition(
    TimelineData& data, quint64 clipId, bool head, const TransitionData& transition);
/// Fits the transitions of the given clips to their (new) neighbours, e.g. after a paste:
/// each is shortened to the longest that fits the clip lengths, dropped where nothing fits or
/// where it hides behind the incoming clip's own transition at the same cut.
void fitTransitions(TimelineData& data, const QVector<quint64>& clipIds);

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

/// Snapping distance in pixels: the same at every zoom (spec 11.3) and for every gesture.
inline constexpr int kSnapDistancePx = 8;

/// Sorted, unique frames worth snapping to: 0, the playhead, in/out marks, timeline markers
/// (both ends of a ranged marker), and the start/end and visible clip markers (at their
/// timeline frames, both ends of a ranged one) of every clip not listed in @p excludeClipIds
/// (typically the clips being dragged).
[[nodiscard]] std::vector<int64_t> snapCandidates(
    const TimelineData& data, int64_t playheadFrame, const QVector<quint64>& excludeClipIds = {});
/// Candidate closest to @p frame within @p toleranceFrames, if any.
[[nodiscard]] std::optional<int64_t> snapToNearest(
    const std::vector<int64_t>& candidates, int64_t frame, int64_t toleranceFrames);
/// First clip start/end strictly after @p frame on the tracks accepted by @p filter (default:
/// every track).
[[nodiscard]] std::optional<int64_t> nextEditPoint(
    const TimelineData& data, int64_t frame, const TrackFilter& filter = {});
/// Last clip start/end strictly before @p frame on the tracks accepted by @p filter (default:
/// every track).
[[nodiscard]] std::optional<int64_t> previousEditPoint(
    const TimelineData& data, int64_t frame, const TrackFilter& filter = {});
/// First marker strictly after @p frame (spec 2.9).
[[nodiscard]] std::optional<int64_t> nextMarker(const TimelineData& data, int64_t frame);
/// Last marker strictly before @p frame (spec 2.9).
[[nodiscard]] std::optional<int64_t> previousMarker(const TimelineData& data, int64_t frame);
/// The clip covering @p frame, searching the video tracks from V1 upwards, then the
/// audio tracks, restricted to @p filter (default: every track). nullptr if none.
[[nodiscard]] const ClipData* clipUnderFrame(
    const TimelineData& data, int64_t frame, const TrackFilter& filter = {});

// ---- Invariants ----

/// Checks the structural invariants (sorted, non-overlapping clips, duration >= 1, start >= 0,
/// speed > 0, unique non-zero ids below nextId, track types matching their list).
/// Returns false and describes the first violation in @p why.
[[nodiscard]] bool checkInvariants(const TimelineData& data, QString* why = nullptr);
/// Raises nextId above every id used in @p data (repairs files edited by hand).
void normalizeIds(TimelineData& data);
/// Keeps the fade handles inside the clip: 0 <= fadeIn, 0 <= fadeOut and
/// fadeIn + fadeOut <= duration (spec 59.13). Call it wherever a clip got shorter. When both do not
/// fit, the fade at the edge that was NOT cut is kept: fade-out is shortened first, fade-in
/// first when @p headCut. A fade that becomes 0 loses its curve. Transition lengths are held
/// to 2 * duration (the longest a clip can carry at a cut).
void clampFades(ClipData& clip, bool headCut = false);

} // namespace kk::editops
