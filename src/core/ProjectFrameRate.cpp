// SPDX-License-Identifier: GPL-3.0-or-later
// Project, part: changing the frame rate of timelines that have content (FrameRateConvert.h).
#include "core/CompoundOps.h"
#include "core/FrameRateConvert.h"
#include "core/Project.h"

#include <algorithm>

namespace kk {

namespace {

bool hasClips(const TimelineData& data)
{
    for (TrackType type : {TrackType::Video, TrackType::Audio, TrackType::Subtitle}) {
        for (const TrackData& track : data.tracks(type)) {
            if (!track.clips.empty())
                return true;
        }
    }
    return false;
}

} // namespace

void Project::convertOneTimeline(quint64 uid, Rational fps, bool followProject)
{
    const auto source = timelineData(uid);
    if (!source || source->fps == fps)
        return;
    const Rational oldFps = source->fps;
    auto copy = std::make_shared<TimelineData>(*source);
    kk::convertTimelineFrameRate(*copy, fps);
    copy->useProjectSettings = followProject;
    replaceTimeline(uid, std::move(copy));
    // Playhead and zoom stay at the same moment of time (not undoable, like moving them).
    if (uid == m_activeUid) {
        m_timeline->setPlayhead(convertFrames(m_timeline->playhead(), oldFps, fps));
    } else if (TimelineSlot* s = slot(uid)) {
        s->playhead = convertFrames(s->playhead, oldFps, fps);
    }
    if (TimelineSlot* s = slot(uid)) {
        if (s->view.pixelsPerFrame > 0.0)
            s->view.pixelsPerFrame *= double(oldFps.num) * fps.den / (double(oldFps.den) * fps.num);
    }
}

bool Project::convertTimelineFrameRate(quint64 uid, Rational fps, bool includeCompounds)
{
    const auto source = timelineData(uid);
    if (!source || !fps.isValid())
        return false;
    if (source->fps == fps)
        return true;
    const Rational oldFps = source->fps;

    // The project goes along when this timeline follows it and nothing else that follows has
    // content that would have to be converted by hand.
    bool moveProject = source->useProjectSettings && fps != m_format.fps;
    if (moveProject) {
        for (const quint64 other : timelineUids()) {
            const auto data = timelineData(other);
            if (other != uid && data && data->useProjectSettings && hasClips(*data)
                && !data->compound) {
                moveProject = false;
                break;
            }
        }
    }

    QVector<quint64> targets {uid};
    if (includeCompounds) {
        for (const quint64 inner : compound::reachableTimelines(uid, timelineLookup())) {
            const auto data = timelineData(inner);
            if (data && data->compound && data->fps == oldFps && !targets.contains(inner))
                targets.append(inner);
        }
    }
    if (moveProject) {
        for (const quint64 other : timelineUids()) {
            const auto data = timelineData(other);
            if (data && data->useProjectSettings && !targets.contains(other))
                targets.append(other);
        }
    }

    beginEdit(tr("Change Frame Rate"));
    const Rational projectFps = moveProject ? fps : m_format.fps;
    if (moveProject) {
        m_format.timelineStart
            = std::max<int64_t>(0, convertTimecode(m_format.timelineStart, m_format.fps, fps));
        m_format.fps = fps;
    }
    for (const quint64 target : targets) {
        const auto data = timelineData(target);
        const bool follows = data && data->useProjectSettings && fps == projectFps;
        convertOneTimeline(target, fps, follows);
    }
    endEdit();
    return true;
}

bool Project::convertProjectFrameRate(Rational fps)
{
    if (!fps.isValid())
        return false;
    if (fps == m_format.fps)
        return true;
    beginEdit(tr("Change Frame Rate"));
    m_format.timelineStart
        = std::max<int64_t>(0, convertTimecode(m_format.timelineStart, m_format.fps, fps));
    m_format.fps = fps;
    for (const quint64 uid : timelineUids()) {
        const auto data = timelineData(uid);
        if (data && data->useProjectSettings)
            convertOneTimeline(uid, fps, true);
    }
    endEdit();
    return true;
}

} // namespace kk
