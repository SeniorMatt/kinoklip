// SPDX-License-Identifier: GPL-3.0-or-later
// StackedTimelinePane: the inactive timeline of the Stacked Timelines view (read-only canvas,
// own tabs, playhead and zoom; click to activate, drag clips out / in to copy them).
#include "ui/timeline/StackedTimelinePane.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Icons.h"
#include "ui/common/SchemeColors.h"
#include "ui/timeline/NestedPreview.h"
#include "ui/timeline/TimelineMedia.h"

#include <QApplication>
#include <QDrag>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace kk {

using namespace timelinemetrics;

namespace {
constexpr double kWheelZoomPerDegree = 1.0035; ///< like TimelineWidget
constexpr double kExtraScrollFraction = 0.5; ///< scrollable room after the last clip
constexpr int64_t kEmptyTimelineFrames = 250; ///< fit range of an empty timeline
constexpr int kInactiveShadeAlpha = 40; ///< dims the inactive timeline a little (scheme shadow)

/// MIME payload of a copy drag of @p clips of timeline @p uid (DragMime.h).
QByteArray clipsPayload(quint64 uid, const QVector<quint64>& clips, int64_t grabFrames)
{
    QJsonArray ids;
    for (const quint64 id : clips)
        ids.append(qint64(id));
    return QJsonDocument(
        QJsonObject {{QLatin1String("timeline"), qint64(uid)}, {QLatin1String("clips"), ids},
            {QLatin1String("grabFrames"), qint64(grabFrames)}})
        .toJson(QJsonDocument::Compact);
}

} // namespace

// ---- StackedTimelineView ----

StackedTimelineView::StackedTimelineView(AppContext& ctx, QWidget* parent)
    : QAbstractScrollArea(parent)
    , m_ctx(ctx)
{
    setObjectName(QStringLiteral("StackedTimelineView"));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::ClickFocus);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setMinimumHeight(kRulerHeight + 2 * kDefaultAudioHeight);
    m_painter.setStyle(palette(), font());
    // Clip, track and state colours come from SchemeColors, which has the new scheme's roles
    // only after kk::theme is done (a PaletteChange can arrive first).
    connect(SchemeColorsNotifier::instance(), &SchemeColorsNotifier::changed, this, [this] {
        m_painter.setStyle(palette(), font());
        viewport()->update();
    });
    m_nestedPreview = new NestedPreview(ctx.project, this);
    connect(m_nestedPreview, &NestedPreview::ready, viewport(), [this] { viewport()->update(); });
    m_painter.setThumbnailProvider(m_nestedPreview->wrap(mediaCacheThumbnails()));
    m_painter.setWaveformProvider(m_nestedPreview->wrap(mediaCacheWaveforms()));
    connectMediaCache(this, [this] { viewport()->update(); });

    Project* project = ctx.project;
    // Inactive timelines change through undo / redo, renames, nesting and activation.
    connect(project, &Project::timelinesChanged, this, &StackedTimelineView::refresh);
    connect(project, &Project::activeTimelineChanged, this, &StackedTimelineView::refresh);
    connect(project, &Project::projectReset, this, [this] { setTimeline(0); });
    connect(project->timeline(), &Timeline::changed, this, &StackedTimelineView::refresh);
    connect(project, &Project::inactivePlayheadChanged, this, [this](quint64 uid) {
        if (uid == m_uid)
            viewport()->update();
    });
}

void StackedTimelineView::setTimeline(quint64 uid)
{
    if (m_uid != 0 && m_uid != uid)
        storeViewState();
    m_uid = uid;
    m_data = uid ? m_ctx.project->timelineData(uid) : nullptr;
    if (m_data)
        m_layout.rebuild(*m_data);
    const TimelineViewState v = uid ? m_ctx.project->viewState(uid) : TimelineViewState {};
    if (v.pixelsPerFrame > 0.0) {
        m_fitPending = false;
        m_layout.setPixelsPerFrame(v.pixelsPerFrame, kHeaderWidth);
        m_layout.setScrollX(v.scrollX);
        m_layout.setScrollY(v.scrollY);
    } else {
        m_fitPending = true;
        if (viewport()->width() > kHeaderWidth + 50)
            zoomToFit();
    }
    updateScrollBars();
    viewport()->update();
}

void StackedTimelineView::storeViewState()
{
    if (m_uid && m_ctx.project->hasTimeline(m_uid))
        m_ctx.project->setViewState(m_uid,
            TimelineViewState {m_layout.pixelsPerFrame(), int(std::lround(m_layout.scrollX())),
                m_layout.scrollY()});
}

