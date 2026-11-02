// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineWidget: construction, painting, zoom, scrolling, playhead follow, context menus.
// The mouse state machine lives in TimelineInput.cpp, drag and drop in TimelineDrop.cpp.
#include "ui/timeline/TimelineWidget.h"

#include "core/EditBehaviour.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/TimelineController.h"

#include <QContextMenuEvent>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

#include <cmath>

namespace kk {

using namespace timelinemetrics;

namespace {
constexpr double kZoomStep = 1.5; ///< Ctrl+= / Ctrl+- factor
constexpr double kWheelZoomPerDegree = 1.0035; ///< Ctrl+wheel: factor per 1/8 degree
constexpr double kExtraScrollFraction = 0.5; ///< scrollable room after the last clip (views)
constexpr double kFollowMargin = 0.1; ///< page-scroll keeps this fraction left of the playhead
constexpr int64_t kEmptyTimelineFrames = 250; ///< zoom-to-fit range of an empty timeline
} // namespace

TimelineWidget::TimelineWidget(AppContext& ctx, TimelineController* controller, QWidget* parent)
    : QAbstractScrollArea(parent)
    , m_ctx(ctx)
    , m_controller(controller)
{
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setMinimumHeight(kRulerHeight + 2 * kDefaultAudioHeight);

    m_painter.setStyle(palette(), font());
    m_painter.setThumbnailProvider(mediaCacheThumbnails());
    m_painter.setWaveformProvider(mediaCacheWaveforms());
    connectMediaCache(this, [this] { refreshMedia(); });

    Timeline* timeline = ctx.project->timeline();
    connect(timeline, &Timeline::changed, this, &TimelineWidget::onTimelineChanged);
    connect(timeline, &Timeline::playheadChanged, this, &TimelineWidget::onPlayheadChanged);
    connect(ctx.selection, &SelectionModel::changed, this, &TimelineWidget::onSelectionChanged);
    connect(ctx.project, &Project::projectReset, this, [this] {
        m_fitOnFirstShow = true;
        onTimelineChanged();
        zoomToFit();
    });
    connect(m_controller, &TimelineController::stateChanged, this, [this] {
        updateHover(viewport()->mapFromGlobal(QCursor::pos()));
        viewport()->update();
    });

    // Keyframe / curve editors below clips (additive layer: own widgets, own input).
    m_keyframeLanes = new KeyframeLanes(ctx, m_layout, m_controller, viewport(), this);
    connect(
        m_keyframeLanes, &KeyframeLanes::layoutChanged, this, &TimelineWidget::onTimelineChanged);

    m_autoScroll.setInterval(30);
    connect(&m_autoScroll, &QTimer::timeout, this, &TimelineWidget::autoScrollTick);

    m_layout.rebuild(timeline->data());
    onSelectionChanged();
}

TimelineWidget::~TimelineWidget()
{
    // Never leave an edit open (e.g. the widget is destroyed mid-drag).
    if (m_drag.editOpen)
        m_ctx.project->cancelEdit();
}

void TimelineWidget::setThumbnailProvider(ThumbnailProvider provider)
{
    m_painter.setThumbnailProvider(std::move(provider));
    viewport()->update();
}

void TimelineWidget::setWaveformProvider(WaveformProvider provider)
{
    m_painter.setWaveformProvider(std::move(provider));
    viewport()->update();
}

void TimelineWidget::refreshMedia()
{
    viewport()->update();
}

// ---- Model notifications ----

void TimelineWidget::onTimelineChanged()
{
    m_layout.setLaneHeights(m_keyframeLanes->laneHeights(m_ctx.project->timeline()->data()));
    m_layout.rebuild(m_ctx.project->timeline()->data());
    updateScrollBars();
    viewport()->update();
}

void TimelineWidget::onSelectionChanged()
{
    const QVector<quint64>& ids = m_ctx.selection->clips();
    m_selected = QSet<quint64>(ids.cbegin(), ids.cend());
    viewport()->update();
}

void TimelineWidget::onPlayheadChanged(int64_t)
{
    // While the user scrubs, the view must not jump under the mouse.
    if (m_drag.mode != DragMode::Scrub && m_followPlayhead)
        ensurePlayheadVisible();
    viewport()->update();
}

void TimelineWidget::ensurePlayheadVisible()
{
    const double x = m_layout.frameToX(double(m_ctx.project->timeline()->playhead()));
    const int width = viewport()->width();
    if (x >= kHeaderWidth && x <= width - 2)
        return;
    // Page scroll: the playhead lands near the left edge, so playback keeps running into the
    // view for almost a whole page before the next jump.
    const double frame = double(m_ctx.project->timeline()->playhead());
    scrollToFrame(frame - kFollowMargin * m_layout.trackAreaWidth() / m_layout.pixelsPerFrame());
}

// ---- Painting ----

void TimelineWidget::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    TimelinePaintInput in;
    in.data = &m_ctx.project->timeline()->data();
    in.layout = &m_layout;
    in.selected = &m_selected;
    in.overlay = &m_overlay;
    in.mediaPool = m_ctx.project->mediaPool();
    in.playhead = m_ctx.project->timeline()->playhead();
    in.targetVideo = m_controller->targetVideoTrack();
    in.targetAudio = m_controller->targetAudioTrack();
    in.editTailClip = m_controller->selectedEdit().tailClip;
    in.editHeadClip = m_controller->selectedEdit().headClip;
    if (const auto& gap = m_controller->selectedGap()) {
        in.gapRow = m_layout.rowOf(gap->track);
        in.gapStart = gap->start;
        in.gapEnd = gap->end;
    }
    in.hasFocus = hasFocus();
    m_painter.paint(p, in);
    m_keyframeLanes->paintClipButtons(p);
}

