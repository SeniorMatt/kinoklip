// SPDX-License-Identifier: GPL-3.0-or-later
// Implementation of the pure timeline editing functions declared in EditOps.h.
#include "core/EditOps.h"

#include "core/EditOpsInternal.h"
#include "core/Subtitles.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace kk::editops {

using namespace detail;

// ---- Lookup ----

std::optional<ClipLocation> findClip(const TimelineData& data, quint64 clipId)
{
    if (clipId == 0)
        return std::nullopt;
    for (TrackType type : kTrackTypes) {
        const auto& list = data.tracks(type);
        for (size_t t = 0; t < list.size(); ++t) {
            const int index = list[t].indexOfClip(clipId);
            if (index >= 0)
                return ClipLocation {TrackRef {type, int(t)}, index};
        }
    }
    return std::nullopt;
}

ClipData* clipById(TimelineData& data, quint64 clipId)
{
    const auto loc = findClip(data, clipId);
    return loc ? &data.track(loc->track)->clips[size_t(loc->clipIndex)] : nullptr;
}

const ClipData* clipById(const TimelineData& data, quint64 clipId)
{
    const auto loc = findClip(data, clipId);
    return loc ? &data.track(loc->track)->clips[size_t(loc->clipIndex)] : nullptr;
}

bool setClipComposition(TimelineData& data, quint64 clipId, std::optional<Composition> composition)
{
    const auto loc = findClip(data, clipId);
    TrackData* track = loc ? data.track(loc->track) : nullptr;
    if (!track || track->locked)
        return false;
    track->clips[size_t(loc->clipIndex)].composition = std::move(composition);
    return true;
}

TrackFilter autoSelectTracks()
{
    return [](TrackRef, const TrackData& track) { return !track.locked && track.autoSelect; };
}

TrackFilter unlockedTracks()
{
    return [](TrackRef, const TrackData& track) { return !track.locked; };
}

// ---- Placing clips ----

void clearRange(TimelineData& data, TrackRef ref, int64_t startFrame, int64_t endFrame)
{
    TrackData* track = data.track(ref);
    if (!track)
        return;
    GroupRemap remap;
    clearRangeImpl(data, *track, startFrame, endFrame, remap);
}

namespace {
bool validPlacement(const TimelineData& data, const PlacedClip& placed)
{
    const TrackData* track = data.track(placed.track);
    return track && !track->locked && placed.clip.duration >= 1 && std::isfinite(placed.clip.speed);
}

} // namespace

QVector<quint64> overwriteClips(TimelineData& data, const QVector<PlacedClip>& clips)
{
    for (const PlacedClip& placed : clips) {
        if (!validPlacement(data, placed))
            return {};
    }
    QVector<quint64> ids;
    GroupRemap remap;
    for (PlacedClip placed : clips) {
        ClipData& clip = placed.clip;
        clip.start = std::max<int64_t>(0, clip.start);
        TrackData* track = data.track(placed.track);
        clearRangeImpl(data, *track, clip.start, clip.end(), remap);
        ensureId(data, clip);
        ids.append(clip.id);
        insertSorted(*track, std::move(clip));
    }
    return ids;
}

quint64 overwriteClip(TimelineData& data, TrackRef ref, ClipData clip)
{
    const QVector<quint64> ids = overwriteClips(data, {PlacedClip {ref, std::move(clip)}});
    return ids.isEmpty() ? 0 : ids.front();
}

QVector<quint64> insertClips(
    TimelineData& data, const QVector<PlacedClip>& clips, RippleScope scope)
{
    if (clips.isEmpty())
        return {};
    int64_t insertAt = kUnbounded;
    int64_t gapEnd = 0;
    for (const PlacedClip& placed : clips) {
        if (!validPlacement(data, placed))
            return {};
        insertAt = std::min(insertAt, std::max<int64_t>(0, placed.clip.start));
    }
    for (const PlacedClip& placed : clips)
        gapEnd = std::max(gapEnd, std::max<int64_t>(0, placed.clip.start) + placed.clip.duration);
    const int64_t gap = gapEnd - insertAt;

    QVector<TrackRef> targets;
    for (const PlacedClip& placed : clips)
        targets.append(placed.track);

    // Open the gap: split whatever is under the insert point, then push later clips right.
    GroupRemap remap;
    forEachTrack(data, [&](TrackRef ref, TrackData& track) {
        if (track.locked)
            return;
        if (!targets.contains(ref) && (scope == RippleScope::TargetTracks || !track.autoSelect))
            return;
        for (size_t i = 0; i < track.clips.size(); ++i) {
            if (track.clips[i].start < insertAt && track.clips[i].end() > insertAt) {
                splitClipAtIndex(data, track, i, insertAt, remap);
                break;
            }
        }
        for (ClipData& c : track.clips) {
            if (c.start >= insertAt)
                c.start += gap;
        }
    });

    QVector<quint64> ids;
    for (PlacedClip placed : clips) {
        ClipData& clip = placed.clip;
        clip.start = std::max<int64_t>(0, clip.start);
        TrackData* track = data.track(placed.track);
        clearRangeImpl(data, *track, clip.start, clip.end(), remap); // no-op unless spans differ
        ensureId(data, clip);
        ids.append(clip.id);
        insertSorted(*track, std::move(clip));
    }
    return ids;
}

quint64 insertClip(TimelineData& data, TrackRef ref, ClipData clip, RippleScope scope)
{
    const QVector<quint64> ids = insertClips(data, {PlacedClip {ref, std::move(clip)}}, scope);
    return ids.isEmpty() ? 0 : ids.front();
}