void StackedTimelineView::refresh()
{
    if (m_uid && !m_ctx.project->hasTimeline(m_uid)) {
        m_uid = 0;
        m_data.reset();
    } else if (m_uid) {
        m_data = m_ctx.project->timelineData(m_uid);
        m_layout.rebuild(*m_data);
    }
    updateScrollBars();
    viewport()->update();
}

void StackedTimelineView::zoomToFit()
{
    m_layout.setViewSize(viewport()->size());
    const int64_t duration = m_data ? m_data->duration() : 0;
    m_layout.setPixelsPerFrame(
        m_layout.fitPixelsPerFrame(duration > 0 ? duration : kEmptyTimelineFrames), kHeaderWidth);
    m_layout.setScrollX(0);
    m_fitPending = false;
}

void StackedTimelineView::updateScrollBars()
{
    m_layout.setViewSize(viewport()->size());
    m_syncingScrollBars = true;
    const int trackWidth = m_layout.trackAreaWidth();
    const double contentWidth = (m_data ? m_data->duration() : 0) * m_layout.pixelsPerFrame()
        + trackWidth * kExtraScrollFraction;
    const int maxX
        = std::max(int(std::ceil(contentWidth)) - trackWidth, int(std::lround(m_layout.scrollX())));
    horizontalScrollBar()->setRange(0, std::max(0, maxX));
    horizontalScrollBar()->setPageStep(std::max(1, trackWidth));
    horizontalScrollBar()->setValue(int(std::lround(m_layout.scrollX())));
    verticalScrollBar()->setRange(0, m_layout.maxScrollY());
    verticalScrollBar()->setPageStep(std::max(1, m_layout.rowAreaHeight()));
    verticalScrollBar()->setValue(m_layout.scrollY());
    m_layout.setScrollY(verticalScrollBar()->value());
    m_syncingScrollBars = false;
}

void StackedTimelineView::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    m_layout.setViewSize(viewport()->size());
    if (m_fitPending && m_data && viewport()->width() > kHeaderWidth + 50)
        zoomToFit();
    updateScrollBars();
}

void StackedTimelineView::scrollContentsBy(int, int)
{
    if (m_syncingScrollBars)
        return;
    m_layout.setScrollX(horizontalScrollBar()->value());
    m_layout.setScrollY(verticalScrollBar()->value());
    viewport()->update();
}

void StackedTimelineView::wheelEvent(QWheelEvent* event)
{
    // Its own zoom: Alt or Ctrl + wheel zooms around the pointer; the wheel alone scrolls.
    const QPoint angle = event->angleDelta();
    const int delta = angle.y() != 0 ? angle.y() : angle.x();
    if (event->modifiers() & (Qt::AltModifier | Qt::ControlModifier)) {
        const double anchor = std::max(event->position().x(), double(kHeaderWidth));
        m_layout.setPixelsPerFrame(
            m_layout.pixelsPerFrame() * std::pow(kWheelZoomPerDegree, delta), anchor);
        updateScrollBars();
        viewport()->update();
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void StackedTimelineView::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange)
        m_painter.setStyle(palette(), font());
    QAbstractScrollArea::changeEvent(event);
}

void StackedTimelineView::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    if (!m_data) {
        p.fillRect(viewport()->rect(), palette().base());
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(viewport()->rect(), Qt::AlignCenter,
            tr("No other timeline open here. Use + to open one, or create one with File > New "
               "Timeline."));
        return;
    }
    TimelinePaintInput in;
    in.data = m_data.get();
    in.layout = &m_layout;
    in.selected = &m_selected;
    in.overlay = &m_overlay;
    in.mediaPool = m_ctx.project->mediaPool();
    in.playhead = m_ctx.project->timelinePlayhead(m_uid);
    in.hasFocus = false;
    m_painter.paint(p, in);
    // The inactive one reads as such: slightly dimmed (the active timeline drives the viewer).
    p.fillRect(viewport()->rect(), withAlpha(schemeColors().shadow, kInactiveShadeAlpha));
}

void StackedTimelineView::mousePressEvent(QMouseEvent* event)
{
    if (!m_data || event->button() != Qt::LeftButton)
        return;
    const QPointF pos = event->position();
    const TimelineHit hit = m_layout.hitTest(*m_data, pos);
    if (hit.area == TimelineHit::Area::Ruler) {
        // Its own playhead: scrubbing here does not activate it.
        m_ctx.project->setTimelinePlayhead(m_uid, m_layout.frameAtX(pos.x()));
        return;
    }
    m_pressed = true;
    m_pressPos = pos;
    m_pressClip = hit.area == TimelineHit::Area::Clip ? hit.clipId : 0;
}

