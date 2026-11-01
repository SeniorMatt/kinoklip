// SPDX-License-Identifier: GPL-3.0-or-later
// TimelinePanel: assembles tool bar, controller and view; wires the zoom actions.
#include "ui/timeline/TimelinePanel.h"

#include "core/Project.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QToolBar>
#include <QVBoxLayout>

namespace kk {

TimelinePanel::TimelinePanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Timeline"), parent)
    , m_ctx(ctx)
{
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_controller = new TimelineController(ctx, this);
    m_controller->connectActions();

    m_toolBar = new QToolBar(body);
    m_toolBar->setIconSize(QSize(18, 18));
    m_toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    const char* const groups[][4] = {
        {"tool.select", "tool.trim", "tool.blade", nullptr},
        {"timeline.snapping", "timeline.linked_selection", nullptr, nullptr},
        {"edit.insert", "edit.overwrite", "edit.place_on_top", nullptr},
        {"edit.split", "mark.add_marker", nullptr, nullptr},
        {"timeline.zoom_out", "timeline.zoom_in", "timeline.zoom_fit", nullptr},
    };
    for (const auto& group : groups) {
        if (!m_toolBar->actions().isEmpty())
            m_toolBar->addSeparator();
        for (const char* id : group) {
            if (id && ctx.actions)
                m_toolBar->addAction(ctx.actions->action(QLatin1String(id)));
        }
    }
    layout->addWidget(m_toolBar);

    m_view = new TimelineWidget(ctx, m_controller, body);
    layout->addWidget(m_view, 1);
    setContentWidget(body);

    if (ctx.actions) {
        auto on = [this](const char* id, void (TimelineWidget::*slot)()) {
            connect(m_ctx.actions->action(QLatin1String(id)), &QAction::triggered, m_view, slot);
        };
        on("timeline.zoom_in", &TimelineWidget::zoomIn);
        on("timeline.zoom_out", &TimelineWidget::zoomOut);
        on("timeline.zoom_fit", &TimelineWidget::zoomToFit);
    }

    // Title = timeline name (the program viewer is already called "Timeline").
    Timeline* timeline = ctx.project->timeline();
    auto updateTitle = [this, timeline] {
        const QString name = timeline->data().name;
        setTitle(name.isEmpty() ? tr("Timeline") : name);
    };
    connect(timeline, &Timeline::changed, this, updateTitle);
    updateTitle();
}

} // namespace kk
