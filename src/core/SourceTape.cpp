// SPDX-License-Identifier: GPL-3.0-or-later
// SourceTape implementation: building the tape, frame <-> source mapping, range segments.
#include "core/SourceTape.h"

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

/// Tape frames of a media of @p durationUs at @p fps, rounded up so the last partial frame
/// stays reachable (as the source viewer does, SourcePlayer.cpp).
int64_t framesCeil(int64_t durationUs, Rational fps)
{
    return int64_t(std::ceil(double(durationUs) * fps.num / (1e6 * fps.den) - 1e-6));
}

} // namespace

SourceTape SourceTape::build(
    const QVector<const MediaItem*>& items, Rational fps, int64_t stillFrames)
{
    SourceTape tape;
    tape.m_fps = fps.isValid() ? fps : Rational {25, 1};
    int64_t at = 0;
    for (const MediaItem* item : items) {
        if (!item || item->isTimeline() || item->isSubtitle())
            continue;
        const media::MediaInfo& info = item->info;
        if (!info.valid || (!info.hasVideo && !info.hasAudio))
            continue;
        SourceTapeEntry e;
        e.mediaId = item->id;
        e.name = item->name;
        e.start = at;
        e.hasVideo = info.hasVideo;
        e.hasAudio = info.hasAudio && !info.isStillImage;
        e.attributes = item->attributes;
        if (info.isStillImage || info.durationUs <= 0) {
            e.duration = std::max<int64_t>(1, stillFrames);
            e.sourceDurationUs = 0;
        } else {
            e.duration = std::max<int64_t>(1, framesCeil(info.durationUs, tape.m_fps));
            e.sourceDurationUs = info.durationUs;
        }
        at += e.duration;
        tape.m_entries.push_back(std::move(e));
    }
    return tape;
}

int64_t SourceTape::duration() const
{
    return m_entries.empty() ? 0 : m_entries.back().end();
}

int SourceTape::entryAt(int64_t frame) const
{
    if (frame < 0 || frame >= duration())
        return -1;
    // Entries are contiguous and sorted: the last one starting at or before the frame.
    const auto it = std::upper_bound(m_entries.begin(), m_entries.end(), frame,
        [](int64_t f, const SourceTapeEntry& e) { return f < e.start; });
    return int(it - m_entries.begin()) - 1;
}

int SourceTape::indexOf(const QString& mediaId) const
{
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].mediaId == mediaId)
            return int(i);
    }
    return -1;
}

SourceTapePosition SourceTape::locate(int64_t frame) const
{
    const int index = entryAt(frame);
    if (index < 0)
        return {};
    const SourceTapeEntry& e = m_entries[size_t(index)];
    const int64_t us = e.sourceDurationUs > 0 ? framesToUs(frame - e.start, m_fps) : 0;
    return {index, std::min(us, std::max<int64_t>(0, e.sourceDurationUs))};
}

int64_t SourceTape::frameOf(int entry, int64_t sourceUs) const
{
    if (entry < 0 || entry >= int(m_entries.size()))
        return -1;
    const SourceTapeEntry& e = m_entries[size_t(entry)];
    const int64_t local
        = e.sourceDurationUs > 0 ? usToFrames(std::max<int64_t>(0, sourceUs), m_fps) : 0;
    return e.start + std::clamp<int64_t>(local, 0, e.duration - 1);
}

std::vector<int64_t> SourceTape::boundaries() const
{
    std::vector<int64_t> out;
    out.reserve(m_entries.size() + 1);
    for (const SourceTapeEntry& e : m_entries)
        out.push_back(e.start);
    if (!m_entries.empty())
        out.push_back(duration());
    return out;
}

QVector<SourceTapeSegment> SourceTape::segments(int64_t in, int64_t out) const
{
    QVector<SourceTapeSegment> result;
    in = std::max<int64_t>(0, in);
    out = std::min(out, duration());
    if (out <= in)
        return result;
    for (int i = entryAt(in); i >= 0 && i < int(m_entries.size()); ++i) {
        const SourceTapeEntry& e = m_entries[size_t(i)];
        if (e.start >= out)
            break;
        const int64_t a = std::max(in, e.start) - e.start;
        const int64_t b = std::min(out, e.end()) - e.start;
        SourceTapeSegment s;
        s.mediaId = e.mediaId;
        if (e.sourceDurationUs > 0) {
            s.inUs = framesToUs(a, m_fps);
            // The last frame of a clip may be partial: never run past the media.
            s.outUs = b >= e.duration ? e.sourceDurationUs
                                      : std::min(framesToUs(b, m_fps), e.sourceDurationUs);
            if (s.outUs <= s.inUs)
                continue;
        }
        result.append(s);
    }
    return result;
}

TimelineData SourceTape::toTimeline(const QString& name, int width, int height) const
{
    TimelineData tl;
    tl.name = name;
    tl.fps = m_fps;
    tl.width = std::max(1, width);
    tl.height = std::max(1, height);
    tl.videoTracks.resize(1);
    tl.audioTracks.resize(1);
    tl.videoTracks[0].id = tl.allocateId();
    tl.audioTracks[0].id = tl.allocateId();
    quint64 group = 0;
    for (const SourceTapeEntry& e : m_entries) {
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = e.mediaId;
        clip.name = e.name;
        clip.start = e.start;
        clip.duration = e.duration;
        clip.sourceInUs = 0;
        clip.sourceDurationUs = e.sourceDurationUs;
        clip.linkGroup = ++group;
        clip.attributes = e.attributes;
        if (e.hasVideo) {
            clip.id = tl.allocateId();
            tl.videoTracks[0].clips.push_back(clip);
        }
        if (e.hasAudio) {
            clip.id = tl.allocateId();
            tl.audioTracks[0].clips.push_back(clip);
        }
    }
    return tl;
}

} // namespace kk