// ---- Removing ----

int liftClips(TimelineData& data, const QVector<quint64>& clipIds)
{
    const QSet<quint64> ids(clipIds.cbegin(), clipIds.cend());
    int removed = 0;
    forEachTrack(data, [&](TrackRef, TrackData& track) {
        if (track.locked)
            return;
        const auto newEnd = std::remove_if(track.clips.begin(), track.clips.end(),
            [&](const ClipData& c) { return ids.contains(c.id); });
        removed += int(track.clips.end() - newEnd);
        track.clips.erase(newEnd, track.clips.end());
    });
    return removed;
}

// rippleDeleteClips() lives in EditOpsRipple.cpp with the other ripple / range operations.

// ---- Splitting ----

QVector<quint64> splitClips(TimelineData& data, const QVector<quint64>& clipIds, int64_t frame)
{
    QVector<quint64> rightIds;
    GroupRemap remap;
    for (quint64 id : clipIds) {
        const auto loc = findClip(data, id);
        if (!loc)
            continue;
        TrackData& track = *data.track(loc->track);
        if (track.locked)
            continue;
        if (const quint64 right
            = splitClipAtIndex(data, track, size_t(loc->clipIndex), frame, remap))
            rightIds.append(right);
    }
    return rightIds;
}

QVector<quint64> splitAt(TimelineData& data, int64_t frame, const TrackFilter& filter)
{
    const TrackFilter accept = filter ? filter : unlockedTracks();
    QVector<quint64> rightIds;
    GroupRemap remap;
    forEachTrack(data, [&](TrackRef ref, TrackData& track) {
        if (track.locked || !accept(ref, track))
            return;
        for (size_t i = 0; i < track.clips.size(); ++i) {
            if (track.clips[i].start < frame && track.clips[i].end() > frame) {
                rightIds.append(splitClipAtIndex(data, track, i, frame, remap));
                break;
            }
        }
    });
    return rightIds;
}

// ---- Moving ----

bool moveClips(
    TimelineData& data, const QVector<quint64>& clipIds, int64_t deltaFrames, int deltaTrack)
{
    struct Moving
    {
        ClipLocation from;
        quint64 id;
    };
    std::vector<Moving> moving;
    QSet<quint64> seen;
    int64_t minStart = kUnbounded;
    int minIndex[kTrackTypeCount];
    int maxIndex[kTrackTypeCount];
    std::fill(std::begin(minIndex), std::end(minIndex), std::numeric_limits<int>::max());
    std::fill(std::begin(maxIndex), std::end(maxIndex), -1);
    for (quint64 id : clipIds) {
        if (seen.contains(id))
            continue;
        const auto loc = findClip(data, id);
        if (!loc)
            continue;
        if (data.track(loc->track)->locked)
            return false;
        seen.insert(id);
        moving.push_back({*loc, id});
        const int t = trackTypeIndex(loc->track.type);
        minIndex[t] = std::min(minIndex[t], loc->track.index);
        maxIndex[t] = std::max(maxIndex[t], loc->track.index);
        minStart = std::min(minStart, data.track(loc->track)->clips[size_t(loc->clipIndex)].start);
    }
    if (moving.empty())
        return false;

    // Clamp as a group so the clips keep their relative positions.
    const int64_t dx = std::max(deltaFrames, -minStart);
    int dy[kTrackTypeCount] = {};
    for (int t = 0; t < kTrackTypeCount; ++t) {
        if (maxIndex[t] < 0)
            continue;
        const int count = int(data.tracks(kTrackTypes[t]).size());
        dy[t] = std::clamp(deltaTrack, -minIndex[t], count - 1 - maxIndex[t]);
    }
    if (dx == 0 && dy[0] == 0 && dy[1] == 0 && dy[2] == 0)
        return false;
    for (const Moving& m : moving) {
        const int t = trackTypeIndex(m.from.track.type);
        const TrackRef dest {m.from.track.type, m.from.track.index + dy[t]};
        if (data.track(dest)->locked)
            return false;
    }

    // Take the clips out first so they never overwrite each other, then drop them in place.
    std::vector<std::pair<TrackRef, ClipData>> lifted;
    for (const Moving& m : moving) {
        TrackData& track = *data.track(m.from.track);
        const int index = track.indexOfClip(m.id);
        ClipData clip = track.clips[size_t(index)];
        track.clips.erase(track.clips.begin() + index);
        const int t = trackTypeIndex(m.from.track.type);
        clip.start += dx;
        lifted.emplace_back(TrackRef {m.from.track.type, m.from.track.index + dy[t]}, clip);
    }
    GroupRemap remap;
    for (auto& [dest, clip] : lifted) {
        TrackData& track = *data.track(dest);
        clearRangeImpl(data, track, clip.start, clip.end(), remap);
        insertSorted(track, std::move(clip));
    }
    return true;
}

// ---- Trimming ----

int64_t maxHeadExtension(const ClipData& clip, Rational fps)
{
    if (!isBounded(clip))
        return kUnbounded;
    if (!isPlainForward(clip)) // retime curve, reverse or freeze: walk the curve
        return retime::validRun(clip, -1, -1, fps, kUnbounded);
    const double perFrame = usPerFrame(clip, fps);
    if (perFrame <= 0)
        return 0;
    // Small epsilon so exact multiples are not lost to floating point error.
    return int64_t(std::floor(double(clip.sourceInUs) / perFrame + 1e-6));
}

