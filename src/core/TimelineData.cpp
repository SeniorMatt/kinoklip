// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers of the timeline value types.
#include "core/TimelineData.h"

#include <algorithm>
#include <cmath>

namespace cw {

int64_t ClipData::framesToSourceUs(int64_t frames, Rational fps) const
{
    if (!fps.isValid())
        return 0;
    return int64_t(std::llround(double(frames) * 1e6 * fps.den / fps.num * speed));
}

const ClipData* TrackData::clipAt(int64_t frame) const
{
    // First clip whose end is after frame; it contains frame if it starts at or before it.
    const auto it = std::upper_bound(clips.cbegin(), clips.cend(), frame,
        [](int64_t f, const ClipData& c) { return f < c.end(); });
    if (it != clips.cend() && it->start <= frame)
        return &*it;
    return nullptr;
}

int TrackData::indexOfClip(quint64 clipId) const
{
    for (size_t i = 0; i < clips.size(); ++i) {
        if (clips[i].id == clipId)
            return int(i);
    }
    return -1;
}

int64_t TimelineData::duration() const
{
    int64_t result = 0;
    for (const TrackData& t : videoTracks)
        result = std::max(result, t.end());
    for (const TrackData& t : audioTracks)
        result = std::max(result, t.end());
    return result;
}

TrackData* TimelineData::track(TrackRef ref)
{
    auto& list = tracks(ref.type);
    if (ref.index < 0 || ref.index >= int(list.size()))
        return nullptr;
    return &list[size_t(ref.index)];
}

const TrackData* TimelineData::track(TrackRef ref) const
{
    const auto& list = tracks(ref.type);
    if (ref.index < 0 || ref.index >= int(list.size()))
        return nullptr;
    return &list[size_t(ref.index)];
}

QString trackDisplayName(const TrackData& track, int index)
{
    if (!track.name.isEmpty())
        return track.name;
    const QChar prefix = track.type == TrackType::Video ? QLatin1Char('V') : QLatin1Char('A');
    return prefix + QString::number(index + 1);
}

TimelineData makeDefaultTimeline(const QString& name, Rational fps, int width, int height)
{
    TimelineData t;
    t.name = name;
    t.fps = fps;
    t.width = width;
    t.height = height;
    for (TrackType type :
        {TrackType::Video, TrackType::Video, TrackType::Audio, TrackType::Audio}) {
        TrackData track;
        track.id = t.allocateId();
        track.type = type;
        t.tracks(type).push_back(track);
    }
    return t;
}

} // namespace cw
