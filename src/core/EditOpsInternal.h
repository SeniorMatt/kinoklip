// SPDX-License-Identifier: GPL-3.0-or-later
// Private helpers shared by the EditOps implementation files (EditOps.cpp, EditOpsRipple.cpp).
// Not part of the public API: include only from src/core/EditOps*.cpp.
#pragma once

#include "core/EditOps.h"

#include <QHash>

#include <algorithm>
#include <limits>

namespace kk::editops::detail {

inline constexpr int64_t kUnbounded = std::numeric_limits<int64_t>::max() / 4;
inline constexpr TrackType kTrackTypes[] = {TrackType::Video, TrackType::Audio};

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
inline void shiftClipKeys(ClipData& clip, int64_t deltaFrames)
{
    clip.transform.shiftKeys(deltaFrames);
    clip.audio.shiftKeys(deltaFrames);
    clip.title.shiftKeys(deltaFrames);
    for (EffectInstance& fx : clip.effects)
        fx.params.shiftKeys(deltaFrames);
    for (MaskData& mask : clip.masks)
        mask.shiftKeys(deltaFrames);
    for (Marker& m : clip.markers) // clip markers follow the content like keyframes
        m.frame += deltaFrames;
}

// Moves the head of a clip by delta frames, keeping the content at every remaining timeline
// frame unchanged (source in point and keyframes follow).
inline void moveHead(ClipData& clip, int64_t delta, Rational fps)
{
    clip.start += delta;
    clip.duration -= delta;
    clip.sourceInUs = std::max<int64_t>(0, clip.sourceInUs + clip.framesToSourceUs(delta, fps));
    shiftClipKeys(clip, -delta);
}

inline void insertSorted(TrackData& track, ClipData clip)
{
    const auto it = std::lower_bound(track.clips.begin(), track.clips.end(), clip.start,
        [](const ClipData& c, int64_t start) { return c.start < start; });
    track.clips.insert(it, std::move(clip));
}

inline bool idInUse(const TimelineData& data, quint64 id)
{
    return id == 0 || findClip(data, id).has_value();
}

// Splits track.clips[index] at frame (start < frame < end). Returns the right-hand clip id.
inline quint64 splitClipAtIndex(
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
    for (MaskData& mask : right.masks) // mask ids are unique per timeline (KeyframeOps paths)
        mask.id = data.allocateId();
    moveHead(right, frame - right.start, data.fps);
    const quint64 rightId = right.id;
    track.clips.insert(track.clips.begin() + std::ptrdiff_t(index) + 1, std::move(right));
    return rightId;
}

inline void clearRangeImpl(
    TimelineData& data, TrackData& track, int64_t s, int64_t e, GroupRemap& remap)
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
inline void forEachTrack(TimelineData& data, Fn&& fn)
{
    for (TrackType type : kTrackTypes) {
        auto& list = data.tracks(type);
        for (size_t i = 0; i < list.size(); ++i)
            fn(TrackRef {type, int(i)}, list[i]);
    }
}

template<typename Fn>
inline void forEachTrack(const TimelineData& data, Fn&& fn)
{
    for (TrackType type : kTrackTypes) {
        const auto& list = data.tracks(type);
        for (size_t i = 0; i < list.size(); ++i)
            fn(TrackRef {type, int(i)}, list[i]);
    }
}

// Source-media duration of one timeline frame at the clip's speed, in µs.
inline double usPerFrame(const ClipData& clip, Rational fps)
{
    return fps.isValid() ? 1e6 * fps.den / fps.num * clip.speed : 0.0;
}

inline bool isBounded(const ClipData& clip)
{
    return (clip.kind == ClipKind::Media || clip.kind == ClipKind::Nested)
        && clip.sourceDurationUs > 0;
}

// True if no clip of @p track overlaps [s, e). Clips are sorted and disjoint, so their ends are
// sorted too: one binary search finds the first clip that ends after s.
inline bool rangeFree(const TrackData& track, int64_t s, int64_t e)
{
    const auto it = std::upper_bound(track.clips.cbegin(), track.clips.cend(), s,
        [](int64_t frame, const ClipData& c) { return frame < c.end(); });
    return it == track.clips.cend() || it->start >= e;
}

// Assigns a fresh id unless the clip already carries one that is free.
inline void ensureId(TimelineData& data, ClipData& clip)
{
    if (idInUse(data, clip.id))
        clip.id = data.allocateId();
}

} // namespace kk::editops::detail
