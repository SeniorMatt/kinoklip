// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController, part: edits from the viewer's edit overlay (spec 4.14) and typed
// timecodes (spec 2.10).
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/TimecodeEntry.h"
#include "ui/timeline/TimelineController.h"

namespace kk {

std::optional<SourceEditMode> TimelineController::sourceEditModeFor(const QString& actionId)
{
    static const std::pair<const char*, SourceEditMode> kModes[] = {
        {"edit.insert", SourceEditMode::Insert},
        {"edit.overwrite", SourceEditMode::Overwrite},
        {"edit.replace", SourceEditMode::Replace},
        {"edit.fit_to_fill", SourceEditMode::FitToFill},
        {"edit.place_on_top", SourceEditMode::PlaceOnTop},
        {"edit.append_at_end", SourceEditMode::AppendAtEnd},
        {"edit.ripple_overwrite", SourceEditMode::RippleOverwrite},
    };
    for (const auto& [id, mode] : kModes)
        if (actionId == QLatin1String(id))
            return mode;
    return std::nullopt;
}

QVector<quint64> TimelineController::editSources(
    const QVector<SourceSelection>& sources, SourceEditMode mode)
{
    if (sources.isEmpty())
        return {};
    // These edits act on one range of the timeline: only the first source makes sense.
    const bool single = mode == SourceEditMode::Replace || mode == SourceEditMode::FitToFill
        || mode == SourceEditMode::RippleOverwrite;
    if (single || sources.size() == 1) {
        const SourceSelection& s = sources.front();
        return insertSource(s.mediaId, s.inUs, s.outUs, mode);
    }
    // Edits nest: the outer pair makes the whole drop one undo step.
    Project* p = project();
    p->beginEdit(tr("Edit Clips"));
    QVector<quint64> created;
    for (const SourceSelection& s : sources)
        created += insertSource(s.mediaId, s.inUs, s.outUs, mode);
    p->endEdit();
    return created;
}

void TimelineController::enterTimecode(const TypedTimecode& entry)
{
    if (!entry.relative) {
        setPlayheadClamped(entry.frames);
        return;
    }
    if (entry.frames == 0)
        return;
    // An offset with something selected moves it, like the nudge keys (one undo step).
    if (!m_selectedEdit.isEmpty() || !effectiveSelection().isEmpty()) {
        nudge(entry.frames);
        return;
    }
    setPlayheadClamped(timeline()->playhead() + entry.frames);
}

} // namespace kk