void TimelineWidget::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange)
        m_painter.setStyle(palette(), font());
    QAbstractScrollArea::changeEvent(event);
}

void TimelineWidget::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    m_layout.setViewSize(viewport()->size());
    if (m_fitOnFirstShow && viewport()->width() > kHeaderWidth + 50) {
        m_fitOnFirstShow = false;
        zoomToFit();
    }
    updateScrollBars();
}

// ---- Scrolling ----

void TimelineWidget::updateScrollBars()
{
    m_layout.setViewSize(viewport()->size());
    m_syncingScrollBars = true;
    const TimelineData& data = m_ctx.project->timeline()->data();
    const int trackWidth = m_layout.trackAreaWidth();
    const double contentWidth
        = data.duration() * m_layout.pixelsPerFrame() + trackWidth * kExtraScrollFraction;
    const int maxX = std::max(int(std::ceil(contentWidth)) - trackWidth,
        int(std::lround(m_layout.scrollX()))); // never clamp away the current position
    QScrollBar* h = horizontalScrollBar();
    h->setRange(0, std::max(0, maxX));
    h->setPageStep(std::max(1, trackWidth));
    h->setSingleStep(24);
    h->setValue(int(std::lround(m_layout.scrollX())));

    QScrollBar* v = verticalScrollBar();
    v->setRange(0, m_layout.maxScrollY());
    v->setPageStep(std::max(1, m_layout.rowAreaHeight()));
    v->setSingleStep(16);
    v->setValue(m_layout.scrollY());
    m_layout.setScrollY(v->value());
    m_syncingScrollBars = false;
    m_keyframeLanes->syncGeometry();
}

void TimelineWidget::scrollContentsBy(int, int)
{
    if (m_syncingScrollBars)
        return;
    m_layout.setScrollX(horizontalScrollBar()->value());
    m_layout.setScrollY(verticalScrollBar()->value());
    // A drag in progress follows the content (e.g. edge auto-scroll).
    if (m_drag.mode != DragMode::None && m_drag.mode != DragMode::Pending
        && m_drag.mode != DragMode::Pan)
        updateDrag(m_drag.lastPos);
    m_keyframeLanes->syncGeometry();
    viewport()->update();
}

void TimelineWidget::scrollToFrame(double frame)
{
    m_layout.setScrollX(std::max(0.0, frame) * m_layout.pixelsPerFrame());
    updateScrollBars();
    viewport()->update();
}

