// SPDX-License-Identifier: GPL-3.0-or-later
// EditOps: ripple delete, in/out range lift / extract / copy, delete gaps, join, swap, shuffle
// and ripple overwrite (declared in EditOps.h).
#include "core/EditOps.h"
#include "core/EditOpsInternal.h"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace kk::editops {

using namespace detail;

namespace {

using Range = std::pair<int64_t, int64_t>;

/// Sorts @p ranges and merges overlapping or touching ones.
std::vector<Range> mergeRanges(std::vector<Range> ranges)
{
    std::sort(ranges.begin(), ranges.end());
    std::vector<Range> merged;
    for (const Range& r : ranges) {
        if (!merged.empty() && r.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, r.second);
        else
            merged.push_back(r);
    }
    return merged;
}

/// Moves every clip of @p track starting at or after @p from by @p delta frames.
void shiftFrom(TrackData& track, int64_t from, int64_t delta)
{
    for (ClipData& c : track.clips) {
        if (c.start >= from)
            c.start += delta;
    }
}

/// The filter itself, or "every unlocked track" when it is empty; locked tracks never pass.
TrackFilter orUnlocked(const TrackFilter& filter)
{
    if (!filter)
        return unlockedTracks();
    return [filter](TrackRef ref, const TrackData& t) { return !t.locked && filter(ref, t); };
}

/// Adds the keys of @p src (clip-local frames of the right clip) to @p dst, @p offset frames
/// later, where @p dst has no key on that frame yet. Used when joining a through edit.
void mergeKeys(ParamSet& dst, const ParamSet& src, int64_t offset)
{
    for (auto it = src.values.cbegin(); it != src.values.cend(); ++it) {
        if (!it.value().isAnimated())
            continue;
        AnimatedValue& target = dst.animated(it.key(), it.value().base);
        for (const Keyframe& k : it.value().keys) {
            if (k.frame >= 0 && !target.hasKeyAt(k.frame + offset)) {
                Keyframe moved = k; // handles and hold travel with the key
                moved.frame += offset;
                target.putKey(moved);
            }
        }
    }
}

bool sameEffectChain(const ClipData& a, const ClipData& b)
{
    if (a.effects.size() != b.effects.size())
        return false;
    for (qsizetype i = 0; i < a.effects.size(); ++i) {
        if (a.effects[i].type != b.effects[i].type || a.effects[i].enabled != b.effects[i].enabled)
            return false;
    }
    return true;
}

} // namespace

// ---- Ripple delete ----

int rippleDeleteClips(TimelineData& data, const QVector<quint64>& clipIds, RippleScope scope)
{
    const QSet<quint64> ids(clipIds.cbegin(), clipIds.cend());
    // Time ranges removed on unlocked tracks, and which tracks lose clips.
    std::vector<Range> removedRanges;
    std::vector<char> hasDeletion[kTrackTypeCount] = {std::vector<char>(data.videoTracks.size(), 0),
        std::vector<char>(data.audioTracks.size(), 0),
        std::vector<char>(data.subtitleTracks.size(), 0)};
    forEachTrack(std::as_const(data), [&](TrackRef ref, const TrackData& track) {
        if (track.locked)
            return;
        for (const ClipData& c : track.clips) {
            if (ids.contains(c.id)) {
                removedRanges.emplace_back(c.start, c.end());
                hasDeletion[trackTypeIndex(ref.type)][size_t(ref.index)] = 1;
            }
        }
    });
    if (removedRanges.empty())
        return 0;
    const std::vector<Range> merged = mergeRanges(std::move(removedRanges));

    int removed = 0;
    forEachTrack(data, [&](TrackRef ref, TrackData& track) {
        if (track.locked)
            return;
        if (hasDeletion[trackTypeIndex(ref.type)][size_t(ref.index)]) {
            // Clips are sorted and disjoint, so every later clip moves left by the total
            // duration of the removed clips before it.
            std::vector<ClipData> kept;
            kept.reserve(track.clips.size());
            int64_t shift = 0;
            for (ClipData& clip : track.clips) {
                if (ids.contains(clip.id)) {
                    shift += clip.duration;
                    ++removed;
                    continue;
                }
                clip.start -= shift;
                kept.push_back(std::move(clip));
            }
            track.clips = std::move(kept);
            return;
        }
        if (scope != RippleScope::AllTracks || !track.autoSelect)
            return;
        // Another auto-select track follows every removed range it has room for. Right to left,
        // so the earlier ranges keep their coordinates.
        for (auto it = merged.crbegin(); it != merged.crend(); ++it) {
            if (rangeFree(track, it->first, it->second))
                shiftFrom(track, it->second, -(it->second - it->first));
        }
    });
    return removed;
}

