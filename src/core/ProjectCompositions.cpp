// SPDX-License-Identifier: GPL-3.0-or-later
// Project: the Compositing page's one undoable entry point (Project::setClipComposition()).
#include "core/EditOps.h"
#include "core/Project.h"

namespace kk {

bool Project::setClipComposition(quint64 timelineUid, quint64 clipId,
    std::optional<Composition> composition, const QString& label)
{
    if (timelineUid == m_activeUid) {
        const ClipData* clip = editops::clipById(m_timeline->data(), clipId);
        if (!clip)
            return false;
        if (clip->composition == composition)
            return true; // nothing to do (and no empty undo step)
        beginEdit(label);
        const bool ok = editops::setClipComposition(
            m_timeline->mutableData(), clipId, std::move(composition));
        endEdit();
        return ok;
    }
    const auto source = timelineData(timelineUid);
    if (!source || !editops::clipById(*source, clipId))
        return false;
    auto copy = std::make_shared<TimelineData>(*source);
    if (!editops::setClipComposition(*copy, clipId, std::move(composition)))
        return false;
    beginEdit(label);
    replaceTimeline(timelineUid, std::move(copy));
    endEdit();
    return true;
}

std::optional<Composition> Project::clipComposition(quint64 timelineUid, quint64 clipId) const
{
    const auto data = timelineData(timelineUid);
    const ClipData* clip = data ? editops::clipById(*data, clipId) : nullptr;
    return clip ? clip->composition : std::nullopt;
}

} // namespace kk
