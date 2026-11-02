// SPDX-License-Identifier: GPL-3.0-or-later
// EditPage, part: editing aids that connect the timeline and the timeline viewer — typed
// timecodes (spec 2.10), the viewer's edit overlay (spec 4.14) and the two-up trim view
// (spec 8.15).
#include "app/pages/EditPage.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/TimecodeEntry.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QLineEdit>
#include <QTextEdit>

namespace kk {

void EditPage::setupEditingAids()
{
    TimelineWidget* view = m_timeline->timelineWidget();
    TimelineController* controller = m_timeline->controller();

    // ---- Typed timecode: with the timeline (or its viewer) focused ----
    const auto accepts = [this, view] {
        if (!isVisible())
            return false;
        QWidget* focus = QApplication::focusWidget();
        if (!focus || qobject_cast<QLineEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus)
            || qobject_cast<QTextEdit*>(focus))
            return false;
        // The number keys cut angles while the multicam viewer is shown.
        if (m_sourceViewer->multicamMode() && m_sourceViewer->isVisible())
            return false;
        return focus == view || view->isAncestorOf(focus) || focus == m_timelineViewer
            || m_timelineViewer->isAncestorOf(focus);
    };
    m_timecodeEntry = new TimecodeEntry(
        accepts, [this] { return m_ctx.project->timeline()->data().fps; }, this);
    connect(m_timecodeEntry, &TimecodeEntry::displayChanged, m_timelineViewer,
        &ViewerPanel::setTimecodeEntryText);
    connect(m_timecodeEntry, &TimecodeEntry::entered, controller,
        [controller](const TypedTimecode& entry) { controller->enterTimecode(entry); });

    // ---- Two-up trim view ----
    connect(
        view, &TimelineWidget::trimPreviewChanged, m_timelineViewer, &ViewerPanel::setTrimPreview);

    // ---- Edit overlay drops ----
    m_timelineViewer->setEditOverlayEnabled(true);
    connect(m_timelineViewer, &ViewerPanel::sourceEditDropped, controller,
        [controller](const QString& actionId, const QList<SourceRange>& ranges) {
            const std::optional<SourceEditMode> mode
                = TimelineController::sourceEditModeFor(actionId);
            if (!mode)
                return;
            QVector<SourceSelection> sources;
            for (const SourceRange& r : ranges)
                sources.append({r.mediaId, r.inUs, r.outUs});
            controller->editSources(sources, *mode);
        });
}

} // namespace kk