// ---- In / out ranges ----

bool liftRange(TimelineData& data, int64_t in, int64_t out, const TrackFilter& filter)
{
    if (in < 0 || out <= in)
        return false;
    const TrackFilter accept = orUnlocked(filter);
    bool changed = false;
    GroupRemap remap;
    forEachTrack(data, [&](TrackRef ref, TrackData& track) {
        if (!accept(ref, track) || rangeFree(track, in, out))
            return;
        clearRangeImpl(data, track, in, out, remap);
        changed = true;
    });
    return changed;
}

bool extractRange(TimelineData& data, int64_t in, int64_t out, const TrackFilter& filter)
{
    if (in < 0 || out <= in)
        return false;
    const TrackFilter accept = orUnlocked(filter);
    bool changed = false;
    GroupRemap remap;
    forEachTrack(data, [&](TrackRef ref, TrackData& track) {
        if (!accept(ref, track))
            return;
        if (!rangeFree(track, in, out)) {
            clearRangeImpl(data, track, in, out, remap);
            changed = true;
        }
        if (track.end() > out) {
            shiftFrom(track, out, -(out - in));
            changed = true;
        }
    });
    return changed;
}

Clipboard copyRange(const TimelineData& data, int64_t in, int64_t out, const TrackFilter& filter)
{
    if (in < 0 || out <= in)
        return {};
    // Cut a scratch copy at the range edges, then copy the pieces inside.
    TimelineData scratch = data;
    const TrackFilter accept
        = filter ? filter : TrackFilter([](TrackRef, const TrackData&) { return true; });
    QVector<quint64> inside;
    GroupRemap remap;
    forEachTrack(scratch, [&](TrackRef ref, TrackData& track) {
        if (!accept(ref, track))
            return;
        for (const int64_t edge : {in, out}) {
            for (size_t i = 0; i < track.clips.size(); ++i) {
                if (track.clips[i].start < edge && track.clips[i].end() > edge) {
                    splitClipAtIndex(scratch, track, i, edge, remap);
                    break;
                }
            }
        }
        for (const ClipData& c : track.clips) {
            if (c.start >= in && c.end() <= out)
                inside.append(c.id);
        }
    });
    Clipboard board = copyClips(scratch, inside);
    if (board.isEmpty())
        return board;
    // copyClips() puts the origin at the earliest copied clip; the range origin is the in mark,
    // so empty room at the start of the range is kept.
    int64_t firstAbsolute = out;
    for (quint64 id : std::as_const(inside)) {
        if (const ClipData* c = clipById(scratch, id))
            firstAbsolute = std::min(firstAbsolute, c->start);
    }
    for (ClipboardClip& c : board.clips)
        c.clip.start += firstAbsolute - in;
    board.duration = out - in;
    return board;
}

int64_t deleteGaps(TimelineData& data, const TrackFilter& filter)
{
    const TrackFilter accept = orUnlocked(filter ? filter : autoSelectTracks());
    // Union of the occupied ranges on the accepted tracks; its holes are the common gaps.
    std::vector<Range> occupied;
    forEachTrack(std::as_const(data), [&](TrackRef ref, const TrackData& track) {
        if (!accept(ref, track))
            return;
        for (const ClipData& c : track.clips)
            occupied.emplace_back(c.start, c.end());
    });
    if (occupied.empty())
        return 0;
    const std::vector<Range> merged = mergeRanges(std::move(occupied));
    std::vector<Range> gaps;
    int64_t cursor = 0;
    for (const Range& r : merged) {
        if (r.first > cursor)
            gaps.emplace_back(cursor, r.first);
        cursor = r.second;
    }
    int64_t total = 0;
    for (auto it = gaps.crbegin(); it != gaps.crend(); ++it) {
        const int64_t len = it->second - it->first;
        forEachTrack(data, [&](TrackRef ref, TrackData& track) {
            if (accept(ref, track))
                shiftFrom(track, it->second, -len);
        });
        total += len;
    }
    return total;
}