int64_t maxTailExtension(const ClipData& clip, Rational fps)
{
    if (!isBounded(clip))
        return kUnbounded;
    if (!isPlainForward(clip))
        return retime::validRun(clip, clip.duration, +1, fps, kUnbounded);
    const double perFrame = usPerFrame(clip, fps);
    if (perFrame <= 0)
        return 0;
    const int64_t usedEndUs = clip.sourceInUs + clip.framesToSourceUs(clip.duration, fps);
    const int64_t availableUs = clip.sourceDurationUs - usedEndUs;
    return std::max<int64_t>(0, int64_t(std::floor(double(availableUs) / perFrame + 1e-6)));
}

int64_t trimHead(TimelineData& data, quint64 clipId, int64_t delta, bool ripple)
{
    const auto loc = findClip(data, clipId);
    if (!loc)
        return 0;
    TrackData& track = *data.track(loc->track);
    if (track.locked)
        return 0;
    const size_t index = size_t(loc->clipIndex);
    ClipData& clip = track.clips[index];

    int64_t lo = -maxHeadExtension(clip, data.fps);
    const int64_t hi = clip.duration - 1;
    if (!ripple) {
        const int64_t previousEnd = index > 0 ? track.clips[index - 1].end() : 0;
        lo = std::max(lo, previousEnd - clip.start);
    }
    const int64_t applied = std::clamp(delta, std::min<int64_t>(lo, 0), std::max<int64_t>(hi, 0));
    if (applied == 0)
        return 0;

    moveHead(clip, applied, data.fps);
    if (ripple) {
        // The clip stays where it starts; everything after it closes up (or makes room).
        clip.start -= applied;
        for (size_t i = index + 1; i < track.clips.size(); ++i)
            track.clips[i].start -= applied;
    }
    return applied;
}

int64_t trimTail(TimelineData& data, quint64 clipId, int64_t delta, bool ripple)
{
    const auto loc = findClip(data, clipId);
    if (!loc)
        return 0;
    TrackData& track = *data.track(loc->track);
    if (track.locked)
        return 0;
    const size_t index = size_t(loc->clipIndex);
    ClipData& clip = track.clips[index];

    const int64_t lo = -(clip.duration - 1);
    int64_t hi = maxTailExtension(clip, data.fps);
    if (!ripple && index + 1 < track.clips.size())
        hi = std::min(hi, track.clips[index + 1].start - clip.end());
    const int64_t applied = std::clamp(delta, std::min<int64_t>(lo, 0), std::max<int64_t>(hi, 0));
    if (applied == 0)
        return 0;

    clip.duration += applied;
    clampFades(clip);
    if (ripple) {
        for (size_t i = index + 1; i < track.clips.size(); ++i)
            track.clips[i].start += applied;
    }
    return applied;
}

int64_t rippleSyncRoom(const TimelineData& data, const QVector<quint64>& trimmedIds, int64_t point)
{
    QVector<TrackRef> skip;
    for (quint64 id : trimmedIds)
        if (const auto loc = findClip(data, id))
            skip.append(loc->track);
    int64_t room = std::numeric_limits<int64_t>::max();
    const TrackFilter accept = autoSelectTracks();
    for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
        const auto& list = data.tracks(type);
        for (size_t t = 0; t < list.size(); ++t) {
            const TrackRef ref {type, int(t)};
            if (skip.contains(ref) || !accept(ref, list[t]))
                continue;
            int64_t prevEnd = 0;
            int64_t firstStart = -1;
            for (const ClipData& c : list[t].clips) {
                if (c.start < point)
                    prevEnd = std::max(prevEnd, c.end());
                else if (firstStart < 0 || c.start < firstStart)
                    firstStart = c.start;
            }
            if (firstStart >= 0)
                room = std::min(room, std::max<int64_t>(0, firstStart - prevEnd));
        }
    }
    return room;
}

void rippleSyncShift(
    TimelineData& data, const QVector<quint64>& trimmedIds, int64_t point, int64_t shift)
{
    if (shift == 0)
        return;
    QVector<TrackRef> skip;
    for (quint64 id : trimmedIds)
        if (const auto loc = findClip(data, id))
            skip.append(loc->track);
    const TrackFilter accept = autoSelectTracks();
    forEachTrack(data, [&](TrackRef ref, TrackData& track) {
        if (skip.contains(ref) || !accept(ref, track))
            return;
        for (ClipData& c : track.clips)
            if (c.start >= point)
                c.start += shift;
    });
}

int64_t rippleTrimSynced(TimelineData& data, const QVector<quint64>& ids, bool head, int64_t delta)
{
    if (ids.isEmpty())
        return 0;
    const ClipData* primary = clipById(data, ids.first());
    if (!primary)
        return 0;
    const int64_t point = primary->end();
    // Shortening pulls the other tracks left: never further than they have room for.
    const int64_t room = rippleSyncRoom(data, ids, point);
    if (head)
        delta = std::min(delta, room);
    else
        delta = std::max(delta, -room);
    const auto trim = [&](quint64 id, int64_t d) {
        return head ? trimHead(data, id, d, true) : trimTail(data, id, d, true);
    };
    const int64_t applied = trim(ids.first(), delta);
    if (applied == 0)
        return 0;
    for (int i = 1; i < ids.size(); ++i)
        trim(ids[i], applied);
    rippleSyncShift(data, ids, point, head ? -applied : applied);
    return applied;
}

