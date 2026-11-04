// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers of the timeline value types.
#include "core/TimelineData.h"

#include "core/Retime.h"

#include <algorithm>
#include <cmath>

namespace kk {

int64_t ClipData::framesToSourceUs(int64_t frames, Rational fps) const
{
    if (!fps.isValid())
        return 0;
    return int64_t(std::llround(double(frames) * 1e6 * fps.den / fps.num * speed));
}

int64_t ClipData::sourceTimeUsAt(int64_t frame, Rational fps) const
{
    return retime::sourceTimeUs(*this, double(frame - start), fps);
}

const TimelineData* NestedTimelines::findClip(const ClipData& clip) const
{
    if (!map)
        return nullptr;
    if (clip.multicamAngle >= 0) {
        const auto it = map->constFind(multicamViewKey(clip.nestedUid, clip.multicamAngle));
        if (it != map->constEnd())
            return it->get();
    }
    return find(clip.nestedUid);
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
    for (const TrackData& t : subtitleTracks)
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

int64_t nestedFrameAt(const ClipData& clip, int64_t frame, Rational parentFps, Rational nestedFps)
{
    // sourceTimeUsAt() is the PLAN 5.1 media formula (sourceIn + speed-scaled offset); the
    // nested timeline is the "media" here, measured in µs of its own time base.
    return usToFrames(clip.sourceTimeUsAt(frame, parentFps), nestedFps);
}

QString trackDisplayName(const TrackData& track, int index)
{
    if (!track.name.isEmpty())
        return track.name;
    const QString number = QString::number(index + 1);
    switch (track.type) {
    case TrackType::Video:
        return QLatin1Char('V') + number;
    case TrackType::Audio:
        return QLatin1Char('A') + number;
    case TrackType::Subtitle:
        break;
    }
    return QLatin1String("ST") + number;
}

int timecodeBase(Rational fps)
{
    return fps.isValid() ? std::max(1, int(std::lround(fps.toDouble()))) : 25;
}

int64_t oneHourTimecode(Rational fps)
{
    return int64_t(3600) * timecodeBase(fps);
}

int64_t convertTimecode(int64_t frames, Rational from, Rational to)
{
    const int a = timecodeBase(from);
    const int b = timecodeBase(to);
    if (a == b)
        return frames;
    const bool negative = frames < 0;
    const int64_t f = negative ? -frames : frames;
    const int64_t converted = (f / a) * b + std::min<int64_t>(f % a, b - 1);
    return negative ? -converted : converted;
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

} // namespace kk
