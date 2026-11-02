// SPDX-License-Identifier: GPL-3.0-or-later
// CutEdits implementation. Every edit flattens the source groups to PlacedClips at their final
// frames, applies one EditOps operation and links each group's video and audio again.
#include "core/CutEdits.h"

#include "core/ParamKeys.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kk::cutedits {

namespace ops = editops;

namespace {

/// The groups' clips moved by @p at frames; @p groupOf receives each clip's group index.
QVector<ops::PlacedClip> flatten(
    const SourceGroups& groups, int64_t at, QVector<int>* groupOf, bool videoOnly = false)
{
    QVector<ops::PlacedClip> out;
    for (int g = 0; g < groups.size(); ++g) {
        for (ops::PlacedClip p : groups[g]) {
            if (videoOnly && p.track.type != TrackType::Video)
                continue;
            p.clip.start += at;
            out.append(std::move(p));
            if (groupOf)
                groupOf->append(g);
        }
    }
    return out;
}

/// Links the clips of each group (video + audio of one segment stay in sync).
void linkGroups(TimelineData& data, const QVector<quint64>& ids, const QVector<int>& groupOf)
{
    if (ids.size() != groupOf.size())
        return;
    int first = 0;
    for (int i = 1; i <= ids.size(); ++i) {
        if (i == ids.size() || groupOf[i] != groupOf[first]) {
            if (i - first > 1)
                ops::linkClips(data, ids.mid(first, i - first));
            first = i;
        }
    }
}

/// The end of the placed clips (largest clip end).
int64_t endOf(const QVector<ops::PlacedClip>& clips)
{
    int64_t end = 0;
    for (const ops::PlacedClip& p : clips)
        end = std::max(end, p.clip.start + p.clip.duration);
    return end;
}

bool rangeFree(const TrackData& track, int64_t start, int64_t end)
{
    return std::none_of(track.clips.begin(), track.clips.end(),
        [&](const ClipData& c) { return c.start < end && c.end() > start; });
}

/// Moves every clip of type @p type onto one track: the first unlocked track at or above the
/// lowest destination whose range [start, end) is free on it (a new track on top if none).
void liftOntoFreeTrack(TimelineData& data, QVector<ops::PlacedClip>& clips, TrackType type)
{
    int lowest = std::numeric_limits<int>::max();
    for (const ops::PlacedClip& p : clips) {
        if (p.track.type == type)
            lowest = std::min(lowest, p.track.index);
    }
    if (lowest == std::numeric_limits<int>::max())
        return;
    int64_t start = std::numeric_limits<int64_t>::max();
    int64_t end = 0;
    for (const ops::PlacedClip& p : clips) {
        if (p.track.type != type)
            continue;
        start = std::min(start, p.clip.start);
        end = std::max(end, p.clip.start + p.clip.duration);
    }
    const auto& list = data.tracks(type);
    int index = std::max(0, lowest);
    while (index < int(list.size())
        && (list[size_t(index)].locked || !rangeFree(list[size_t(index)], start, end)))
        ++index;
    if (index >= int(list.size()))
        index = ops::addTrack(data, type);
    for (ops::PlacedClip& p : clips) {
        if (p.track.type == type)
            p.track.index = index;
    }
}

/// Places @p clips with overwriteClips() and links the groups.
Result overwriteAndLink(
    TimelineData& data, QVector<ops::PlacedClip> clips, const QVector<int>& groupOf, int64_t start)
{
    Result r;
    if (clips.isEmpty())
        return r;
    r.ids = ops::overwriteClips(data, clips);
    if (!r.ok())
        return r;
    linkGroups(data, r.ids, groupOf);
    r.start = start;
    r.end = endOf(clips);
    return r;
}

/// Place on top at @p at (optionally video only, optionally zoomed).
Result placeOnTopImpl(
    TimelineData& data, const SourceGroups& groups, int64_t at, bool videoOnly, double zoom)
{
    QVector<int> groupOf;
    QVector<ops::PlacedClip> clips = flatten(groups, std::max<int64_t>(0, at), &groupOf, videoOnly);
    if (clips.isEmpty())
        return {};
    // A destination may be "one above the top track": then a new track is needed anyway.
    for (const ops::PlacedClip& p : clips) {
        if (p.track.index < 0 || p.track.index > int(data.tracks(p.track.type).size())
            || p.clip.duration < 1)
            return {};
    }
    liftOntoFreeTrack(data, clips, TrackType::Video);
    liftOntoFreeTrack(data, clips, TrackType::Audio);
    if (zoom != 1.0) {
        for (ops::PlacedClip& p : clips) {
            if (p.track.type != TrackType::Video)
                continue;
            p.clip.transform.setBase(keys::ZoomX, zoom);
            p.clip.transform.setBase(keys::ZoomY, zoom);
        }
    }
    return overwriteAndLink(data, std::move(clips), groupOf, std::max<int64_t>(0, at));
}

} // namespace