int64_t slipClip(TimelineData& data, quint64 clipId, int64_t deltaFrames)
{
    ClipData* clip = clipById(data, clipId);
    if (!clip || data.track(findClip(data, clipId)->track)->locked)
        return 0;
    // Slipping right shows later source material: the in point moves forward, limited by the
    // media left after the out point; slipping left is limited by the media before the in point.
    if (isPlainForward(*clip) || !isBounded(*clip)) {
        const int64_t applied = std::clamp(
            deltaFrames, -maxHeadExtension(*clip, data.fps), maxTailExtension(*clip, data.fps));
        const int64_t us = isPlainForward(*clip)
            ? clip->framesToSourceUs(applied, data.fps)
            : int64_t(std::llround(double(applied) * usPerFrame(*clip, data.fps)));
        clip->sourceInUs = std::max<int64_t>(0, clip->sourceInUs + us);
        return applied;
    }
    // Retimed (curve, reverse, freeze): every shown source time moves by the same amount, one
    // frame of slip = one source frame at |speed| (one frame at the timeline rate for curves),
    // limited by the source range the clip shows.
    const double unit
        = usPerFrame(*clip, data.fps) > 0 ? usPerFrame(*clip, data.fps) : retime::frameUs(data.fps);
    const retime::SourceRange shown = retime::shownSourceRange(*clip, 0, clip->duration, data.fps);
    const int64_t lo
        = -int64_t(std::floor(double(std::max<int64_t>(0, shown.minUs)) / unit + 1e-6));
    const int64_t hi = int64_t(std::floor(
        double(std::max<int64_t>(0, clip->sourceDurationUs - 1 - shown.maxUs)) / unit + 1e-6));
    const int64_t applied = std::clamp(deltaFrames, lo, hi);
    clip->sourceInUs
        = std::max<int64_t>(0, clip->sourceInUs + int64_t(std::llround(double(applied) * unit)));
    return applied;
}

// ---- Roll, slide, gaps ----

int64_t rollEdit(TimelineData& data, quint64 leftClipId, quint64 rightClipId, int64_t delta)
{
    const auto left = findClip(data, leftClipId);
    const auto right = findClip(data, rightClipId);
    if (!left || !right || !(left->track == right->track)
        || right->clipIndex != left->clipIndex + 1)
        return 0;
    TrackData& track = *data.track(left->track);
    if (track.locked)
        return 0;
    ClipData& a = track.clips[size_t(left->clipIndex)];
    ClipData& b = track.clips[size_t(right->clipIndex)];
    if (a.end() != b.start)
        return 0;
    // Rolling right extends a's tail (needs media after a's out point) and shortens b's head;
    // rolling left is the mirror image.
    const int64_t hi = std::min(maxTailExtension(a, data.fps), b.duration - 1);
    const int64_t lo = -std::min(a.duration - 1, maxHeadExtension(b, data.fps));
    const int64_t applied = std::clamp(delta, std::min<int64_t>(lo, 0), std::max<int64_t>(hi, 0));
    if (applied == 0)
        return 0;
    a.duration += applied;
    clampFades(a);
    moveHead(b, applied, data.fps);
    return applied;
}

int64_t slideClip(TimelineData& data, quint64 clipId, int64_t delta)
{
    const auto loc = findClip(data, clipId);
    if (!loc)
        return 0;
    TrackData& track = *data.track(loc->track);
    if (track.locked)
        return 0;
    const size_t i = size_t(loc->clipIndex);
    ClipData* prev = i > 0 ? &track.clips[i - 1] : nullptr;
    ClipData* next = i + 1 < track.clips.size() ? &track.clips[i + 1] : nullptr;
    const ClipData& clip = track.clips[i];
    const bool prevTouches = prev && prev->end() == clip.start;
    const bool nextTouches = next && next->start == clip.end();

    // Each side limits both directions: a touching neighbour by its duration (shrinking) and its
    // media handle (growing); a gap by its size (shrinking) and not at all (growing).
    int64_t hi = kUnbounded;
    int64_t lo = -kUnbounded;
    if (prevTouches) {
        hi = std::min(hi, maxTailExtension(*prev, data.fps));
        lo = std::max(lo, -(prev->duration - 1));
    } else {
        lo = std::max(lo, -(clip.start - (prev ? prev->end() : 0)));
    }
    if (nextTouches) {
        hi = std::min(hi, next->duration - 1);
        lo = std::max(lo, -maxHeadExtension(*next, data.fps));
    } else if (next) {
        hi = std::min(hi, next->start - clip.end());
    }
    const int64_t applied = std::clamp(delta, std::min<int64_t>(lo, 0), std::max<int64_t>(hi, 0));
    if (applied == 0)
        return 0;
    if (prevTouches) {
        prev->duration += applied;
        clampFades(*prev);
    }
    track.clips[i].start += applied; // keyframes are clip-local and move with the clip
    if (nextTouches)
        moveHead(*next, applied, data.fps);
    return applied;
}

std::optional<std::pair<int64_t, int64_t>> gapAt(
    const TimelineData& data, TrackRef ref, int64_t frame)
{
    const TrackData* track = data.track(ref);
    if (!track || frame < 0)
        return std::nullopt;
    // First clip that ends after frame; if it also starts at or before frame it covers it.
    const auto it = std::upper_bound(track->clips.cbegin(), track->clips.cend(), frame,
        [](int64_t f, const ClipData& c) { return f < c.end(); });
    if (it == track->clips.cend() || it->start <= frame)
        return std::nullopt;
    const int64_t gapStart = it == track->clips.cbegin() ? 0 : (it - 1)->end();
    return std::make_pair(gapStart, it->start);
}