// ---- Joining, swapping, shuffling ----

bool canJoin(const TimelineData& data, quint64 leftClipId, quint64 rightClipId)
{
    const auto left = findClip(data, leftClipId);
    const auto right = findClip(data, rightClipId);
    if (!left || !right || !(left->track == right->track)
        || right->clipIndex != left->clipIndex + 1)
        return false;
    const TrackData& track = *data.track(left->track);
    if (track.locked)
        return false;
    const ClipData& a = track.clips[size_t(left->clipIndex)];
    const ClipData& b = track.clips[size_t(right->clipIndex)];
    if (a.end() != b.start || a.kind != b.kind || a.mediaId != b.mediaId || a.speed != b.speed
        || a.enabled != b.enabled || a.transitionOut || b.transitionIn || !sameEffectChain(a, b)
        || a.retimeProcess != b.retimeProcess || a.pitchCorrection != b.pitchCorrection)
        return false;
    // Retime curves: the right clip's must be the left one's continued (a split shifts it).
    {
        ParamSet shifted = a.retime;
        shifted.shiftKeys(-a.duration);
        if (shifted != b.retime)
            return false;
    }
    // Grades likewise (a split keeps the grade on both halves, keys shifted).
    {
        ClipGrade shifted = a.grade;
        shifted.shiftKeys(-a.duration);
        if (shifted != b.grade)
            return false;
    }
    switch (a.kind) {
    case ClipKind::Nested:
        if (a.nestedUid != b.nestedUid)
            return false;
        [[fallthrough]];
    case ClipKind::Media: {
        // The right clip must continue the source where the left one ends (half a frame of
        // rounding allowed: source times are microseconds).
        const int64_t expected = a.sourceTimeUsAt(a.end(), data.fps);
        const double tolerance = std::max(1.0,
            retime::frameUs(data.fps) * std::abs(retime::speedAt(a, double(a.duration))) / 2.0);
        return std::abs(double(b.sourceInUs - expected)) <= tolerance;
    }
    case ClipKind::Title:
    case ClipKind::Subtitle:
        return a.title.statics == b.title.statics;
    case ClipKind::Generator:
        return a.generator == b.generator && a.title == b.title;
    case ClipKind::Adjustment:
        return a.effects.size() == b.effects.size();
    case ClipKind::Solid:
        return a.solidColor == b.solidColor;
    }
    return false;
}

bool joinClips(TimelineData& data, quint64 leftClipId, quint64 rightClipId)
{
    if (!canJoin(data, leftClipId, rightClipId))
        return false;
    const auto loc = findClip(data, leftClipId);
    TrackData& track = *data.track(loc->track);
    const size_t i = size_t(loc->clipIndex);
    ClipData& a = track.clips[i];
    const ClipData& b = track.clips[i + 1];
    mergeKeys(a.transform, b.transform, a.duration);
    mergeKeys(a.audio, b.audio, a.duration);
    mergeKeys(a.title, b.title, a.duration);
    for (qsizetype e = 0; e < a.effects.size(); ++e)
        mergeKeys(a.effects[e].params, b.effects[e].params, a.duration);
    a.transitionOut = b.transitionOut;
    a.fadeOutFrames = b.fadeOutFrames;
    a.fadeOutCurve = b.fadeOutCurve;
    a.duration += b.duration;
    track.clips.erase(track.clips.begin() + std::ptrdiff_t(i) + 1);
    return true;
}

