// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineEditUtil — private helper of the timeline controller files: run one undoable edit.
#pragma once

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"

#include <QString>

#include <algorithm>
#include <cmath>

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

/// Plays clip @p clipId at @p speed (1.0 = normal): the same source material, so the duration
/// scales by old / new speed, limited by the media; later clips on its track ripple.
/// Returns whether anything changed (false for a locked track or an unchanged speed).
inline bool applyClipSpeed(TimelineData& data, quint64 clipId, double speed)
{
    const ClipData* c = editops::clipById(data, clipId);
    const auto loc = editops::findClip(data, clipId);
    if (!c || !(speed > 0.0) || data.track(loc->track)->locked || c->speed == speed)
        return false;
    int64_t frames
        = std::max<int64_t>(1, int64_t(std::llround(double(c->duration) * c->speed / speed)));
    if ((c->kind == ClipKind::Media || c->kind == ClipKind::Nested) && c->sourceDurationUs > 0) {
        const double usPerFrame = 1e6 * data.fps.den / data.fps.num * speed;
        const auto available = int64_t(double(c->sourceDurationUs - c->sourceInUs) / usPerFrame);
        frames = std::clamp<int64_t>(available, 1, frames);
    }
    const int64_t delta = frames - c->duration;
    // Shrink at the old speed first / grow at the new one, so the clip never shows media past
    // its end.
    if (delta < 0)
        editops::trimTail(data, clipId, delta, true);
    editops::clipById(data, clipId)->speed = speed;
    if (delta > 0)
        editops::trimTail(data, clipId, delta, true);
    return true;
}

} // namespace kk::timelineedit