bool canCloseGap(const TimelineData& data, TrackRef ref, int64_t frame, RippleScope scope)
{
    TimelineData copy = data; // dry run: closeGap decides, so the answer cannot drift from it
    return closeGap(copy, ref, frame, scope, nullptr) > 0;
}

int64_t closeGap(
    TimelineData& data, TrackRef ref, int64_t frame, RippleScope scope, bool* brokeSync)
{
    if (brokeSync)
        *brokeSync = false;
    const auto gap = gapAt(data, ref, frame);
    const TrackData* target = data.track(ref);
    if (!gap || target->locked)
        return 0;
    const int64_t cut = gap->second; // clips starting here or later move left
    int64_t amount = gap->second - gap->first;
    // Other tracks can only give up the empty room directly left of the cut. A track with no
    // room at all (a clip straddles the cut, or it ends right at it) is left alone instead of
    // blocking the close; the rest move by the same amount, reduced to the least room among
    // them (keeps sync where possible, spec 10.15).
    struct Info
    {
        TrackRef ref;
        int64_t room = 0;
        bool moves = false;
    };
    std::vector<Info> tracks;
    forEachTrack(data, [&](TrackRef r, const TrackData& track) {
        Info info {r, 0, false};
        if (r == ref) {
            info.room = amount;
            info.moves = true;
        } else if (!track.locked) {
            int64_t lastEnd = 0;
            for (const ClipData& c : track.clips) {
                if (c.start >= cut)
                    break;
                lastEnd = std::max(lastEnd, c.end());
            }
            info.room = std::max<int64_t>(0, cut - lastEnd);
            info.moves = scope == RippleScope::AllTracks && track.autoSelect && info.room > 0;
        }
        tracks.push_back(info);
    });
    auto infoOf = [&](TrackRef r) -> Info& {
        return *std::find_if(tracks.begin(), tracks.end(), [&](const Info& i) { return i.ref == r; });
    };

    // Linked clips must never drift apart (spec 10.16). Pull in the track of a moving clip's
    // partner when it has room, then drop every track whose moving clip still has a partner that
    // stays; dropping one can strand another, so repeat until stable.
    auto forEachLinked = [&](auto&& fn) { // fn(TrackRef, const ClipData&) for every grouped clip
        forEachTrack(data, [&](TrackRef r, const TrackData& track) {
            for (const ClipData& c : track.clips) {
                if (c.linkGroup != 0)
                    fn(r, c);
            }
        });
    };
    auto isMoving = [&](TrackRef r, const ClipData& c) { return infoOf(r).moves && c.start >= cut; };
    for (bool changed = true; changed;) {
        changed = false;
        QSet<quint64> movingGroups;
        forEachLinked([&](TrackRef r, const ClipData& c) {
            if (isMoving(r, c))
                movingGroups.insert(c.linkGroup);
        });
        forEachLinked([&](TrackRef r, const ClipData& c) {
            Info& info = infoOf(r);
            if (!info.moves && info.room > 0 && c.start >= cut && movingGroups.contains(c.linkGroup)) {
                info.moves = true;
                changed = true;
            }
        });
    }
    for (bool changed = true; changed;) {
        changed = false;
        QSet<quint64> stayingGroups;
        forEachLinked([&](TrackRef r, const ClipData& c) {
            if (!isMoving(r, c))
                stayingGroups.insert(c.linkGroup);
        });
        forEachLinked([&](TrackRef r, const ClipData& c) {
            Info& info = infoOf(r);
            if (info.moves && c.start >= cut && stayingGroups.contains(c.linkGroup)) {
                info.moves = false;
                changed = true;
            }
        });
    }
    if (!infoOf(ref).moves) {
        if (brokeSync)
            *brokeSync = true;
        return 0;
    }
    for (const Info& info : tracks) {
        if (info.moves && !(info.ref == ref))
            amount = std::min(amount, info.room);
    }
    if (amount <= 0)
        return 0;
    forEachTrack(data, [&](TrackRef r, TrackData& track) {
        if (!infoOf(r).moves)
            return;
        for (ClipData& c : track.clips) {
            if (c.start >= cut)
                c.start -= amount;
        }
    });
    return amount;
}

QVector<quint64> placeOnTop(TimelineData& data, const QVector<PlacedClip>& clips)
{
    for (const PlacedClip& placed : clips) {
        if (placed.clip.duration < 1 || !std::isfinite(placed.clip.speed) || placed.track.index < 0
            || placed.track.index > int(data.tracks(placed.track.type).size()))
            return {};
    }
    QVector<quint64> ids;
    for (PlacedClip placed : clips) {
        ClipData& clip = placed.clip;
        clip.start = std::max<int64_t>(0, clip.start);
        auto& list = data.tracks(placed.track.type);
        int index = placed.track.index;
        while (index < int(list.size())
            && (list[size_t(index)].locked
                || !rangeFree(list[size_t(index)], clip.start, clip.end())))
            ++index;
        if (index >= int(list.size()))
            index = addTrack(data, placed.track.type); // may reallocate `list`
        ensureId(data, clip);
        ids.append(clip.id);
        insertSorted(data.tracks(placed.track.type)[size_t(index)], std::move(clip));
    }
    return ids;
}

// ---- Clipboard ----

