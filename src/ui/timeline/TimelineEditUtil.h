// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineEditUtil — private helper of the timeline controller files: run one undoable edit.
#pragma once

#include "core/Project.h"
#include "core/Timeline.h"

#include <QString>

namespace kk::timelineedit {

/// Runs @p fn on the mutable timeline inside one undoable edit. @p fn returns whether it
/// changed anything; if not, the edit is cancelled so no empty undo step (or id allocation)
/// is left behind. Returns whether something changed.
template<typename Fn>
bool runEdit(Project* project, const QString& label, Fn&& fn)
{
    project->beginEdit(label);
    const bool changed = fn(project->timeline()->mutableData());
    if (changed)
        project->endEdit();
    else
        project->cancelEdit();
    return changed;
}

} // namespace kk::timelineedit