quint64 swapWithNeighbour(TimelineData& data, quint64 clipId, int direction)
{
    const auto loc = findClip(data, clipId);
    if (!loc || direction == 0)
        return 0;
    TrackData& track = *data.track(loc->track);
    if (track.locked)
        return 0;
    const size_t i = size_t(loc->clipIndex);
    if ((direction < 0 && i == 0) || (direction > 0 && i + 1 >= track.clips.size()))
        return 0;
    const size_t first = direction < 0 ? i - 1 : i;
    ClipData& l = track.clips[first];
    ClipData& r = track.clips[first + 1];
    const int64_t gap = r.start - l.end();
    const int64_t origin = l.start;
    const quint64 neighbour = direction < 0 ? l.id : r.id;
    r.start = origin;
    l.start = origin + r.duration + gap;
    for (ClipData* c : {&l, &r}) {
        c->transitionIn.reset();
        c->transitionOut.reset();
    }
    std::swap(track.clips[first], track.clips[first + 1]);
    return neighbour;
}

bool shuffleClip(TimelineData& data, quint64 clipId, int64_t frame)
{
    const auto loc = findClip(data, clipId);
    if (!loc)
        return false;
    TrackData& track = *data.track(loc->track);
    const size_t i = size_t(loc->clipIndex);
    frame = std::max<int64_t>(0, frame);
    if (track.locked || frame == track.clips[i].start)
        return false;
    ClipData clip = std::move(track.clips[i]);
    track.clips.erase(track.clips.begin() + std::ptrdiff_t(i));
    // Close the hole the clip leaves, then open one of its length at the destination.
    shiftFrom(track, clip.start + clip.duration, -clip.duration);
    GroupRemap remap;
    for (size_t k = 0; k < track.clips.size(); ++k) {
        if (track.clips[k].start < frame && track.clips[k].end() > frame) {
            splitClipAtIndex(data, track, k, frame, remap);
            break;
        }
    }
    shiftFrom(track, frame, clip.duration);
    clip.start = frame;
    clip.transitionIn.reset();
    clip.transitionOut.reset();
    insertSorted(track, std::move(clip));
    return true;
}

// ---- Ripple overwrite ----

QVector<quint64> rippleOverwriteClips(TimelineData& data, const QVector<PlacedClip>& clips,
    int64_t rangeStart, int64_t rangeEnd, RippleScope scope)
{
    if (clips.isEmpty() || rangeStart < 0 || rangeEnd < rangeStart)
        return {};
    int64_t newEnd = rangeStart;
    QVector<TrackRef> targets;
    for (const PlacedClip& p : clips) {
        const TrackData* t = data.track(p.track);
        if (!t || t->locked || p.clip.duration < 1 || !std::isfinite(p.clip.speed))
            return {};
        newEnd = std::max(newEnd, rangeStart + p.clip.duration);
        targets.append(p.track);
    }
    const int64_t delta = (newEnd - rangeStart) - (rangeEnd - rangeStart);
    GroupRemap remap;
    forEachTrack(data, [&](TrackRef ref, TrackData& track) {
        if (track.locked)
            return;
        const bool target = targets.contains(ref);
        if (!target && (scope == RippleScope::TargetTracks || !track.autoSelect))
            return;
        if (target) {
            clearRangeImpl(data, track, rangeStart, rangeEnd, remap);
        } else if (delta > 0) {
            // Like an insert: a clip straddling the end of the range is cut there.
            for (size_t k = 0; k < track.clips.size(); ++k) {
                if (track.clips[k].start < rangeEnd && track.clips[k].end() > rangeEnd) {
                    splitClipAtIndex(data, track, k, rangeEnd, remap);
                    break;
                }
            }
        } else if (delta < 0 && !rangeFree(track, rangeEnd + delta, rangeEnd)) {
            return; // no room to follow a shorter replacement
        }
        if (delta != 0)
            shiftFrom(track, rangeEnd, delta);
    });
    QVector<quint64> ids;
    for (PlacedClip p : clips) {
        p.clip.start = rangeStart;
        ensureId(data, p.clip);
        ids.append(p.clip.id);
        insertSorted(*data.track(p.track), std::move(p.clip));
    }
    return ids;
}

} // namespace kk::editops
