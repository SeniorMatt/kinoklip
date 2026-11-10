// SPDX-License-Identifier: GPL-3.0-or-later
// CompViewer implementation: header, surface, guides, node drops and the picture source.
#include "ui/comp/CompViewer.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Metrics.h"
#include "ui/common/PanelInfo.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/CompViewerOverlay.h"
#include "ui/viewer/GuideOverlay.h"
#include "ui/viewer/ViewerSurface.h"

#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <future>
#include <memory>

namespace kk {

namespace {
/// Zoom choices of the header: label, surface zoom (0 = fit).
const std::pair<const char*, double> kZooms[] = {
    {QT_TRANSLATE_NOOP("kk::CompViewer", "Fit"), 0.0},
    {"50%", 0.5},
    {"100%", 1.0},
    {"200%", 2.0},
};
} // namespace

CompViewer::CompViewer(int index, AppContext& ctx, CompSession* session, QWidget* parent)
    : PanelFrame(QString(), parent)
    , m_ctx(ctx)
    , m_session(session)
    , m_index(index)
{
    setPanelInfo(this, index == 0 ? tr("Left Viewer") : tr("Right Viewer"),
        tr("Shows the output of one node of the open composition."));
    setObjectName(
        index == 0 ? QStringLiteral("CompViewerLeft") : QStringLiteral("CompViewerRight"));
    setTitleCentered(true);
    setAcceptDrops(true);

    m_zoom = new QComboBox;
    m_zoom->setObjectName(QStringLiteral("CompViewerZoom"));
    m_zoom->setMinimumWidth(metrics::kTitleComboMinWidth);
    for (const auto& [label, zoom] : kZooms)
        m_zoom->addItem(tr(label), zoom);
    m_zoom->setFocusPolicy(Qt::NoFocus);
    addTitleBarLeadingWidget(m_zoom);
    connect(m_zoom, &QComboBox::currentIndexChanged, this,
        [this] { setZoom(m_zoom->currentData().toDouble()); });
    m_frameLabel = new QLabel;
    m_frameLabel->setObjectName(QStringLiteral("CompViewerFrame"));
    addTitleBarWidget(m_frameLabel);

    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    m_surface = ViewerSurface::create(body);
    layout->addWidget(m_surface->widget());
    setContentWidget(body);
    const TimelineData& data = ctx.project->timeline()->data();
    m_surface->setAspect(QSize(data.width, data.height));
    m_guides = new GuideOverlay(ctx, m_surface, true);
    m_guides->setFrameSize(QSize(data.width, data.height));
    m_controls = new CompViewerOverlay(session, m_surface, m_index);

    connect(session, &CompSession::viewersChanged, this, &CompViewer::refresh);
    connect(session, &CompSession::clipChanged, this, &CompViewer::refresh);
    connect(session, &CompSession::compositionChanged, this, &CompViewer::refresh);
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this, &CompViewer::refresh);
    m_watcher = new QFutureWatcher<RenderedFramePtr>(this);
    connect(m_watcher, &QFutureWatcher<RenderedFramePtr>::finished, this, &CompViewer::onRendered);
    connect(ctx.project->timeline(), &Timeline::changed, this, [this] {
        const TimelineData& d = m_ctx.project->timeline()->data();
        m_surface->setAspect(QSize(d.width, d.height));
        m_guides->setFrameSize(QSize(d.width, d.height));
    });
    m_engine = ctx.playback;
    if (m_engine)
        connect(m_engine, &PlaybackEngine::frameReady, this, &CompViewer::showFrame);
    refresh();
}

CompViewer::~CompViewer()
{
    if (m_engine)
        m_engine->removeOutputClient(this);
}

quint64 CompViewer::node() const
{
    return m_session ? m_session->viewerNode(m_index) : 0;
}

QString CompViewer::headerText() const
{
    return title();
}

void CompViewer::setZoom(double zoom)
{
    m_surface->setZoom(zoom);
    for (int i = 0; i < m_zoom->count(); ++i) {
        if (qFuzzyCompare(m_zoom->itemData(i).toDouble() + 1, zoom + 1)) {
            const QSignalBlocker block(m_zoom);
            m_zoom->setCurrentIndex(i);
        }
    }
    m_controls->update();
    updateOutputClient();
}