void TimelineWidget::wheelEvent(QWheelEvent* event)
{
    // The modifier convention and the zoom anchor are preferences (EditBehaviour, spec
    // 19.3 / 19.6 / 19.7); a few QSettings lookups per wheel event are cheap.
    const EditBehaviour behaviour = EditBehaviour::fromSettings();
    const QPoint angle = event->angleDelta();
    // Qt turns Alt+wheel into a horizontal delta on some platforms: use whichever is set.
    const int anyDelta = angle.y() != 0 ? angle.y() : angle.x();
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool resolve = behaviour.wheelScheme == WheelScheme::Resolve;
    const double pointerX = event->position().x();
    auto wheelZoom = [&] {
        const double anchor = behaviour.zoomAnchor == ZoomAnchor::Cursor && pointerX >= kHeaderWidth
            ? pointerX
            : zoomAnchorX();
        zoomBy(std::pow(kWheelZoomPerDegree, anyDelta), anchor);
    };
    QScrollBar* h = horizontalScrollBar();
    QScrollBar* v = verticalScrollBar();
    const int timeStep = m_layout.trackAreaWidth() / 1200; // pixels of scroll per 1/8 degree
    if (resolve) {
        if (mods & Qt::AltModifier) {
            wheelZoom();
        } else if (mods & Qt::ShiftModifier) {
            setTrackHeightScale(m_layout.heightScale() * std::pow(kWheelZoomPerDegree, anyDelta));
        } else if ((mods & Qt::ControlModifier) || angle.x() != 0) {
            // Ctrl+wheel and horizontal touchpad swipes scroll time.
            h->setValue(
                h->value() - (angle.x() != 0 ? angle.x() : angle.y()) * std::max(1, timeStep));
        } else {
            v->setValue(v->value() - angle.y() / 2);
        }
        event->accept();
        return;
    }
    if (mods & Qt::ControlModifier) {
        wheelZoom();
        event->accept();
        return;
    }
    // Classic: Shift+wheel (some platforms already turn it into a horizontal delta) and
    // horizontal touchpad swipes scroll time; the plain wheel scrolls tracks when they
    // overflow, otherwise time.
    const bool shift = mods & Qt::ShiftModifier;
    const int dx = angle.x() != 0 ? angle.x() : (shift ? angle.y() : 0);
    if (dx != 0) {
        h->setValue(h->value() - dx * m_layout.trackAreaWidth() / 1200);
    } else if (m_layout.maxScrollY() > 0) {
        v->setValue(v->value() - angle.y() / 2);
    } else {
        h->setValue(h->value() - angle.y() * m_layout.trackAreaWidth() / 1200);
    }
    event->accept();
}

void TimelineWidget::setTrackHeightScale(double scale)
{
    m_layout.setHeightScale(scale);
    m_layout.rebuild(m_ctx.project->timeline()->data());
    updateScrollBars();
    viewport()->update();
}

// ---- Zoom ----

double TimelineWidget::zoomAnchorX() const
{
    // Zoom around the playhead when it is visible, otherwise around the view centre.
    const double x = m_layout.frameToX(double(m_ctx.project->timeline()->playhead()));
    if (x >= kHeaderWidth && x <= viewport()->width())
        return x;
    return kHeaderWidth + m_layout.trackAreaWidth() / 2.0;
}

void TimelineWidget::zoomBy(double factor, double anchorX)
{
    setPixelsPerFrame(m_layout.pixelsPerFrame() * factor, anchorX);
}

void TimelineWidget::setPixelsPerFrame(double pixelsPerFrame, double anchorX)
{
    m_layout.setPixelsPerFrame(pixelsPerFrame, anchorX);
    updateScrollBars();
    viewport()->update();
}

void TimelineWidget::zoomIn()
{
    zoomBy(kZoomStep, zoomAnchorX());
}

void TimelineWidget::zoomOut()
{
    zoomBy(1.0 / kZoomStep, zoomAnchorX());
}

void TimelineWidget::zoomToFit()
{
    m_layout.setViewSize(viewport()->size());
    const int64_t duration = m_ctx.project->timeline()->data().duration();
    m_layout.setPixelsPerFrame(
        m_layout.fitPixelsPerFrame(duration > 0 ? duration : kEmptyTimelineFrames), kHeaderWidth);
    m_layout.setScrollX(0);
    updateScrollBars();
    viewport()->update();
}

