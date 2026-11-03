// SPDX-License-Identifier: GPL-3.0-or-later
// CutPage layout and wiring.
#include "app/pages/CutPage.h"

#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/PanelSplitter.h"
#include "ui/cut/CutEditor.h"
#include "ui/cut/CutOverview.h"
#include "ui/cut/CutToolBar.h"
#include "ui/cut/CutViewer.h"
#include "ui/cut/FastReview.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineLayout.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QEvent>
#include <QHideEvent>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

namespace kk {

namespace {

// Default sizes for a 1920x1080 window (splitters scale them).
constexpr int kMediaPoolWidth = 560;
constexpr int kViewerWidth = 1360;
constexpr int kTopRowHeight = 560;
constexpr int kTimelinesHeight = 400;
constexpr int kOverviewHeight = 110;
constexpr int kLowerHeight = 290;
constexpr double kCompactAudio = 0.55; ///< audio row height factor of the lower timeline

QAction* registryAction(const AppContext& ctx, const char* id)
{
    return ctx.actions && ctx.actions->contains(QLatin1String(id))
        ? ctx.actions->action(QLatin1String(id))
        : nullptr;
}

} // namespace

CutPage::CutPage(AppContext& ctx, TimelineController* controller, QWidget* parent)
    : Page(parent)
    , m_ctx(ctx)
    , m_controller(controller)
{
    m_mediaPool = new MediaPoolPanel(ctx);
    m_mediaPool->setObjectName(QStringLiteral("CutMediaPool"));
    m_mediaPool->setStripViewAvailable(true);
    m_viewer = new CutViewer(ctx);
    m_toolBar = new CutToolBar(ctx);
    m_overview = new CutOverview(ctx);
    m_lower = new TimelineWidget(ctx, controller);
    m_lower->setObjectName(QStringLiteral("CutLowerTimeline"));
    TimelineViewOptions options;
    options.centredPlayhead = true;
    options.rippleEdgeTrims = true;
    options.audioHeightScale = kCompactAudio;
    options.keepsViewState = false; // the Edit page's view owns the per-timeline zoom
    options.doubleClickOpensSource = false;
    m_lower->setViewOptions(options);
    // Focus in the lower timeline makes the program viewer the transport target.
    m_lower->setProperty("kkProgramFocus", true);
    m_editor = new CutEditor(ctx, controller, this);
    m_review = new FastReview(ctx, this);

    m_topRow = new PanelSplitter(Qt::Horizontal);
    m_topRow->addWidget(m_mediaPool);
    m_topRow->addWidget(m_viewer);
    m_topRow->setStretchFactor(1, 1);
    m_topRow->setSizes({kMediaPoolWidth, kViewerWidth});

    m_timelines = new PanelSplitter(Qt::Vertical);
    m_timelines->addWidget(m_overview);
    m_timelines->addWidget(m_lower);
    m_timelines->setStretchFactor(1, 1);
    m_timelines->setSizes({kOverviewHeight, kLowerHeight});

    auto* bottom = new QWidget;
    auto* bottomLayout = new QVBoxLayout(bottom);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(0);
    bottomLayout->addWidget(m_toolBar);
    bottomLayout->addWidget(m_timelines, 1);

    m_main = new PanelSplitter(Qt::Vertical);
    m_main->addWidget(m_topRow);
    m_main->addWidget(bottom);
    m_main->setStretchFactor(0, 1);
    m_main->setSizes({kTopRowHeight, kTimelinesHeight});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_main);

    // ---- Dual timeline: the overview marks what the lower timeline shows ----
    connect(m_lower, &TimelineWidget::viewChanged, this, &CutPage::syncOverviewRange);
    // A click into either timeline shows the timeline in the viewer (Resolve).
    m_lower->viewport()->installEventFilter(this);
    m_overview->installEventFilter(this);

