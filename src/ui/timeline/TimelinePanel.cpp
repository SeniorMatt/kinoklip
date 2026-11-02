// SPDX-License-Identifier: GPL-3.0-or-later
// TimelinePanel: assembles tool bar, controller and view; wires the zoom actions.
#include "ui/timeline/TimelinePanel.h"

#include "core/Project.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QEvent>
#include <QSlider>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr int kZoomSliderSteps = 1000; ///< slider resolution over the whole zoom range
constexpr int kZoomSliderWidth = 140; ///< px, like Resolve's compact zoom slider
} // namespace

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
    m_toolBar->setIconSize(QSize(metrics::kTransportIconSize, metrics::kTransportIconSize));
    m_toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    // Resolve's timeline tool bar order: edit tools | source edit modes | snapping and
    // linking | flags and markers | (right) zoom. Data, not code: the groups are action ids.
    const char* const groups[][4] = {
        {"tool.select", "tool.trim", "tool.blade", nullptr},
        {"edit.insert", "edit.overwrite", "edit.place_on_top", "edit.replace"},
        {"timeline.snapping", "timeline.linked_selection", nullptr, nullptr},
        {"edit.split", "mark.add_marker", nullptr, nullptr},
    };
    for (const auto& group : groups) {
        if (!m_toolBar->actions().isEmpty())
            m_toolBar->addSeparator();
        for (const char* id : group) {
            if (id && ctx.actions && ctx.actions->contains(QLatin1String(id)))
                m_toolBar->addAction(ctx.actions->action(QLatin1String(id)));
        }
    }
    auto* spacer = new QWidget(m_toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolBar->addWidget(spacer);
    if (ctx.actions) {
        m_toolBar->addAction(ctx.actions->action(QStringLiteral("timeline.zoom_fit")));
        m_toolBar->addAction(ctx.actions->action(QStringLiteral("timeline.zoom_out")));
    }
    m_zoomSlider = new QSlider(Qt::Horizontal, m_toolBar);
    m_zoomSlider->setObjectName(QStringLiteral("TimelineZoomSlider"));
    m_zoomSlider->setRange(0, kZoomSliderSteps);
    m_zoomSlider->setFixedWidth(kZoomSliderWidth);
    m_zoomSlider->setFocusPolicy(Qt::NoFocus);
    m_zoomSlider->setToolTip(tr("Timeline zoom"));
    m_toolBar->addWidget(m_zoomSlider);
    if (ctx.actions)
        m_toolBar->addAction(ctx.actions->action(QStringLiteral("timeline.zoom_in")));
    layout->addWidget(m_toolBar);

    m_view = new TimelineWidget(ctx, m_controller, body);
    layout->addWidget(m_view, 1);
    setContentWidget(body);
    // Resolve has no title above its timeline: the name is in the timeline viewer's header
    // and the tool bar starts right under the viewers.
    setTitleBarVisible(false);

    // Zoom slider <-> view. The slider is logarithmic over the view's zoom limits. The view
    // has no zoom signal (its zoom logic belongs to the timeline module): every repaint of
    // the viewport re-syncs the slider, which is cheap (one division and a log).
    connect(m_zoomSlider, &QSlider::valueChanged, this, [this](int value) {
        if (m_syncingSlider)
            return;
        const double t = double(value) / kZoomSliderSteps;
        const double ppf = timelinemetrics::kMinPixelsPerFrame
            * std::pow(
                timelinemetrics::kMaxPixelsPerFrame / timelinemetrics::kMinPixelsPerFrame, t);
        // Resolve zooms around the playhead.
        const double anchor
            = m_view->layout().frameToX(double(m_ctx.project->timeline()->playhead()));
        m_view->setPixelsPerFrame(ppf, anchor);
    });
    m_view->viewport()->installEventFilter(this);
    syncZoomSlider();

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

void TimelinePanel::syncZoomSlider()
{
    const double range
        = std::log(timelinemetrics::kMaxPixelsPerFrame / timelinemetrics::kMinPixelsPerFrame);
    const double t
        = std::log(m_view->pixelsPerFrame() / timelinemetrics::kMinPixelsPerFrame) / range;
    const int value = int(std::lround(std::clamp(t, 0.0, 1.0) * kZoomSliderSteps));
    if (m_zoomSlider->value() == value)
        return;
    m_syncingSlider = true;
    m_zoomSlider->setValue(value);
    m_syncingSlider = false;
}

bool TimelinePanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Paint && watched == m_view->viewport())
        syncZoomSlider();
    return PanelFrame::eventFilter(watched, event);
}

} // namespace kk
