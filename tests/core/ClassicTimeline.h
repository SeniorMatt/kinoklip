// SPDX-License-Identifier: GPL-3.0-or-later
// The project defaults from before spec 25.12 (25 fps, timelines starting at 00:00:00:00) and
// 25.14 (V1, V2, A1, A2), for the tests written against them: their frame counts and timecodes
// assume 25 fps, their edits a second video and audio track.
#pragma once

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"

#include <QUndoStack>

namespace kk::test {

/// Switches @p project (fresh, empty) to 25 fps with its timelines starting at 00:00:00:00
/// and holding V1, V2, A1 and A2; no undo step remains.
inline void useClassicTimeline(Project& project)
{
    ProjectSettings s = project.settings();
    s.fps = Rational {25, 1};
    s.timelineStart = 0;
    project.setSettings(s);
    for (const quint64 uid : project.timelineUids())
        project.setTimelineStartTimecode(uid, 0);
    project.beginEdit(QStringLiteral("classic tracks"));
    TimelineData& d = project.timeline()->mutableData();
    while (d.videoTracks.size() < 2)
        editops::addTrack(d, TrackType::Video);
    while (d.audioTracks.size() < 2)
        editops::addTrack(d, TrackType::Audio);
    project.endEdit();
    project.undoStack()->clear();
}

} // namespace kk::test