void StackedTimelineView::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_pressed || !m_data)
        return;
    if (m_pressClip == 0) {
        return;
    }
    if ((event->position() - m_pressPos).manhattanLength() < QApplication::startDragDistance())
        return;
    // Drag the clip (with its linked partners) out: a copy into the active timeline.
    m_pressed = false;
    QVector<quint64> clips = editops::linkedClips(*m_data, m_pressClip);
    if (!clips.contains(m_pressClip))
        clips.prepend(m_pressClip);
    int64_t earliest = std::numeric_limits<int64_t>::max();
    for (const quint64 id : clips) {
        if (const ClipData* c = editops::clipById(*m_data, id))
            earliest = std::min(earliest, c->start);
    }
    const int64_t grab
        = std::max<int64_t>(0, int64_t(std::floor(m_layout.xToFrame(m_pressPos.x()))) - earliest);
    auto* mime = new QMimeData;
    mime->setData(QLatin1String(mime::TimelineClips), clipsPayload(m_uid, clips, grab));
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction, Qt::CopyAction);
    m_pressClip = 0;
}

void StackedTimelineView::mouseReleaseEvent(QMouseEvent* event)
{
    if (!m_pressed || event->button() != Qt::LeftButton)
        return;
    m_pressed = false;
    m_pressClip = 0;
    emit activateRequested(); // a click (no drag) makes this timeline the active one
}

bool StackedTimelineView::planDrop(const QMimeData* mime, QPointF pos, int64_t* frame,
    TrackRef* track, quint64* sourceUid, QVector<quint64>* clips)
{
    m_overlay.ghosts.clear();
    if (!m_data || !mime->hasFormat(QLatin1String(mime::TimelineClips)))
        return false;
    const QJsonObject o
        = QJsonDocument::fromJson(mime->data(QLatin1String(mime::TimelineClips))).object();
    *sourceUid = quint64(o.value(QLatin1String("timeline")).toInteger());
    if (*sourceUid == m_uid)
        return false;
    const auto source = m_ctx.project->timelineData(*sourceUid);
    if (!source)
        return false;
    clips->clear();
    for (const QJsonValue& v : o.value(QLatin1String("clips")).toArray())
        clips->append(quint64(v.toInteger()));
    const editops::Clipboard board = editops::copyClips(*source, *clips);
    if (board.isEmpty())
        return false;
    const TimelineHit hit = m_layout.hitTest(*m_data, pos);
    if (hit.area != TimelineHit::Area::Clip && hit.area != TimelineHit::Area::Track)
        return false;
    *track = hit.track;
    *frame = std::max<int64_t>(
        0, m_layout.nearestFrameAtX(pos.x()) - o.value(QLatin1String("grabFrames")).toInteger());
    for (const editops::ClipboardClip& c : board.clips) {
        if (c.type == TrackType::Subtitle)
            continue;
        const int row
            = m_layout.rowOf(TrackRef {c.type, std::max(0, track->index) + c.trackOffset});
        if (row >= 0)
            m_overlay.ghosts.push_back(
                {row, QRect(), *frame + c.clip.start, c.clip.duration, c.clip.name});
    }
    return true;
}

void StackedTimelineView::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasFormat(QLatin1String(mime::TimelineClips))) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}

void StackedTimelineView::dragMoveEvent(QDragMoveEvent* event)
{
    int64_t frame = 0;
    TrackRef track;
    quint64 source = 0;
    QVector<quint64> clips;
    if (planDrop(event->mimeData(), event->position(), &frame, &track, &source, &clips)) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        event->ignore();
    }
    viewport()->update();
}

void StackedTimelineView::dragLeaveEvent(QDragLeaveEvent*)
{
    m_overlay.ghosts.clear();
    viewport()->update();
}

void StackedTimelineView::dropEvent(QDropEvent* event)
{
    int64_t frame = 0;
    TrackRef track;
    quint64 source = 0;
    QVector<quint64> clips;
    const bool ok = planDrop(event->mimeData(), event->position(), &frame, &track, &source, &clips);
    m_overlay.ghosts.clear();
    viewport()->update();
    if (!ok) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    const bool insert = event->modifiers() & Qt::ControlModifier;
    emit clipsDropped(source, clips, frame, track, insert);
}

// ---- StackedTimelinePane ----