// ---- Context menus ----

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (m_drag.mode != DragMode::None)
        return;
    const TimelineData& data = m_ctx.project->timeline()->data();
    const TimelineHit hit = m_layout.hitTest(data, event->pos());
    ActionRegistry* reg = m_ctx.actions;
    auto action = [reg](const char* id) { return reg ? reg->action(QLatin1String(id)) : nullptr; };
    QMenu menu(this);
    auto addAction = [&](const char* id) {
        if (QAction* a = action(id))
            menu.addAction(a);
    };

    switch (hit.area) {
    case TimelineHit::Area::Header:
    case TimelineHit::Area::HeaderResize: {
        const TrackRef ref = hit.track;
        const TrackData* track = data.track(ref);
        menu.addAction(tr("Rename Track…"), this, [this, ref, track] {
            bool ok = false;
            const QString name = QInputDialog::getText(
                this, tr("Rename Track"), tr("Name:"), QLineEdit::Normal, track->name, &ok);
            if (ok)
                m_controller->renameTrack(ref, name.trimmed());
        });
        menu.addAction(track->locked ? tr("Unlock Track") : tr("Lock Track"), this,
            [this, ref] { m_controller->toggleTrackFlag(ref, TimelineController::FlagLocked); });
        menu.addSeparator();
        menu.addAction(tr("Add Track Above"), this,
            [this, ref] { m_controller->addTrack(ref.type, ref.index + 1); });
        menu.addAction(tr("Add Track Below"), this,
            [this, ref] { m_controller->addTrack(ref.type, ref.index); });
        addAction("timeline.add_video_track");
        addAction("timeline.add_audio_track");
        menu.addSeparator();
        QAction* del = menu.addAction(
            tr("Delete Track"), this, [this, ref] { m_controller->removeTrack(ref); });
        del->setEnabled(!track->locked);
        break;
    }
    case TimelineHit::Area::Clip: {
        if (!m_selected.contains(hit.clipId))
            m_ctx.selection->setClips(m_controller->withLinks({hit.clipId}));
        addAction("edit.cut");
        addAction("edit.copy");
        addAction("edit.paste");
        menu.addSeparator();
        addAction("edit.split");
        addAction("clip.toggle_enabled");
        addAction("timeline.add_transition");
        menu.addSeparator();
        addAction("clip.link");
        addAction("clip.unlink");
        menu.addSeparator();
        const quint64 id = hit.clipId;
        const ClipData* clip = editops::clipById(data, id);
        const TrackData* clipTrack = data.track(hit.track);
        const bool editable = clipTrack && !clipTrack->locked;
        if (editable && clip && (clip->transitionIn || clip->transitionOut)) {
            menu.addAction(tr("Remove Transitions"), this, [this, id] {
                m_ctx.project->beginEdit(tr("Remove Transitions"));
                if (ClipData* c = editops::clipById(m_ctx.project->timeline()->mutableData(), id)) {
                    c->transitionIn.reset();
                    c->transitionOut.reset();
                }
                m_ctx.project->endEdit();
            });
        }
        // Alignment of the transitions at a cut (spec 13.5).
        if (editable && clip) {
            for (const bool head : {true, false}) {
                const auto& slot = head ? clip->transitionIn : clip->transitionOut;
                if (!slot || !editops::cutNeighbour(data, id, head))
                    continue;
                QMenu* sub = menu.addMenu(
                    head ? tr("Transition Alignment (Start)") : tr("Transition Alignment (End)"));
                const std::pair<TransitionAlignment, QString> choices[] = {
                    {TransitionAlignment::EndOnCut, tr("End on Edit")},
                    {TransitionAlignment::Centre, tr("Centre on Edit")},
                    {TransitionAlignment::StartOnCut, tr("Start on Edit")},
                };
                for (const auto& [alignment, text] : choices) {
                    QAction* a = sub->addAction(text, this, [this, id, head, alignment] {
                        m_controller->setTransitionAlignment(id, head, alignment);
                    });
                    a->setCheckable(true);
                    a->setChecked(slot->alignment == alignment);
                }
            }
        }
        if (editable && clip && !clip->effects.isEmpty()) {
            menu.addAction(tr("Remove Effects"), this, [this, id] {
                m_ctx.project->beginEdit(tr("Remove Effects"));
                if (ClipData* c = editops::clipById(m_ctx.project->timeline()->mutableData(), id))
                    c->effects.clear();
                m_ctx.project->endEdit();
            });
        }
        menu.addSeparator();
        addAction("edit.delete");
        addAction("edit.ripple_delete");
        break;
    }
    case TimelineHit::Area::Track: {
        const TrackRef ref = hit.track;
        const int64_t frame = m_layout.frameAtX(event->pos().x());
        if (editops::gapAt(data, ref, frame)) {
            menu.addAction(tr("Close Gap"), this,
                [this, ref, frame] { m_controller->closeGapAt(ref, frame); });
            menu.addSeparator();
        }
        addAction("edit.paste");
        addAction("edit.select_all");
        menu.addSeparator();
        addAction("timeline.add_video_track");
        addAction("timeline.add_audio_track");
        break;
    }
    case TimelineHit::Area::Ruler: {
        const int64_t frame = m_layout.frameAtX(event->pos().x());
        menu.addAction(tr("Add Marker Here"), this, [this, frame] {
            m_ctx.project->timeline()->setPlayhead(frame);
            m_controller->addMarker();
        });
        // A marker within a few pixels can be deleted.
        int markerIndex = -1;
        for (int i = 0; i < data.markers.size(); ++i) {
            if (std::abs(m_layout.frameToX(double(data.markers[i].frame)) - event->pos().x()) < 6)
                markerIndex = i;
        }
        if (markerIndex >= 0) {
            menu.addAction(tr("Delete Marker"), this, [this, markerIndex] {
                m_ctx.project->beginEdit(tr("Delete Marker"));
                auto& markers = m_ctx.project->timeline()->mutableData().markers;
                if (markerIndex < markers.size())
                    markers.removeAt(markerIndex);
                m_ctx.project->endEdit();
            });
        }
        menu.addSeparator();
        addAction("mark.in");
        addAction("mark.out");
        addAction("mark.clear_both");
        break;
    }
    case TimelineHit::Area::None:
        return;
    }
    if (!menu.isEmpty())
        menu.exec(event->globalPos());
}

} // namespace kk