Clipboard copyClips(const TimelineData& data, const QVector<quint64>& clipIds)
{
    struct Found
    {
        TrackRef ref;
        const ClipData* clip;
        const ClipData* next; ///< the touching right neighbour, if any
    };
    std::vector<Found> found;
    QSet<quint64> seen;
    int64_t minStart = kUnbounded;
    int64_t maxEnd = 0;
    int minIndex[kTrackTypeCount];
    std::fill(std::begin(minIndex), std::end(minIndex), std::numeric_limits<int>::max());
    for (quint64 id : clipIds) {
        const auto loc = findClip(data, id);
        if (!loc || seen.contains(id))
            continue;
        seen.insert(id);
        const auto& clips = data.track(loc->track)->clips;
        const ClipData* c = &clips[size_t(loc->clipIndex)];
        const size_t nextIndex = size_t(loc->clipIndex) + 1;
        const ClipData* next = nextIndex < clips.size() && clips[nextIndex].start == c->end()
            ? &clips[nextIndex]
            : nullptr;
        found.push_back({loc->track, c, next});
        minStart = std::min(minStart, c->start);
        maxEnd = std::max(maxEnd, c->end());
        int& m = minIndex[trackTypeIndex(loc->track.type)];
        m = std::min(m, loc->track.index);
    }
    Clipboard board;
    if (found.empty())
        return board;
    std::sort(found.begin(), found.end(),
        [](const Found& a, const Found& b) { return a.clip->start < b.clip->start; });
    board.baseVideoTrack = minIndex[0] == std::numeric_limits<int>::max() ? 0 : minIndex[0];
    board.baseAudioTrack = minIndex[1] == std::numeric_limits<int>::max() ? 0 : minIndex[1];
    board.baseSubtitleTrack = minIndex[2] == std::numeric_limits<int>::max() ? 0 : minIndex[2];
    board.duration = maxEnd - minStart;
    const int boardBases[kTrackTypeCount]
        = {board.baseVideoTrack, board.baseAudioTrack, board.baseSubtitleTrack};
    for (const Found& f : found) {
        ClipboardClip entry;
        entry.type = f.ref.type;
        entry.trackOffset = f.ref.index - boardBases[trackTypeIndex(f.ref.type)];
        entry.clip = *f.clip;
        entry.clip.start -= minStart;
        // The cut after the clip keeps its transition on the right-hand clip; when that one
        // stays behind, the copy takes the transition as its tail (owner request 0.4.0).
        if (f.next && f.next->transitionIn && !seen.contains(f.next->id))
            entry.clip.transitionOut = f.next->transitionIn;
        board.clips.append(std::move(entry));
    }
    return board;
}

QVector<quint64> pasteClips(TimelineData& data, const Clipboard& clipboard, int64_t frame,
    int videoBase, int audioBase, bool insert)
{
    if (clipboard.isEmpty())
        return {};
    frame = std::max<int64_t>(0, frame);
    const int bases[kTrackTypeCount] = {videoBase >= 0 ? videoBase : clipboard.baseVideoTrack,
        audioBase >= 0 ? audioBase : clipboard.baseAudioTrack, clipboard.baseSubtitleTrack};
    // Refuse before changing anything if an existing destination track is locked.
    for (const ClipboardClip& entry : clipboard.clips) {
        const int index = bases[trackTypeIndex(entry.type)] + entry.trackOffset;
        const TrackData* track = data.track(TrackRef {entry.type, index});
        if (index < 0 || (track && track->locked))
            return {};
    }
    // Make sure every destination track exists (pasting high tracks into a smaller timeline).
    for (const ClipboardClip& entry : clipboard.clips) {
        const int index = bases[trackTypeIndex(entry.type)] + entry.trackOffset;
        while (int(data.tracks(entry.type).size()) <= index)
            addTrack(data, entry.type);
    }
    QVector<PlacedClip> placed;
    placed.reserve(clipboard.clips.size());
    GroupRemap remap; // fresh link groups, shared between clips that were linked together
    for (const ClipboardClip& entry : clipboard.clips) {
        PlacedClip p;
        p.track = TrackRef {entry.type, bases[trackTypeIndex(entry.type)] + entry.trackOffset};
        p.clip = entry.clip;
        p.clip.id = data.allocateId();
        p.clip.start = frame + entry.clip.start;
        p.clip.linkGroup = remap.map(data, entry.clip.linkGroup);
        for (EffectInstance& fx : p.clip.effects)
            fx.id = data.allocateId();
        for (MaskData& mask : p.clip.masks)
            mask.id = data.allocateId();
        placed.append(std::move(p));
    }
    const QVector<quint64> ids
        = insert ? insertClips(data, placed, RippleScope::AllTracks) : overwriteClips(data, placed);
    fitTransitions(data, ids);
    return ids;
}

// ---- Tracks ----

int addTrack(TimelineData& data, TrackType type, int index)
{
    auto& list = data.tracks(type);
    if (index < 0 || index > int(list.size()))
        index = int(list.size());
    TrackData track = type == TrackType::Subtitle ? subtitles::makeSubtitleTrack() : TrackData {};
    track.id = data.allocateId();
    track.type = type;
    list.insert(list.begin() + index, std::move(track));
    return index;
}

bool removeTrack(TimelineData& data, TrackRef ref)
{
    const TrackData* track = data.track(ref);
    if (!track || track->locked)
        return false;
    auto& list = data.tracks(ref.type);
    list.erase(list.begin() + ref.index);
    return true;
}

