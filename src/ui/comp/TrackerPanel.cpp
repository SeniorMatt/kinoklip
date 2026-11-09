// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/comp/TrackerPanel.h"

#include "compositing/tracking/TrackStore.h"
#include "core/NodeRegistry.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/AppContext.h"
#include "ui/common/Metrics.h"
#include "ui/comp/CompSession.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {

QToolButton* makeButton(const QString& name, const QString& text, const QString& tip)
{
    auto* b = new QToolButton;
    b->setObjectName(name);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return b;
}

} // namespace

TrackerPanel::TrackerPanel(AppContext& ctx, CompSession* session, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
    , m_session(session)
{
    setObjectName(QStringLiteral("TrackerPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceS, metrics::kSpaceM, metrics::kSpaceS);
    auto* row = new QHBoxLayout;
    m_backward = makeButton(QStringLiteral("TrackBackward"), QStringLiteral("◀ ") + tr("Track"),
        tr("Track backward from the playhead"));
    m_stop = makeButton(QStringLiteral("TrackStop"), QStringLiteral("■ ") + tr("Stop"),
        tr("Stop tracking (the frames tracked so far are kept)"));
    m_forward = makeButton(QStringLiteral("TrackForward"), tr("Track") + QStringLiteral(" ▶"),
        tr("Track forward from the playhead"));
    m_clear = makeButton(QStringLiteral("TrackClear"), tr("Clear"),
        tr("Remove the tracked positions (the point stays where it is now)"));
    for (QToolButton* b : {m_backward, m_stop, m_forward})
        row->addWidget(b);
    row->addStretch(1);
    row->addWidget(m_clear);
    layout->addLayout(row);
    m_bar = new QProgressBar;
    m_bar->setObjectName(QStringLiteral("TrackProgress"));
    m_bar->setRange(0, 100);
    m_bar->setTextVisible(true);
    m_bar->hide();
    layout->addWidget(m_bar);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("TrackStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    connect(m_backward, &QToolButton::clicked, this, [this] { track(false); });
    connect(m_forward, &QToolButton::clicked, this, [this] { track(true); });
    connect(m_stop, &QToolButton::clicked, this, &TrackerPanel::stop);
    connect(m_clear, &QToolButton::clicked, this, [this] {
        if (!m_session || !m_node)
            return;
        const quint64 id = m_node;
        const int64_t frame = m_session->compFrame();
        const CompNode* n = m_session->composition().node(id);
        m_session->edit(
            tr("Clear Track %1").arg(n ? n->name : QString()), [id, frame](Composition& c) {
                CompNode* node = c.node(id);
                if (!node || node->locked)
                    return false;
                tracking::clearTrack(*node, frame);
                return true;
            });
    });
    connect(&m_runner, &tracking::TrackerRunner::progress, this, [this](int done, int total) {
        m_bar->setRange(0, std::max(1, total));
        m_bar->setValue(done);
    });
    connect(&m_runner, &tracking::TrackerRunner::sampleReady, this,
        [this](const tracking::TrackSample& s) {
            if (m_session)
                m_session->seekCompFrame(s.frame);
        });
    connect(&m_runner, &tracking::TrackerRunner::finished, this, &TrackerPanel::onFinished);
    if (session) {
        connect(session, &CompSession::clipChanged, this, [this] {
            if (m_runner.isRunning()) {
                m_discard = true;
                m_runner.stop();
            }
        });
        connect(session, &CompSession::compositionChanged, this, &TrackerPanel::updateState);
        connect(session, &CompSession::selectionChanged, this, &TrackerPanel::updateState);
    }
    updateState();
}

void TrackerPanel::setNode(quint64 nodeId)
{
    m_node = nodeId;
    updateState();
}

QString TrackerPanel::statusText() const
{
    return m_status->text();
}

void TrackerPanel::setStatus(const QString& text)
{
    m_status->setText(text);
}

void TrackerPanel::updateState()
{
    const CompNode* n = m_session ? m_session->composition().node(m_node) : nullptr;
    const bool running = m_runner.isRunning();
    const bool editable = n && !n->locked && m_ctx.playback;
    m_backward->setEnabled(editable && !running);
    m_forward->setEnabled(editable && !running);
    m_stop->setEnabled(running);
    // The point tracker's track is its X key; the planar tracker's its first corner's.
    const QString trackKey = n && n->type == nodetypes::PlanarTracker
        ? nodekeys::planarCorner(0, false)
        : nodekeys::CenterX;
    m_clear->setEnabled(
        n && !n->locked && !running && n->params.values.value(trackKey).isAnimated());
    m_bar->setVisible(running && m_runNode == m_node);
}

bool TrackerPanel::track(bool forward)
{
    m_discard = false;
    const CompNode* n = m_session ? m_session->composition().node(m_node) : nullptr;
    const bool planar = n && n->type == nodetypes::PlanarTracker;
    if (!n || (n->type != nodetypes::Tracker && !planar) || n->locked || m_runner.isRunning())
        return false;
    if (!m_ctx.playback || !m_ctx.playback->compositor() || !m_session->isStored()) {
        setStatus(tr("Tracking needs the playback engine."));
        return false;
    }
    // The picture to follow: the tracking source if connected, else the node's own input.
    quint64 sourceNode = 0;
    for (const QString& port : {nodeports::Source, nodeports::Input}) {
        const NodeLink link = n->inputs.value(port);
        if (link.isConnected()) {
            sourceNode = link.nodeId;
            break;
        }
    }
    if (!sourceNode) {
        setStatus(tr("Connect a picture to the tracker's input first."));
        return false;
    }
    const int64_t start = m_session->compFrame();
    const int64_t last = forward ? m_session->compFrameCount() - 1 : 0;
    if (start == last) {
        setStatus(forward ? tr("The playhead is on the last frame.")
                          : tr("The playhead is on the first frame."));
        return false;
    }
    const QSize frame = m_session->frameSize();
    if (frame.isEmpty())
        return false;
    tracking::TrackJob job;
    job.firstFrame = start;
    job.lastFrame = last;
    job.startU = n->params.value(nodekeys::CenterX, start, 0.5);
    job.startV = n->params.value(nodekeys::CenterY, start, 0.5);
    if (planar) {
        // Pattern Size is the side of a patch, Search Size the side of the box it may move in.
        job.planar = true;
        for (int i = 0; i < nodekeys::kPlanarCorners; ++i) {
            job.startCorners[size_t(2 * i)]
                = n->params.value(nodekeys::planarCorner(i, false), start, 0.5);
            job.startCorners[size_t(2 * i + 1)]
                = n->params.value(nodekeys::planarCorner(i, true), start, 0.5);
        }
    }
    job.patternFraction = n->params.value(nodekeys::PatternSize, 0, 48.0) / frame.width();
    job.searchFraction = n->params.value(nodekeys::SearchSize, 0, 160.0) / frame.width();
    // Analysis size: the timeline resolution, at most 960 wide (the engine may lower it further).
    const double scale = std::min(1.0, 960.0 / frame.width());
    const QSize outSize(
        std::max(16, int(frame.width() * scale)), std::max(16, int(frame.height() * scale)));
    const quint64 clipId = m_session->clipId();
    const quint64 renderId = sourceNode == m_session->composition().mediaOutId ? 0 : sourceNode;
    PlaybackEngine* engine = m_ctx.playback;
    const auto timeline = m_ctx.project->timeline()->snapshot();
    job.source = [engine, timeline, clipId, renderId, outSize](int64_t f) {
        return engine->renderCompositionNode(timeline, clipId, renderId, f, outSize, OutputImage);
    };
    m_runNode = m_node;
    m_runStart = start;
    setStatus(forward ? tr("Tracking forward...") : tr("Tracking backward..."));
    if (!m_runner.start(std::move(job)))
        return false;
    updateState();
    return true;
}

void TrackerPanel::stop()
{
    m_runner.stop();
}

void TrackerPanel::onFinished(tracking::TrackOutcome outcome, const QString& reason)
{
    const auto& samples = m_runner.samples();
    const int64_t reached = samples.empty() ? m_runStart : samples.back().frame;
    if (!m_discard && m_session && !samples.empty()) {
        const quint64 id = m_runNode;
        const std::vector<tracking::TrackSample> found = samples;
        const int64_t start = m_runStart;
        const CompNode* n = m_session->composition().node(id);
        m_session->edit(
            tr("Track %1").arg(n ? n->name : QString()), [id, found, start](Composition& c) {
                CompNode* node = c.node(id);
                return node && !node->locked && tracking::storeTrack(*node, found, start);
            });
    }
    switch (outcome) {
    case tracking::TrackOutcome::Finished:
        setStatus(tr("Tracked %1 frames.").arg(samples.size()));
        break;
    case tracking::TrackOutcome::Stopped:
        setStatus(m_discard ? QString() : tr("Stopped at frame %1.").arg(reached));
        break;
    case tracking::TrackOutcome::Lost:
    case tracking::TrackOutcome::Failed:
        setStatus(tr("%1 Tracked up to frame %2.").arg(reason).arg(reached));
        break;
    }
    updateState();
}

} // namespace kk
