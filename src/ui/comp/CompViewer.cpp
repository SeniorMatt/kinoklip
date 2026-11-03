// SPDX-License-Identifier: GPL-3.0-or-later
// CompViewer implementation: header, surface, guides, node drops and the picture source.
#include "ui/comp/CompViewer.h"

#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Metrics.h"
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
    setObjectName(index == 0 ? QStringLiteral("CompViewerLeft") : QStringLiteral("CompViewerRight"));
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
    m_controls = new CompViewerOverlay(session, m_surface);

    connect(session, &CompSession::viewersChanged, this, &CompViewer::refresh);
    connect(session, &CompSession::clipChanged, this, &CompViewer::refresh);
    connect(session, &CompSession::compositionChanged, this, &CompViewer::updateHeader);
    connect(ctx.project->timeline(), &Timeline::playheadChanged, this, &CompViewer::updateHeader);
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
        m_surface->clear(m_index == 0 ? tr("Select a node and press 1") : tr("Select a node and press 2"));
        return;
    }
    // TODO(comp): the node's own image (core render API) once merged; until then the program
    // picture under the playhead (what MediaIn delivers).
    if (m_engine && m_engine->currentFrame())
        m_surface->setRenderedFrame(m_engine->currentFrame());
    else
        m_surface->clear();
}

void CompViewer::showFrame(const RenderedFramePtr& frame)
{
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