// ---- Linking ----

quint64 linkClips(TimelineData& data, const QVector<quint64>& clipIds)
{
    QVector<ClipData*> clips;
    for (quint64 id : clipIds) {
        if (ClipData* c = clipById(data, id); c && !clips.contains(c))
            clips.append(c);
    }
    if (clips.size() < 2)
        return 0;
    const quint64 group = data.allocateId();
    for (ClipData* c : clips)
        c->linkGroup = group;
    return group;
}

void unlinkClips(TimelineData& data, const QVector<quint64>& clipIds)
{
    QSet<quint64> touchedGroups;
    for (quint64 id : clipIds) {
        if (ClipData* c = clipById(data, id); c && c->linkGroup != 0) {
            touchedGroups.insert(c->linkGroup);
            c->linkGroup = 0;
        }
    }
    // Dissolve groups that are left with a single member.
    QHash<quint64, int> members;
    forEachTrack(data, [&](TrackRef, TrackData& track) {
        for (const ClipData& c : track.clips) {
            if (touchedGroups.contains(c.linkGroup))
                ++members[c.linkGroup];
        }
    });
    forEachTrack(data, [&](TrackRef, TrackData& track) {
        for (ClipData& c : track.clips) {
            if (touchedGroups.contains(c.linkGroup) && members.value(c.linkGroup) < 2)
                c.linkGroup = 0;
        }
    });
}

QVector<quint64> linkedClips(const TimelineData& data, quint64 clipId)
{
    const ClipData* clip = clipById(data, clipId);
    if (!clip)
        return {};
    if (clip->linkGroup == 0)
        return {clipId};
    QVector<quint64> result;
    forEachTrack(data, [&](TrackRef, const TrackData& track) {
        for (const ClipData& c : track.clips) {
            if (c.linkGroup == clip->linkGroup)
                result.append(c.id);
        }
    });
    return result;
}

QVector<quint64> withLinkedClips(const TimelineData& data, const QVector<quint64>& clipIds)
{
    QVector<quint64> result;
    for (quint64 id : clipIds) {
        for (quint64 linked : linkedClips(data, id)) {
            if (!result.contains(linked))
                result.append(linked);
        }
    }
    return result;
}

// ---- Snapping & navigation ----

std::vector<int64_t> snapCandidates(
    const TimelineData& data, int64_t playheadFrame, const QVector<quint64>& excludeClipIds)
{
    std::vector<int64_t> frames {0, playheadFrame};
    if (data.markIn >= 0)
        frames.push_back(data.markIn);
    if (data.markOut >= 0)
        frames.push_back(data.markOut);
    for (const Marker& m : data.markers) {
        frames.push_back(m.frame);
        if (m.duration > 1)
            frames.push_back(m.end()); // ranged markers: both ends (spec 18.11)
    }
    forEachTrack(data, [&](TrackRef, const TrackData& track) {
        for (const ClipData& c : track.clips) {
            if (excludeClipIds.contains(c.id))
                continue;
            frames.push_back(c.start);
            frames.push_back(c.end());
            // Clip markers at their timeline position, both ends of a ranged one; markers
            // trimmed out of the visible part are not targets (owner request 0.4.0).
            const auto addVisible = [&](int64_t f) {
                if (f >= c.start && f <= c.end())
                    frames.push_back(f);
            };
            for (const Marker& m : c.markers) {
                addVisible(c.start + m.frame);
                if (m.duration > 1)
                    addVisible(c.start + m.end());
            }
        }
    });
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

std::optional<int64_t> snapToNearest(
    const std::vector<int64_t>& candidates, int64_t frame, int64_t toleranceFrames)
{
    std::optional<int64_t> best;
    int64_t bestDistance = toleranceFrames + 1;
    // Candidates are sorted: only the neighbours of the insertion point can be closest.
    const auto it = std::lower_bound(candidates.cbegin(), candidates.cend(), frame);
    for (auto c = it == candidates.cbegin() ? it : it - 1; c != candidates.cend() && c <= it; ++c) {
        const int64_t distance = std::abs(*c - frame);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = *c;
        }
    }
    return best;
}

namespace {
std::vector<int64_t> editPoints(const TimelineData& data, const TrackFilter& filter)
{
    std::vector<int64_t> frames;
    forEachTrack(data, [&](TrackRef ref, const TrackData& track) {
        if (filter && !filter(ref, track))
            return;
        for (const ClipData& c : track.clips) {
            frames.push_back(c.start);
            frames.push_back(c.end());
        }
    });
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}
} // namespace

std::optional<int64_t> nextEditPoint(
    const TimelineData& data, int64_t frame, const TrackFilter& filter)
{
    const std::vector<int64_t> points = editPoints(data, filter);
    const auto it = std::upper_bound(points.cbegin(), points.cend(), frame);
    if (it == points.cend())
        return std::nullopt;
    return *it;
}

std::optional<int64_t> previousEditPoint(
    const TimelineData& data, int64_t frame, const TrackFilter& filter)
{
    const std::vector<int64_t> points = editPoints(data, filter);
    const auto it = std::lower_bound(points.cbegin(), points.cend(), frame);
    if (it == points.cbegin())
        return std::nullopt;
    return *(it - 1);
}

std::optional<int64_t> nextMarker(const TimelineData& data, int64_t frame)
{
    // Markers are sorted by frame.
    const auto it = std::upper_bound(data.markers.cbegin(), data.markers.cend(), frame,
        [](int64_t f, const Marker& m) { return f < m.frame; });
    if (it == data.markers.cend())
        return std::nullopt;
    return it->frame;
}

