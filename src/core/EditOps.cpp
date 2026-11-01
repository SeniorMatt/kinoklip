// SPDX-License-Identifier: GPL-3.0-or-later
// Implementation of the pure timeline editing functions declared in EditOps.h.
#include "core/EditOps.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace cw::editops {

namespace {

constexpr int64_t kUnbounded = std::numeric_limits<int64_t>::max() / 4;
constexpr TrackType kTrackTypes[] = {TrackType::Video, TrackType::Audio};

// Hands out one new link group per old group during a single operation, so that the right-hand
// pieces of linked clips split together stay linked to each other.
class GroupRemap
{
public:
    quint64 map(TimelineData& data, quint64 oldGroup)
    {
        if (oldGroup == 0)
            return 0;
        auto it = m_groups.find(oldGroup);
        if (it == m_groups.end())
            it = m_groups.insert(oldGroup, data.allocateId());
        return *it;
    }

private:
    QHash<quint64, quint64> m_groups;
};

// Shifts the keyframes of every parameter set of a clip (they are clip-local, PLAN §5.1).
void shiftClipKeys(ClipData& clip, int64_t deltaFrames)
{
    clip.transform.shiftKeys(deltaFrames);
    clip.audio.shiftKeys(deltaFrames);
    clip.title.shiftKeys(deltaFrames);
    for (EffectInstance& fx : clip.effects)
        fx.params.shiftKeys(deltaFrames);
}

// Moves the head of a clip by delta frames, keeping the content at every remaining timeline
// frame unchanged (source in point and keyframes follow).
void moveHead(ClipData& clip, int64_t delta, Rational fps)
{
    clip.start += delta;
    clip.duration -= delta;
    clip.sourceInUs = std::max<int64_t>(0, clip.sourceInUs + clip.framesToSourceUs(delta, fps));
    shiftClipKeys(clip, -delta);
}

void insertSorted(TrackData& track, ClipData clip)
{
    const auto it = std::lower_bound(track.clips.begin(), track.clips.end(), clip.start,
        [](const ClipData& c, int64_t start) { return c.start < start; });
    track.clips.insert(it, std::move(clip));
}

bool idInUse(const TimelineData& data, quint64 id)
{
    return id == 0 || findClip(data, id).has_value();
}

// Splits track.clips[index] at frame (start < frame < end). Returns the right-hand clip id.
quint64 splitClipAtIndex(
    TimelineData& data, TrackData& track, size_t index, int64_t frame, GroupRemap& remap)
{
    ClipData right = track.clips[index];
    ClipData& left = track.clips[index];
    if (frame <= left.start || frame >= left.end())
        return 0;
    left.duration = frame - left.start;
    left.transitionOut.reset();

    right.id = data.allocateId();
    right.linkGroup = remap.map(data, right.linkGroup);
    right.transitionIn.reset();
    moveHead(right, frame - right.start, data.fps);
    const quint64 rightId = right.id;
    track.clips.insert(track.clips.begin() + std::ptrdiff_t(index) + 1, std::move(right));
    return rightId;
}

void clearRangeImpl(TimelineData& data, TrackData& track, int64_t s, int64_t e, GroupRemap& remap)
{
    if (s >= e || track.locked)
        return;
    std::vector<ClipData> out;
    out.reserve(track.clips.size() + 1);
    for (ClipData& clip : track.clips) {
        if (clip.end() <= s || clip.start >= e) {
            out.push_back(std::move(clip));
        } else if (clip.start >= s && clip.end() <= e) {
            continue; // fully covered: removed
        } else if (clip.start < s && clip.end() > e) {
            // Straddles the whole range: keep a left and a right piece.
            ClipData right = clip;
            right.id = data.allocateId();
            right.linkGroup = remap.map(data, right.linkGroup);
            right.transitionIn.reset();
            moveHead(right, e - right.start, data.fps);
            clip.duration = s - clip.start;
            clip.transitionOut.reset();
            out.push_back(std::move(clip));
            out.push_back(std::move(right));
        } else if (clip.start < s) {
            clip.duration = s - clip.start; // tail covered
            clip.transitionOut.reset();
            out.push_back(std::move(clip));
        } else {
            moveHead(clip, e - clip.start, data.fps); // head covered
            clip.transitionIn.reset();
            out.push_back(std::move(clip));
        }
    }
    track.clips = std::move(out);
}

// Visits every track as (ref, track).
template<typename Fn>
void forEachTrack(TimelineData& data, Fn&& fn)
{
    for (TrackType type : kTrackTypes) {
        auto& list = data.tracks(type);
        for (size_t i = 0; i < list.size(); ++i)
            fn(TrackRef {type, int(i)}, list[i]);
    }
}

template<typename Fn>
void forEachTrack(const TimelineData& data, Fn&& fn)
{
    for (TrackType type : kTrackTypes) {
        const auto& list = data.tracks(type);
        for (size_t i = 0; i < list.size(); ++i)
            fn(TrackRef {type, int(i)}, list[i]);
    }
}

// Source-media duration of one timeline frame at the clip's speed, in µs.
double usPerFrame(const ClipData& clip, Rational fps)
{
    return fps.isValid() ? 1e6 * fps.den / fps.num * clip.speed : 0.0;
}

bool isBounded(const ClipData& clip)
{
    return clip.kind == ClipKind::Media && clip.sourceDurationUs > 0;
}

} // namespace

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
    return track && !track->locked && placed.clip.duration >= 1 && placed.clip.speed > 0;
}

// Assigns a fresh id unless the clip already carries one that is free.
void ensureId(TimelineData& data, ClipData& clip)
{
    if (idInUse(data, clip.id))
        clip.id = data.allocateId();
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
        if (scope == RippleScope::TargetTracks && !targets.contains(ref))
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

int rippleDeleteClips(TimelineData& data, const QVector<quint64>& clipIds)
{
    const QSet<quint64> ids(clipIds.cbegin(), clipIds.cend());
    int removed = 0;
    forEachTrack(data, [&](TrackRef, TrackData& track) {
        if (track.locked)
            return;
        // Clips are sorted and disjoint, so every later clip moves left by the total duration
        // of the removed clips before it.
        std::vector<ClipData> kept;
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
    });
    return removed;
}

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
    const int64_t applied = std::clamp(
        deltaFrames, -maxHeadExtension(*clip, data.fps), maxTailExtension(*clip, data.fps));
    clip->sourceInUs
        = std::max<int64_t>(0, clip->sourceInUs + clip->framesToSourceUs(applied, data.fps));
    return applied;
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
std::vector<int64_t> editPoints(const TimelineData& data)
{
    std::vector<int64_t> frames;
    forEachTrack(data, [&](TrackRef, const TrackData& track) {
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

std::optional<int64_t> nextEditPoint(const TimelineData& data, int64_t frame)
{
    const std::vector<int64_t> points = editPoints(data);
    const auto it = std::upper_bound(points.cbegin(), points.cend(), frame);
    if (it == points.cend())
        return std::nullopt;
    return *it;
}

std::optional<int64_t> previousEditPoint(const TimelineData& data, int64_t frame)
{
    const std::vector<int64_t> points = editPoints(data);
    const auto it = std::lower_bound(points.cbegin(), points.cend(), frame);
    if (it == points.cbegin())
        return std::nullopt;
    return *(it - 1);
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
                if (c.speed <= 0)
                    return fail(clipName + QStringLiteral(" has speed <= 0"));
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
        }
    });
    data.nextId = std::max(data.nextId, maxId + 1);
}

} // namespace cw::editops
