// SPDX-License-Identifier: GPL-3.0-or-later
// The project defaults from before spec 25.12 (25 fps, timelines starting at 00:00:00:00), for
// the tests written against them: their frame counts and timecodes assume 25 fps.
#pragma once

#include "core/Project.h"
#include "core/Timeline.h"

#include <QUndoStack>

namespace kk::test {

/// Switches @p project (fresh, empty) to 25 fps with its timelines starting at 00:00:00:00;
/// no undo step remains.
inline void useClassicTimeline(Project& project)
{
    ProjectSettings s = project.settings();
    s.fps = Rational {25, 1};
    s.timelineStart = 0;
    project.setSettings(s);
    for (const quint64 uid : project.timelineUids())
        project.setTimelineStartTimecode(uid, 0);
    project.undoStack()->clear();
}

} // namespace kk::test