int64_t span(const SourceGroups& groups)
{
    int64_t end = 0;
    for (const auto& g : groups) {
        for (const ops::PlacedClip& p : g)
            end = std::max(end, p.clip.start + p.clip.duration);
    }
    return end;
}

int64_t nearestEditPoint(const TimelineData& data, int64_t frame)
{
    int64_t best = 0;
    int64_t bestDistance = std::llabs(frame);
    auto consider = [&](int64_t point) {
        const int64_t d = std::llabs(point - frame);
        if (d < bestDistance || (d == bestDistance && point < best)) {
            best = point;
            bestDistance = d;
        }
    };
    consider(data.duration());
    for (const auto* tracks : {&data.videoTracks, &data.audioTracks}) {
        for (const TrackData& t : *tracks) {
            for (const ClipData& c : t.clips) {
                consider(c.start);
                consider(c.end());
            }
        }
    }
    return best;
}

Result smartInsert(TimelineData& data, const SourceGroups& groups, int64_t playhead)
{
    const int64_t at = nearestEditPoint(data, playhead);
    QVector<int> groupOf;
    const QVector<ops::PlacedClip> clips = flatten(groups, at, &groupOf);
    Result r;
    if (clips.isEmpty())
        return r;
    r.ids = ops::insertClips(data, clips, ops::RippleScope::AllTracks);
    if (!r.ok())
        return r;
    linkGroups(data, r.ids, groupOf);
    r.start = at;
    r.end = at + span(groups);
    return r;
}

Result append(TimelineData& data, const SourceGroups& groups)
{
    const int64_t at = data.duration();
    QVector<int> groupOf;
    return overwriteAndLink(data, flatten(groups, at, &groupOf), groupOf, at);
}

Result rippleOverwrite(TimelineData& data, const SourceGroups& groups, int64_t playhead)
{
    QVector<int> groupOf;
    QVector<ops::PlacedClip> clips = flatten(groups, 0, &groupOf);
    if (clips.isEmpty())
        return {};
    // The clip that is replaced: under the playhead on the destination video track, else the
    // first one found on the auto-select tracks.
    const ClipData* target = nullptr;
    for (const ops::PlacedClip& p : clips) {
        if (p.track.type == TrackType::Video) {
            if (const TrackData* t = data.track(p.track))
                target = t->clipAt(playhead);
            break;
        }
    }
    if (!target)
        target = ops::clipUnderFrame(data, playhead, ops::autoSelectTracks());
    if (!target)
        return {};
    const int64_t rangeStart = target->start;
    const int64_t rangeEnd = target->end();
    const int64_t length = span(groups);

    // rippleOverwriteClips() places clips at the range start only, so it is given one stand-in
    // per destination track as long as the whole material; the stand-ins are then lifted and
    // the real segments overwritten into the room they made (same undo step: the caller's).
    QVector<ops::PlacedClip> standIns;
    for (const ops::PlacedClip& p : clips) {
        const bool seen = std::any_of(standIns.begin(), standIns.end(),
            [&](const ops::PlacedClip& s) { return s.track == p.track; });
        if (seen)
            continue;
        ops::PlacedClip s = p;
        s.clip.id = 0;
        s.clip.linkGroup = 0;
        s.clip.duration = length;
        standIns.append(s);
    }
    const QVector<quint64> standInIds
        = ops::rippleOverwriteClips(data, standIns, rangeStart, rangeEnd);
    if (standInIds.isEmpty())
        return {};
    ops::liftClips(data, standInIds);
    for (ops::PlacedClip& p : clips)
        p.clip.start += rangeStart;
    return overwriteAndLink(data, std::move(clips), groupOf, rangeStart);
}