void CompViewer::updateHeader()
{
    const QString side = m_index == 0 ? tr("Left") : tr("Right");
    const CompNode* n = m_session ? m_session->composition().node(node()) : nullptr;
    setTitle(n ? tr("%1 — %2").arg(side, n->name) : tr("%1 — No Node").arg(side));
    if (m_session && m_session->clipId()) {
        m_frameLabel->setText(tr("Frame %1").arg(m_session->compFrame()));
    } else {
        m_frameLabel->clear();
    }
    m_controls->refresh();
}

void CompViewer::refresh()
{
    updateHeader();
    if (!m_session || !m_session->clipId()) {
        m_surface->clear(tr("Select a clip on the Edit page"));
        return;
    }
    if (!node()) {
        m_surface->clear(
            m_index == 0 ? tr("Select a node and press 1") : tr("Select a node and press 2"));
        return;
    }
    if (rendersNodes()) {
        requestRender();
        return;
    }
    // No stored composition: the default MediaIn -> MediaOut shows exactly the clip's picture,
    // which the program engine already renders (no second render needed).
    if (m_engine && m_engine->currentFrame())
        m_surface->setRenderedFrame(m_engine->currentFrame());
    else
        m_surface->clear();
}

bool CompViewer::rendersNodes() const
{
    return m_engine && m_engine->compositor() && m_session && m_session->isStored();
}

void CompViewer::requestRender()
{
    if (!isVisible() || !rendersNodes() || !node())
        return;
    // One render in flight per viewer: requests while it runs (scrubbing, playback, slider
    // drags) collapse into one more render of the newest state when it finishes.
    if (m_inFlight) {
        m_renderAgain = true;
        return;
    }
    m_inFlight = true;
    m_renderAgain = false;
    const qreal dpr = m_surface->widget()->devicePixelRatioF();
    const QSize logical = m_surface->pictureRect().size();
    const QSize pixels(int(logical.width() * dpr), int(logical.height() * dpr));
    const quint64 nodeId = node() == m_session->composition().mediaOutId ? 0 : node();
    auto future = std::make_shared<std::future<RenderedFramePtr>>(m_engine->renderCompositionNode(
        m_ctx.project->timeline()->snapshot(), m_session->clipId(), nodeId, m_session->compFrame(),
        pixels, m_surface->canDrawTextures() ? OutputTexture : OutputImage));
    // Wait on a pool thread, deliver on the GUI thread (the compositor's futures are std::).
    m_watcher->setFuture(QtConcurrent::run([future] { return future->get(); }));
}

void CompViewer::onRendered()
{
    m_inFlight = false;
    const RenderedFramePtr frame = m_watcher->result();
    if (frame && frame->ok && rendersNodes() && node())
        m_surface->setRenderedFrame(frame);
    if (m_renderAgain)
        requestRender();
}

void CompViewer::showFrame(const RenderedFramePtr& frame)
{
    if (rendersNodes()) {
        requestRender(); // the program moved on (playback): render the node at the new frame
        return;
    }
    if (isVisible() && node() && frame)
        m_surface->setRenderedFrame(frame);
}

void CompViewer::updateOutputClient()
{
    if (!m_engine)
        return;
    if (!isVisible()) {
        m_engine->removeOutputClient(this);
        return;
    }
    const qreal dpr = m_surface->widget()->devicePixelRatioF();
    const QSize logical = m_surface->pictureRect().size();
    m_engine->setOutputClient(this, QSize(int(logical.width() * dpr), int(logical.height() * dpr)),
        m_surface->canDrawTextures());
}

void CompViewer::showEvent(QShowEvent* event)
{
    PanelFrame::showEvent(event);
    updateOutputClient();
    refresh();
}

void CompViewer::hideEvent(QHideEvent* event)
{
    PanelFrame::hideEvent(event);
    if (m_engine)
        m_engine->removeOutputClient(this);
}

void CompViewer::resizeEvent(QResizeEvent* event)
{
    PanelFrame::resizeEvent(event);
    updateOutputClient();
    requestRender();
}

void CompViewer::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat(QLatin1String(mime::CompNode)))
        event->acceptProposedAction();
}

void CompViewer::dropEvent(QDropEvent* event)
{
    const quint64 id = event->mimeData()->data(QLatin1String(mime::CompNode)).toULongLong();
    if (m_session && id) {
        m_session->setViewerNode(m_index, id);
        event->acceptProposedAction();
    }
}

} // namespace kk