std::optional<int64_t> previousMarker(const TimelineData& data, int64_t frame)
{
    const auto it = std::lower_bound(data.markers.cbegin(), data.markers.cend(), frame,
        [](const Marker& m, int64_t f) { return m.frame < f; });
    if (it == data.markers.cbegin())
        return std::nullopt;
    return (it - 1)->frame;
}

const ClipData* clipUnderFrame(const TimelineData& data, int64_t frame, const TrackFilter& filter)
{
    for (const TrackType type : kTrackTypes) {
        const auto& list = data.tracks(type);
        for (size_t i = 0; i < list.size(); ++i) {
            if (filter && !filter(TrackRef {type, int(i)}, list[i]))
                continue;
            if (const ClipData* c = list[i].clipAt(frame))
                return c;
        }
    }
    return nullptr;
}

// ---- Invariants ----

bool checkInvariants(const TimelineData& data, QString* why)
{
    auto fail = [why](const QString& message) {
        if (why)
            *why = message;
        return false;
    };
    if (!data.fps.isValid())
        return fail(QStringLiteral("invalid frame rate"));
    if (data.width <= 0 || data.height <= 0)
        return fail(QStringLiteral("invalid resolution"));

    QSet<quint64> ids;
    auto claim = [&](quint64 id, const QString& what) -> bool {
        if (id == 0)
            return fail(what + QStringLiteral(" has id 0"));
        if (id >= data.nextId)
            return fail(what + QStringLiteral(" id %1 >= nextId %2").arg(id).arg(data.nextId));
        if (ids.contains(id))
            return fail(what + QStringLiteral(" id %1 is not unique").arg(id));
        ids.insert(id);
        return true;
    };

    for (TrackType type : kTrackTypes) {
        const auto& list = data.tracks(type);
        for (size_t t = 0; t < list.size(); ++t) {
            const TrackData& track = list[t];
            const QString trackName = trackDisplayName(track, int(t));
            if (track.type != type)
                return fail(trackName + QStringLiteral(" is in the wrong track list"));
            if (!claim(track.id, QStringLiteral("track ") + trackName))
                return false;
            for (size_t i = 0; i < track.clips.size(); ++i) {
                const ClipData& c = track.clips[i];
                const QString clipName = QStringLiteral("clip %1 on %2").arg(c.id).arg(trackName);
                if (!claim(c.id, clipName))
                    return false;
                if (c.duration < 1)
                    return fail(clipName + QStringLiteral(" has duration < 1"));
                if (c.start < 0)
                    return fail(clipName + QStringLiteral(" starts before 0"));
                if (!std::isfinite(c.speed))
                    return fail(clipName + QStringLiteral(" has a non-finite speed"));
                if (c.sourceInUs < 0)
                    return fail(clipName + QStringLiteral(" has a negative source in point"));
                if (c.linkGroup >= data.nextId)
                    return fail(clipName + QStringLiteral(" has link group >= nextId"));
                if ((type == TrackType::Subtitle) != (c.kind == ClipKind::Subtitle))
                    return fail(clipName + QStringLiteral(" is on the wrong kind of track"));
                if (i + 1 < track.clips.size() && c.end() > track.clips[i + 1].start)
                    return fail(clipName + QStringLiteral(" overlaps or is out of order"));
                for (const EffectInstance& fx : c.effects) {
                    if (!claim(fx.id, clipName + QStringLiteral(" effect")))
                        return false;
                }
                for (const MaskData& mask : c.masks) {
                    if (!claim(mask.id, clipName + QStringLiteral(" mask")))
                        return false;
                }
            }
        }
    }
    return true;
}

void clampFades(ClipData& clip, bool headCut)
{
    const int64_t room = std::max<int64_t>(0, clip.duration);
    clip.fadeInFrames = std::max<int64_t>(0, clip.fadeInFrames);
    clip.fadeOutFrames = std::max<int64_t>(0, clip.fadeOutFrames);
    if (headCut) {
        clip.fadeOutFrames = std::min(clip.fadeOutFrames, room);
        clip.fadeInFrames = std::min(clip.fadeInFrames, room - clip.fadeOutFrames);
    } else {
        clip.fadeInFrames = std::min(clip.fadeInFrames, room);
        clip.fadeOutFrames = std::min(clip.fadeOutFrames, room - clip.fadeInFrames);
    }
    if (clip.fadeInFrames == 0)
        clip.fadeInCurve = 0.0;
    if (clip.fadeOutFrames == 0)
        clip.fadeOutCurve = 0.0;
    for (std::optional<TransitionData>* t : {&clip.transitionIn, &clip.transitionOut}) {
        if (*t)
            (*t)->duration = std::clamp<int64_t>((*t)->duration, 1, std::max<int64_t>(1, 2 * room));
    }
}

void normalizeIds(TimelineData& data)
{
    quint64 maxId = 0;
    forEachTrack(data, [&](TrackRef, const TrackData& track) {
        maxId = std::max(maxId, track.id);
        for (const ClipData& c : track.clips) {
            maxId = std::max({maxId, c.id, c.linkGroup});
            for (const EffectInstance& fx : c.effects)
                maxId = std::max(maxId, fx.id);
            for (const MaskData& mask : c.masks)
                maxId = std::max(maxId, mask.id);
        }
    });
    data.nextId = std::max(data.nextId, maxId + 1);
}

} // namespace kk::editops
