// SPDX-License-Identifier: GPL-3.0-or-later
// Implementation of the pure timeline editing functions declared in EditOps.h.
#include "core/EditOps.h"

#include "core/EditOpsInternal.h"

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
    int minIndex[2] = {std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
    int maxIndex[2] = {-1, -1};
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
        const int t = loc->track.type == TrackType::Video ? 0 : 1;
        minIndex[t] = std::min(minIndex[t], loc->track.index);
        maxIndex[t] = std::max(maxIndex[t], loc->track.index);
        minStart = std::min(minStart, data.track(loc->track)->clips[size_t(loc->clipIndex)].start);
    }
    if (moving.empty())
        return false;

    // Clamp as a group so the clips keep their relative positions.
    const int64_t dx = std::max(deltaFrames, -minStart);
    int dy[2] = {0, 0};
    for (int t = 0; t < 2; ++t) {
        if (maxIndex[t] < 0)
            continue;
        const int count = int(data.tracks(kTrackTypes[t]).size());
        dy[t] = std::clamp(deltaTrack, -minIndex[t], count - 1 - maxIndex[t]);
    }
    if (dx == 0 && dy[0] == 0 && dy[1] == 0)
        return false;
    for (const Moving& m : moving) {
        const int t = m.from.track.type == TrackType::Video ? 0 : 1;
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
        const int t = m.from.track.type == TrackType::Video ? 0 : 1;
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
    if (ripple) {
        for (size_t i = index + 1; i < track.clips.size(); ++i)
            track.clips[i].start += applied;
    }
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
    if (prevTouches)
        prev->duration += applied;
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

int64_t closeGap(TimelineData& data, TrackRef ref, int64_t frame, RippleScope scope)
{
    const auto gap = gapAt(data, ref, frame);
    const TrackData* target = data.track(ref);
    if (!gap || target->locked)
        return 0;
    const int64_t cut = gap->second; // clips starting here or later move left
    int64_t amount = gap->second - gap->first;
    if (scope == RippleScope::AllTracks) {
        // Every other track can only give up the empty room directly left of the cut; a clip
        // straddling the cut leaves no room at all.
        forEachTrack(data, [&](TrackRef r, const TrackData& track) {
            if (track.locked || !track.autoSelect || r == ref)
                return;
            int64_t lastEnd = 0;
            for (const ClipData& c : track.clips) {
                if (c.start >= cut)
                    break;
                lastEnd = std::max(lastEnd, c.end());
            }
            amount = std::min(amount, std::max<int64_t>(0, cut - lastEnd));
        });
    }
    if (amount <= 0)
        return 0;
    forEachTrack(data, [&](TrackRef r, TrackData& track) {
        if (track.locked
            || (!(r == ref) && (scope == RippleScope::TargetTracks || !track.autoSelect)))
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
    };
    std::vector<Found> found;
    QSet<quint64> seen;
    int64_t minStart = kUnbounded;
    int64_t maxEnd = 0;
    int minIndex[2] = {std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
    for (quint64 id : clipIds) {
        const auto loc = findClip(data, id);
        if (!loc || seen.contains(id))
            continue;
        seen.insert(id);
        const ClipData* c = &data.track(loc->track)->clips[size_t(loc->clipIndex)];
        found.push_back({loc->track, c});
        minStart = std::min(minStart, c->start);
        maxEnd = std::max(maxEnd, c->end());
        int& m = minIndex[loc->track.type == TrackType::Video ? 0 : 1];
        m = std::min(m, loc->track.index);
    }
    Clipboard board;
    if (found.empty())
        return board;
    std::sort(found.begin(), found.end(),
        [](const Found& a, const Found& b) { return a.clip->start < b.clip->start; });
    board.baseVideoTrack = minIndex[0] == std::numeric_limits<int>::max() ? 0 : minIndex[0];
    board.baseAudioTrack = minIndex[1] == std::numeric_limits<int>::max() ? 0 : minIndex[1];
    board.duration = maxEnd - minStart;
    for (const Found& f : found) {
        ClipboardClip entry;
        entry.type = f.ref.type;
        entry.trackOffset = f.ref.index
            - (f.ref.type == TrackType::Video ? board.baseVideoTrack : board.baseAudioTrack);
        entry.clip = *f.clip;
        entry.clip.start -= minStart;
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
    const int bases[2] = {videoBase >= 0 ? videoBase : clipboard.baseVideoTrack,
        audioBase >= 0 ? audioBase : clipboard.baseAudioTrack};
    // Refuse before changing anything if an existing destination track is locked.
    for (const ClipboardClip& entry : clipboard.clips) {
        const int index = bases[entry.type == TrackType::Video ? 0 : 1] + entry.trackOffset;
        const TrackData* track = data.track(TrackRef {entry.type, index});
        if (index < 0 || (track && track->locked))
            return {};
    }
    // Make sure every destination track exists (pasting high tracks into a smaller timeline).
    for (const ClipboardClip& entry : clipboard.clips) {
        const int index = bases[entry.type == TrackType::Video ? 0 : 1] + entry.trackOffset;
        while (int(data.tracks(entry.type).size()) <= index)
            addTrack(data, entry.type);
    }
    QVector<PlacedClip> placed;
    placed.reserve(clipboard.clips.size());
    GroupRemap remap; // fresh link groups, shared between clips that were linked together
    for (const ClipboardClip& entry : clipboard.clips) {
        PlacedClip p;
        p.track = TrackRef {
            entry.type, bases[entry.type == TrackType::Video ? 0 : 1] + entry.trackOffset};
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
    return insert ? insertClips(data, placed, RippleScope::AllTracks)
                  : overwriteClips(data, placed);
}

// ---- Tracks ----

int addTrack(TimelineData& data, TrackType type, int index)
{
    auto& list = data.tracks(type);
    if (index < 0 || index > int(list.size()))
        index = int(list.size());
    TrackData track;
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
    for (const Marker& m : data.markers)
        frames.push_back(m.frame);
    forEachTrack(data, [&](TrackRef, const TrackData& track) {
        for (const ClipData& c : track.clips) {
            if (excludeClipIds.contains(c.id))
                continue;
            frames.push_back(c.start);
            frames.push_back(c.end());
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
    for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
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