Result placeOnTop(TimelineData& data, const SourceGroups& groups, int64_t playhead)
{
    return placeOnTopImpl(data, groups, playhead, false, 1.0);
}

Result closeUp(TimelineData& data, const SourceGroups& groups, int64_t playhead, double zoom)
{
    if (!(zoom > 0.0) || !std::isfinite(zoom))
        return {};
    return placeOnTopImpl(data, groups, playhead, true, zoom);
}

Result sourceOverwrite(TimelineData& data, const SourceGroups& groups, int64_t start)
{
    if (start >= 0)
        return placeOnTopImpl(data, groups, start, true, 1.0);
    // The source begins before the timeline: offsets become absolute frames and the part
    // before frame 0 is cut off.
    SourceGroups cut;
    for (const auto& g : groups) {
        QVector<ops::PlacedClip> kept;
        for (ops::PlacedClip p : g) {
            const int64_t from = start + p.clip.start;
            if (from + p.clip.duration <= 0)
                continue;
            if (from < 0) {
                const int64_t drop = -from;
                p.clip.duration -= drop;
                if (p.clip.sourceDurationUs > 0)
                    p.clip.sourceInUs
                        += int64_t(std::llround(double(framesToUs(drop, data.fps)) * p.clip.speed));
            }
            p.clip.start = std::max<int64_t>(0, from);
            kept.append(std::move(p));
        }
        if (!kept.isEmpty())
            cut.append(kept);
    }
    return placeOnTopImpl(data, cut, 0, true, 1.0);
}

std::optional<int64_t> sourceOverwriteStart(const TimelineData& data, int64_t playhead,
    const QString& sourceMediaId, int64_t sourceInUs, int64_t sourceOutUs,
    const TimecodeLookup& startTimecodeUs)
{
    if (!startTimecodeUs)
        return std::nullopt;
    const ClipData* under = nullptr;
    for (const TrackData& t : data.videoTracks) {
        under = t.clipAt(playhead);
        if (under && under->kind == ClipKind::Media)
            break;
        under = nullptr;
    }
    if (!under)
        under = ops::clipUnderFrame(data, playhead);
    if (!under || under->kind != ClipKind::Media || under->mediaId.isEmpty())
        return std::nullopt;
    const std::optional<int64_t> timelineTc = startTimecodeUs(under->mediaId);
    const std::optional<int64_t> sourceTc = startTimecodeUs(sourceMediaId);
    if (!timelineTc || !sourceTc)
        return std::nullopt;
    // Absolute time of the frame under the playhead, then the same time in the source.
    const int64_t underUs = under->sourceInUs
        + int64_t(
            std::llround(double(framesToUs(playhead - under->start, data.fps)) * under->speed));
    const int64_t sourceUs = *timelineTc + underUs - *sourceTc;
    if (sourceUs < sourceInUs || (sourceOutUs > sourceInUs && sourceUs >= sourceOutUs))
        return std::nullopt;
    return playhead - usToFrames(sourceUs - sourceInUs, data.fps);
}

double fastReviewRate(int64_t clipFrames, Rational fps)
{
    if (!fps.isValid() || clipFrames <= 0)
        return 1.0;
    const double seconds = double(clipFrames) * fps.den / fps.num;
    return std::clamp(seconds / kFastReviewSecondsPerClip, 1.0, kFastReviewMaxRate);
}

} // namespace kk::cutedits