    // ---- Smart edits ----
    m_editor->setMaterialProvider([this]() -> QVector<SourceSelection> {
        QVector<SourceSelection> material = m_viewer->sourceMaterial();
        if (material.isEmpty() && m_viewer->mode() == CutViewer::Mode::Timeline) {
            // Timeline mode: the media pool selection, whole clips, in view order.
            const QStringList selected = m_mediaPool->selectedMediaIds();
            for (const QString& id : m_mediaPool->visibleMediaIds()) {
                if (selected.contains(id))
                    material.append(SourceSelection {id, 0, 0});
            }
        }
        return material;
    });
    m_editor->connectActions([this] { return isVisible(); });
    connect(m_editor, &CutEditor::editRefused, m_toolBar, &CutToolBar::showMessage);
    connect(m_editor, &CutEditor::edited, m_toolBar, [this] { m_toolBar->showMessage({}); });
    // F9 / Shift+F10 / F12 / Shift+F12 run the Cut page versions while this page is shown.
    m_controller->setSourceEditHandler(
        [this](SourceEditMode mode) { return isVisible() && m_editor->handleSourceEdit(mode); });
    // Dragging media onto the timeline viewer offers the smart edits (spec 35.26).
    ViewerPanel* program = m_viewer->timelineViewer();
    program->setEditTileActions({QStringLiteral("cut.smart_insert"), QStringLiteral("cut.append"),
        QStringLiteral("cut.ripple_overwrite"), QStringLiteral("cut.close_up"),
        QStringLiteral("cut.place_on_top"), QStringLiteral("cut.source_overwrite")});
    program->setEditOverlayEnabled(true);
    connect(program, &ViewerPanel::sourceEditDropped, this,
        [this](const QString& actionId, const QList<SourceRange>& ranges) {
            const std::optional<CutEdit> edit = CutEditor::editForAction(actionId);
            if (!edit)
                return;
            QVector<SourceSelection> material;
            for (const SourceRange& r : ranges)
                material.append({r.mediaId, r.inUs, r.outUs});
            m_editor->run(*edit, material);
        });
    connect(m_toolBar, &CutToolBar::zoomInRequested, m_lower, &TimelineWidget::zoomIn);
    connect(m_toolBar, &CutToolBar::zoomOutRequested, m_lower, &TimelineWidget::zoomOut);

    connectMediaPool();
    connectActions();
    setCutActionsEnabled(false);
}

void CutPage::connectMediaPool()
{
    // A click on a clip: Source Tape mode jumps the tape to it, otherwise the clip opens in
    // Source Clip mode. A double-click always opens the clip.
    connect(m_mediaPool, &MediaPoolPanel::currentMediaChanged, this, [this](const QString& id) {
        if (!isVisible() || id.isEmpty())
            return;
        const MediaItem* item = m_ctx.project->mediaPool()->item(id);
        if (!item || item->isTimeline() || item->isSubtitle())
            return;
        if (m_viewer->mode() == CutViewer::Mode::SourceTape) {
            m_viewer->showTapeClip(id);
            return;
        }
        m_viewer->clipViewer()->setSourceMedia(id);
        m_viewer->setMode(CutViewer::Mode::SourceClip);
    });
    connect(m_mediaPool, &MediaPoolPanel::openInSourceViewer, this, [this](const QString& id) {
        m_viewer->clipViewer()->setSourceMedia(id);
        m_viewer->setMode(CutViewer::Mode::SourceClip);
        m_viewer->clipViewer()->setFocus();
    });
    connect(m_mediaPool, &MediaPoolPanel::shownItemsChanged, this, &CutPage::updateTapeItems);
    connect(m_ctx.project->mediaPool(), &MediaPool::changed, this, &CutPage::updateTapeItems);
    connect(m_ctx.project, &Project::projectReset, this, &CutPage::updateTapeItems);
    updateTapeItems();
}

void CutPage::connectActions()
{
    // Q: between the last source mode and the timeline.
    if (QAction* q = registryAction(m_ctx, "view.toggle_source_timeline")) {
        connect(q, &QAction::triggered, this, [this] {
            if (!isVisible())
                return;
            m_viewer->setMode(m_viewer->mode() == CutViewer::Mode::Timeline
                    ? m_viewer->lastSourceMode()
                    : CutViewer::Mode::Timeline);
            m_viewer->currentViewer()->setFocus();
        });
    }
    if (QAction* review = registryAction(m_ctx, "cut.fast_review")) {
        connect(review, &QAction::triggered, this, [this](bool on) {
            if (!isVisible())
                return;
            if (on)
                m_viewer->setMode(CutViewer::Mode::Timeline);
            m_review->setActive(on);
        });
        connect(m_review, &FastReview::activeChanged, review, &QAction::setChecked);
    }
    // The registry zoom keys zoom the lower timeline here (the Edit page's view stays put).
    for (const auto& [id, zoomIn] :
        {std::pair {"timeline.zoom_in", true}, {"timeline.zoom_out", false}}) {
        if (QAction* a = registryAction(m_ctx, id)) {
            const bool in = zoomIn;
            connect(a, &QAction::triggered, this, [this, in] {
                if (!isVisible())
                    return;
                if (in)
                    m_lower->zoomIn();
                else
                    m_lower->zoomOut();
            });
        }
    }
}