StackedTimelinePane::StackedTimelinePane(AppContext& ctx, QWidget* parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    setObjectName(QStringLiteral("StackedTimelinePane"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    m_bar = new QTabBar(this);
    m_bar->setObjectName(QStringLiteral("StackedTimelineTabBar"));
    m_bar->setDocumentMode(true);
    m_bar->setExpanding(false);
    m_bar->setElideMode(Qt::ElideRight);
    m_bar->setUsesScrollButtons(true);
    m_bar->setFocusPolicy(Qt::NoFocus);
    m_bar->setTabsClosable(true);
    row->addWidget(m_bar, 1);
    m_open = new QToolButton(this);
    m_open->setObjectName(QStringLiteral("StackedTimelineOpen"));
    m_open->setText(QStringLiteral("+"));
    m_open->setAutoRaise(true);
    m_open->setToolTip(tr("Open another timeline in this stacked timeline"));
    m_open->setFocusPolicy(Qt::NoFocus);
    row->addWidget(m_open);
    layout->addLayout(row);
    m_view = new StackedTimelineView(ctx, this);
    layout->addWidget(m_view, 1);

    connect(m_bar, &QTabBar::currentChanged, this, [this](int index) {
        if (!m_syncing && index >= 0 && index < m_tabs.size())
            m_view->setTimeline(m_tabs[index]);
    });
    connect(m_bar, &QTabBar::tabCloseRequested, this, [this](int index) {
        if (index < 0 || index >= m_tabs.size())
            return;
        const quint64 closed = m_tabs.takeAt(index);
        if (closed == m_view->timelineUid())
            m_view->setTimeline(
                m_tabs.isEmpty() ? 0 : m_tabs[std::min<int>(index, int(m_tabs.size()) - 1)]);
        syncTabs();
    });
    connect(m_open, &QToolButton::clicked, this, &StackedTimelinePane::showOpenMenu);
    connect(m_view, &StackedTimelineView::activateRequested, this,
        &StackedTimelinePane::activateRequested);
    connect(m_view, &StackedTimelineView::clipsDropped, this, &StackedTimelinePane::clipsDropped);
    Project* project = ctx.project;
    connect(project, &Project::timelinesChanged, this, [this] {
        // Deleted timelines leave the pane's tabs; renames show.
        m_tabs.erase(std::remove_if(m_tabs.begin(), m_tabs.end(),
                         [this](quint64 uid) { return !m_ctx.project->hasTimeline(uid); }),
            m_tabs.end());
        syncTabs();
    });
    connect(project, &Project::projectReset, this, [this] {
        m_tabs.clear();
        syncTabs();
    });
}

void StackedTimelinePane::setTabs(const QVector<quint64>& tabs, quint64 current)
{
    m_tabs.clear();
    for (const quint64 uid : tabs) {
        if (m_ctx.project->hasTimeline(uid) && !m_tabs.contains(uid))
            m_tabs.append(uid);
    }
    if (current && m_ctx.project->hasTimeline(current) && !m_tabs.contains(current))
        m_tabs.append(current);
    m_view->setTimeline(
        m_tabs.contains(current) ? current : (m_tabs.isEmpty() ? 0 : m_tabs.front()));
    syncTabs();
}

void StackedTimelinePane::showTimeline(quint64 uid)
{
    if (!m_ctx.project->hasTimeline(uid))
        return;
    if (!m_tabs.contains(uid))
        m_tabs.append(uid);
    m_view->setTimeline(uid);
    syncTabs();
}

void StackedTimelinePane::syncTabs()
{
    m_syncing = true;
    while (m_bar->count() > 0)
        m_bar->removeTab(m_bar->count() - 1);
    for (const quint64 uid : std::as_const(m_tabs)) {
        const auto data = m_ctx.project->timelineData(uid);
        const int index
            = m_bar->addTab(icon(data && data->compound ? QStringLiteral("compound_clip")
                                                        : QStringLiteral("timeline")),
                data ? data->name : QString());
        if (uid == m_view->timelineUid())
            m_bar->setCurrentIndex(index);
    }
    m_syncing = false;
}

void StackedTimelinePane::showOpenMenu()
{
    QMenu menu(this);
    const quint64 active = m_ctx.project->activeTimelineUid();
    for (const quint64 uid : m_ctx.project->timelineUids()) {
        const auto data = m_ctx.project->timelineData(uid);
        if (!data || uid == active)
            continue; // the active timeline is already shown in the other pane
        QAction* a = menu.addAction(
            icon(data->compound ? QStringLiteral("compound_clip") : QStringLiteral("timeline")),
            data->name, this, [this, uid] { showTimeline(uid); });
        a->setCheckable(true);
        a->setChecked(uid == m_view->timelineUid());
    }
    if (menu.isEmpty())
        menu.addAction(tr("No other timeline"))->setEnabled(false);
    menu.exec(m_open->mapToGlobal(QPoint(0, m_open->height())));
}

} // namespace kk