void CutPage::updateTapeItems()
{
    const QString binId = m_mediaPool->currentBin();
    const Bin* bin = m_ctx.project->mediaPool()->bin(binId);
    m_viewer->setTapeItems(m_mediaPool->visibleMediaIds(), m_mediaPool->currentBinKey(),
        bin ? bin->name : tr("Master"));
}

void CutPage::syncOverviewRange()
{
    const auto [first, last] = m_lower->visibleFrameRange();
    m_overview->setDetailRange(first, last);
}

void CutPage::applyDefaultZoom()
{
    // The viewport's own width: the layout learns the new size only after resize events.
    const int width = m_lower->viewport()->width() - timelinemetrics::kHeaderWidth;
    if (m_zoomApplied || width <= 0)
        return;
    m_zoomApplied = true;
    const double fps = m_ctx.project->timeline()->data().fps.toDouble();
    const double frames = kLowerTimelineSeconds * (fps > 0 ? fps : 25.0);
    m_lower->setPixelsPerFrame(width / frames, timelinemetrics::kHeaderWidth);
    syncOverviewRange();
}

void CutPage::setCutActionsEnabled(bool enabled)
{
    if (!m_ctx.actions)
        return;
    for (const QString& id : m_ctx.actions->menuEntries(QStringLiteral("cut"))) {
        if (m_ctx.actions->contains(id))
            m_ctx.actions->action(id)->setEnabled(enabled);
    }
}

void CutPage::showEvent(QShowEvent* event)
{
    Page::showEvent(event);
    setCutActionsEnabled(true);
    applyDefaultZoom();
    m_lower->centreOnPlayhead();
    syncOverviewRange();
    updateTapeItems();
    m_viewer->setMode(m_viewer->mode()); // re-activates the shown viewer for the transport
}

void CutPage::hideEvent(QHideEvent* event)
{
    Page::hideEvent(event);
    if (event->spontaneous())
        return; // minimised: keep the state
    m_review->stop();
    setCutActionsEnabled(false);
}

bool CutPage::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == m_lower->viewport() || watched == m_overview)
        && event->type() == QEvent::MouseButtonPress
        && m_viewer->mode() != CutViewer::Mode::Timeline)
        m_viewer->setMode(CutViewer::Mode::Timeline);
    if (watched == m_lower->viewport() && event->type() == QEvent::Resize && !m_zoomApplied
        && isVisible())
        applyDefaultZoom();
    return Page::eventFilter(watched, event);
}

void CutPage::setPanelVisible(PanelId id, bool visible)
{
    if (id != PanelId::MediaPool || m_mediaPool->isHidden() == !visible)
        return;
    m_mediaPool->setVisible(visible);
    emit panelVisibilityChanged(id, visible);
}

bool CutPage::isPanelVisible(PanelId id) const
{
    return id == PanelId::MediaPool && !m_mediaPool->isHidden();
}

void CutPage::saveLayout(QSettings& settings) const
{
    settings.setValue(QStringLiteral("main"), m_main->saveState());
    settings.setValue(QStringLiteral("topRow"), m_topRow->saveState());
    settings.setValue(QStringLiteral("timelines"), m_timelines->saveState());
    settings.setValue(QStringLiteral("viewerMode"), int(m_viewer->mode()));
    settings.setValue(QStringLiteral("visible/mediaPool"), isPanelVisible(PanelId::MediaPool));
}

void CutPage::restoreLayout(const QSettings& settings)
{
    m_main->restoreState(settings.value(QStringLiteral("main")).toByteArray());
    m_topRow->restoreState(settings.value(QStringLiteral("topRow")).toByteArray());
    m_timelines->restoreState(settings.value(QStringLiteral("timelines")).toByteArray());
    const int mode
        = settings.value(QStringLiteral("viewerMode"), int(CutViewer::Mode::Timeline)).toInt();
    if (mode >= int(CutViewer::Mode::SourceClip) && mode <= int(CutViewer::Mode::Timeline))
        m_viewer->setMode(CutViewer::Mode(mode));
    if (settings.contains(QStringLiteral("visible/mediaPool")))
        setPanelVisible(
            PanelId::MediaPool, settings.value(QStringLiteral("visible/mediaPool")).toBool());
}

} // namespace kk
