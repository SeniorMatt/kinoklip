// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineWidget: construction, painting, zoom, scrolling, playhead follow, context menus.
// The mouse state machine lives in TimelineInput.cpp, drag and drop in TimelineDrop.cpp.
#include "ui/timeline/TimelineWidget.h"

#include "core/EditBehaviour.h"
#include "core/Multicam.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "export/RenderCacheManager.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaManagement.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/UiAnimation.h"
#include "ui/timeline/ClipColorMenu.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/NestedPreview.h"
#include "ui/timeline/TimelineController.h"

#include <QContextMenuEvent>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QUndoStack>
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
    , m_anim([this] { viewport()->update(); })
{
    if (ctx.project)
        m_timelineLookup = ctx.project->timelineLookup();
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    if (ctx.media) {
        RenderCacheManager* cache = ctx.media->renderCache();
        auto syncCache = [this, cache] {
            m_cacheBar.clear();
            for (const RenderCacheManager::Range& r : cache->ranges())
                m_cacheBar.append(
                    {r.start, r.end, r.state == RenderCacheManager::RangeState::Cached});
            viewport()->update(0, 0, viewport()->width(), timelinemetrics::kRulerHeight);
        };
        connect(cache, &RenderCacheManager::rangesChanged, this, syncCache);
        syncCache();
    }
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
    // Compound clips / nested timelines: previews rendered from the nested timeline.
    m_nestedPreview = new NestedPreview(ctx.project, this);
    connect(m_nestedPreview, &NestedPreview::ready, this, &TimelineWidget::refreshMedia);
    m_painter.setThumbnailProvider(m_nestedPreview->wrap(mediaCacheThumbnails()));
    m_painter.setWaveformProvider(m_nestedPreview->wrap(mediaCacheWaveforms()));
    connectMediaCache(this, [this] { refreshMedia(); });

    Timeline* timeline = ctx.project->timeline();
    connect(timeline, &Timeline::changed, this, &TimelineWidget::onTimelineChanged);
    // Offline media are drawn red (the set changes on activation, relink, undo).
    connect(ctx.project->mediaPool(), &MediaPool::offlineChanged, viewport(),
        qOverload<>(&QWidget::update));
    connect(timeline, &Timeline::playheadChanged, this, &TimelineWidget::onPlayheadChanged);
    connect(ctx.selection, &SelectionModel::changed, this, &TimelineWidget::onSelectionChanged);
    connect(ctx.project, &Project::projectReset, this, [this] {
        m_anim.clear(); // a loaded project is not an edit
        onTimelineChanged();
        if (m_options.keepsViewState)
            restoreViewState(m_ctx.project->activeTimelineUid());
        else if (m_options.centredPlayhead)
            centreOnPlayhead();
    });
    // Timeline tabs: each timeline keeps its own zoom and scroll (Project::viewState()); the
    // selection and the keyframe lanes refer to clip ids of the old timeline.
    connect(ctx.project, &Project::activeTimelineAboutToChange, this, [this](quint64 uid) {
        if (m_drag.mode != DragMode::None)
            cancelDrag();
        if (m_options.keepsViewState)
            m_ctx.project->setViewState(uid,
                TimelineViewState {m_layout.pixelsPerFrame(), int(std::lround(m_layout.scrollX())),
                    m_layout.scrollY()});
        m_ctx.selection->clear();
        m_keyframeLanes->reset();
        m_controller->clearRetimeControls();
    });
    connect(ctx.project, &Project::activeTimelineChanged, this, [this](quint64 uid) {
        onTimelineChanged();
        if (m_options.keepsViewState)
            restoreViewState(uid);
        else if (m_options.centredPlayhead)
            centreOnPlayhead();
    });
    connect(m_controller, &TimelineController::retimeControlsChanged, viewport(),
        [this] { viewport()->update(); });
    connect(m_controller, &TimelineController::retimeCurveRequested, this,
        &TimelineWidget::showRetimeCurve);
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

    // Animations (v2.2): the undo stack index moves when a gesture's edit is pushed (its live
    // changes were not animated) and on undo / redo.
    connect(
        ctx.project->undoStack(), &QUndoStack::indexChanged, this, &TimelineWidget::syncAnimations);
    connect(uianim::Notifier::instance(), &uianim::Notifier::changed, this,
        &TimelineWidget::onAnimationSettingsChanged);

    m_layout.rebuild(timeline->data());
    m_lastPlayhead = timeline->playhead();
    onSelectionChanged();
    syncAnimations(); // the first edit animates from the state at construction
}

void TimelineWidget::restoreViewState(quint64 uid)
{
    const TimelineViewState v = m_ctx.project->viewState(uid);
    if (v.pixelsPerFrame <= 0.0) {
        // Never shown: fit, now or on the first show with a real width.
        m_fitOnFirstShow = true;
        zoomToFit();
        return;
    }
    m_fitOnFirstShow = false;
    m_layout.setPixelsPerFrame(v.pixelsPerFrame, kHeaderWidth);
    m_layout.setScrollX(v.scrollX);
    m_layout.setScrollY(v.scrollY);
    updateScrollBars();
    viewport()->update();
}

TimelineWidget::~TimelineWidget()
{
    // Never leave an edit open (e.g. the widget is destroyed mid-drag).
    if (m_drag.editOpen)
        m_ctx.project->cancelEdit();
}

void TimelineWidget::setThumbnailProvider(ThumbnailProvider provider)
{
    m_painter.setThumbnailProvider(m_nestedPreview->wrap(std::move(provider)));
    viewport()->update();
}

void TimelineWidget::setWaveformProvider(WaveformProvider provider)
{
    m_painter.setWaveformProvider(m_nestedPreview->wrap(std::move(provider)));
    viewport()->update();
}

void TimelineWidget::refreshMedia()
{
    viewport()->update();
}

// ---- Model notifications ----

void TimelineWidget::onTimelineChanged()
{
    // Animations: compare the committed state with the previous one while the old layout is
    // still in place (the old rows give the old clip positions). Live changes of an open edit
    // (drags, here or in another view of the same timeline) are drawn 1:1 instead.
    if (uianim::enabled()) {
        const Timeline* timeline = m_ctx.project->timeline();
        if (m_ctx.project->isEditing())
            m_anim.markStale();
        else if (m_anim.stale() || timeline->revision() != m_anim.revision()) {
            m_anim.modelChanged(timeline->plainSnapshot(), m_ctx.project->activeTimelineUid(),
                timeline->revision(), m_layout);
            // Keyboard trims of the selected edit point: its highlight pulses once.
            const auto& edit = m_controller->selectedEdit();
            if ((edit.tailClip && m_anim.isAnimating(edit.tailClip))
                || (edit.headClip && m_anim.isAnimating(edit.headClip)))
                m_anim.pulseEditPoint();
        }
    }
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

void TimelineWidget::syncAnimations()
{
    if (!uianim::enabled() || !m_anim.stale() || m_ctx.project->isEditing())
        return;
    const Timeline* timeline = m_ctx.project->timeline();
    m_anim.resync(
        timeline->plainSnapshot(), m_ctx.project->activeTimelineUid(), timeline->revision());
}

void TimelineWidget::onAnimationSettingsChanged()
{
    m_anim.clear();
    syncAnimations();
    viewport()->update();
}

void TimelineWidget::onPlayheadChanged(int64_t frame)
{
    // Animations: an isolated jump (edit point navigation, Home / End, a click on the Cut
    // page overview) glides the drawn line; playback, scrubbing and centred views do not.
    if (uianim::enabled()) {
        const bool playing = m_ctx.playback && m_ctx.playback->isPlaying();
        m_anim.playheadMoved(m_lastPlayhead, frame,
            !playing && !m_options.centredPlayhead && m_drag.mode != DragMode::Scrub,
            m_layout.pixelsPerFrame());
    }
    m_lastPlayhead = frame;
    // While the user scrubs, the view must not jump under the mouse.
    if (m_drag.mode != DragMode::Scrub && m_options.centredPlayhead)
        centreOnPlayhead();
    else if (m_drag.mode != DragMode::Scrub && m_followPlayhead)
        ensurePlayheadVisible();
    viewport()->update();
}

void TimelineWidget::setViewOptions(const TimelineViewOptions& options)
{
    m_options = options;
    m_layout.setAudioHeightScale(options.audioHeightScale);
    m_layout.setVideoHeightScale(options.videoHeightScale);
    m_layout.setAudioPageHeaders(options.audioPageHeaders);
    m_layout.rebuild(m_ctx.project->timeline()->data());
    setHorizontalScrollBarPolicy(
        options.centredPlayhead ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAlwaysOn);
    if (options.centredPlayhead) {
        m_fitOnFirstShow = false; // the page chooses the zoom
        centreOnPlayhead();
    }
    updateScrollBars();
    viewport()->update();
}

void TimelineWidget::centreOnPlayhead()
{
    const double frame = double(m_ctx.project->timeline()->playhead());
    m_layout.setViewSize(viewport()->size());
    m_layout.setScrollX(
        std::max(0.0, frame * m_layout.pixelsPerFrame() - m_layout.trackAreaWidth() / 2.0));
    updateScrollBars();
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
    in.timelines = &m_timelineLookup;
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
    in.transition = m_ctx.selection->transition().value_or(TransitionRef {0, true});
    in.hasFocus = hasFocus();
    in.cache = &m_cacheBar;
    in.retimeShown = &m_controller->retimeControls();
    in.hoverClip = m_hoverClip;
    in.selectedKeys = &m_ctx.selection->keyframes();
    if (m_options.headerMeter)
        in.headerMeter = &m_options.headerMeter;
    in.thumbnails = m_thumbMode;
    in.waveforms = m_showWaveforms;
    in.sourceTimecode = m_showSourceTimecode;
    if (uianim::enabled()) {
        m_anim.observeSnap(m_overlay.snapFrame);
        m_anim.observeEditPoint(in.editTailClip, in.editHeadClip);
        if (m_anim.tick())
            in.anim = &m_anim;
    }
    m_painter.paint(p, in);
    m_keyframeLanes->paintClipButtons(p);
}

void TimelineWidget::setClipThumbnails(ClipThumbnails mode)
{
    if (m_thumbMode == mode)
        return;
    m_thumbMode = mode;
    viewport()->update();
}

void TimelineWidget::setShowWaveforms(bool on)
{
    if (m_showWaveforms == on)
        return;
    m_showWaveforms = on;
    viewport()->update();
}

void TimelineWidget::setShowSourceTimecode(bool on)
{
    if (m_showSourceTimecode == on)
        return;
    m_showSourceTimecode = on;
    viewport()->update();
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
    if (m_options.centredPlayhead)
        centreOnPlayhead();
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
    emit viewChanged();
}

std::pair<double, double> TimelineWidget::visibleFrameRange() const
{
    return {m_layout.xToFrame(kHeaderWidth), m_layout.xToFrame(viewport()->width())};
}

void TimelineWidget::scrollContentsBy(int, int)
{
    if (m_syncingScrollBars)
        return;
    m_layout.setScrollX(horizontalScrollBar()->value());
    m_layout.setScrollY(verticalScrollBar()->value());
    // Centred playhead: scrolling by hand (wheel) moves the playhead under the centre.
    if (m_options.centredPlayhead && m_drag.mode == DragMode::None) {
        const double centre = kHeaderWidth + m_layout.trackAreaWidth() / 2.0;
        m_ctx.project->timeline()->setPlayhead(
            std::max<int64_t>(0, int64_t(std::floor(m_layout.xToFrame(centre)))));
    }
    // A drag in progress follows the content (e.g. edge auto-scroll).
    if (m_drag.mode != DragMode::None && m_drag.mode != DragMode::Pending
        && m_drag.mode != DragMode::Pan)
        updateDrag(m_drag.lastPos);
    m_keyframeLanes->syncGeometry();
    viewport()->update();
    emit viewChanged();
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
    // One wheel notch (120 = 15 degrees) scrolls a tenth of the visible time at any width
    // (the integer step used to truncate: 120 px per notch on any view under 2400 px).
    const double timeStep = m_layout.trackAreaWidth() / 1200.0; // px per 1/8 degree
    if (resolve) {
        if (mods & Qt::AltModifier) {
            wheelZoom();
        } else if (mods & Qt::ShiftModifier) {
            setTrackHeightScale(m_layout.heightScale() * std::pow(kWheelZoomPerDegree, anyDelta));
        } else if ((mods & Qt::ControlModifier) || angle.x() != 0) {
            // Ctrl+wheel and horizontal touchpad swipes scroll time.
            h->setValue(h->value()
                - int(std::lround((angle.x() != 0 ? angle.x() : angle.y()) * timeStep)));
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
    if (m_options.centredPlayhead) {
        centreOnPlayhead();
        return;
    }
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

void TimelineWidget::transitionMenu(QMenu& menu, const TransitionRef& ref)
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const ClipData* clip = editops::clipById(data, ref.clipId);
    const auto& slot = clip ? (ref.head ? clip->transitionIn : clip->transitionOut)
                            : std::optional<TransitionData> {};
    if (!slot)
        return;
    const auto loc = editops::findClip(data, ref.clipId);
    const TrackData* track = loc ? data.track(loc->track) : nullptr;
    const bool editable = track && !track->locked;
    menu.addAction(tr("Open in Inspector"), this,
        [this, ref] { emit inspectRequested(ref.clipId, InspectTarget::Transition); });
    menu.addSeparator();
    QAction* duration = menu.addAction(tr("Change Transition Duration…"), this, [this, ref] {
        const ClipData* c = editops::clipById(m_ctx.project->timeline()->data(), ref.clipId);
        const auto& t = c ? (ref.head ? c->transitionIn : c->transitionOut)
                          : std::optional<TransitionData> {};
        if (!t)
            return;
        const int64_t limit = editops::maxTransitionDuration(
            m_ctx.project->timeline()->data(), ref.clipId, ref.head, t->alignment, false);
        bool ok = false;
        const int frames
            = QInputDialog::getInt(this, tr("Change Transition Duration"), tr("Duration (frames):"),
                int(t->duration), 1, int(std::max<int64_t>(1, limit)), 1, &ok);
        if (ok)
            m_controller->setTransitionDuration(ref.clipId, ref.head, frames);
    });
    duration->setEnabled(editable);
    if (editops::cutNeighbour(data, ref.clipId, ref.head)) {
        QMenu* sub = menu.addMenu(tr("Transition Alignment"));
        sub->setEnabled(editable);
        const std::pair<TransitionAlignment, QString> choices[] = {
            {TransitionAlignment::EndOnCut, tr("End on Edit")},
            {TransitionAlignment::Centre, tr("Centre on Edit")},
            {TransitionAlignment::StartOnCut, tr("Start on Edit")},
        };
        for (const auto& [alignment, text] : choices) {
            QAction* a = sub->addAction(text, this, [this, ref, alignment = alignment] {
                m_controller->setTransitionAlignment(ref.clipId, ref.head, alignment);
            });
            a->setCheckable(true);
            a->setChecked(slot->alignment == alignment);
        }
    }
    menu.addSeparator();
    QAction* del = menu.addAction(
        tr("Delete Transition"), this, [this, ref] { m_controller->deleteTransition(ref); });
    del->setEnabled(editable);
}

void TimelineWidget::addTrackHeightMenu(QMenu& menu, TrackRef ref, bool allTracks)
{
    using Preset = TimelineController::TrackHeightPreset;
    QMenu* sub = menu.addMenu(allTracks ? tr("Track Height (All Tracks)") : tr("Track Height"));
    sub->setObjectName(QStringLiteral("TrackHeightMenu"));
    const TrackData* track = m_ctx.project->timeline()->data().track(ref);
    const std::optional<Preset> current
        = track && !allTracks ? TimelineController::heightPresetOf(*track) : std::nullopt;
    const std::pair<Preset, QString> presets[] = {
        {Preset::Small, tr("Small")},
        {Preset::Medium, tr("Medium")},
        {Preset::Large, tr("Large")},
    };
    for (const auto& [preset, text] : presets) {
        QAction* a = sub->addAction(text, this, [this, ref, p = preset, allTracks] {
            m_controller->setTrackHeightPreset(ref, p, allTracks);
        });
        a->setCheckable(!allTracks);
        a->setChecked(current == preset);
    }
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (m_drag.mode != DragMode::None)
        return;
    // Retime controls: the speed point menu on a point, the speed menu on a segment.
    quint64 retimeClip = 0;
    const retimebar::Hit rh = retimeHitAt(event->pos(), &retimeClip);
    if (rh.kind == retimebar::Hit::Kind::PointTop || rh.kind == retimebar::Hit::Kind::PointBottom) {
        showSpeedPointMenu(retimeClip, rh.localFrame, event->pos());
        return;
    }
    if (rh.kind == retimebar::Hit::Kind::Segment) {
        showSpeedMenu(retimeClip, rh.segment, event->pos());
        return;
    }
    if (levelKeyMenu(event->pos(), event->globalPos()))
        return; // a volume key on a level line (spec 31.2)
    const TimelineData& data = m_ctx.project->timeline()->data();
    const TimelineHit hit = m_layout.hitTest(data, event->pos());
    ActionRegistry* reg = m_ctx.actions;
    auto action = [reg](const char* id) { return reg ? reg->action(QLatin1String(id)) : nullptr; };
    QMenu menu(this);
    auto addAction = [&](const char* id) {
        if (QAction* a = action(id))
            menu.addAction(a);
    };

    // A transition wins over the clip under it (spec 13.10).
    if (const auto t = transitionAt(event->pos())) {
        m_controller->selectTransition(*t);
        transitionMenu(menu, *t);
        menu.exec(event->globalPos());
        return;
    }

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
        // The header buttons as menu entries (Resolve offers them in the header menu too).
        if (ref.type == TrackType::Video)
            menu.addAction(
                track->enabled ? tr("Disable Track") : tr("Enable Track"), this, [this, ref] {
                    m_controller->toggleTrackFlag(ref, TimelineController::FlagEnabled);
                });
        if (ref.type == TrackType::Audio) {
            menu.addAction(track->muted ? tr("Unmute Track") : tr("Mute Track"), this,
                [this, ref] { m_controller->toggleTrackFlag(ref, TimelineController::FlagMuted); });
            menu.addAction(track->solo ? tr("Unsolo Track") : tr("Solo Track"), this,
                [this, ref] { m_controller->toggleTrackFlag(ref, TimelineController::FlagSolo); });
        }
        menu.addSeparator();
        // Move Track Up / Down (spec 12.9): on screen, so the outermost track cannot move out.
        const int count = int(data.tracks(ref.type).size());
        const bool screenTop
            = ref.type == TrackType::Audio ? ref.index == 0 : ref.index == count - 1;
        const bool screenBottom
            = ref.type == TrackType::Audio ? ref.index == count - 1 : ref.index == 0;
        menu.addAction(tr("Move Track Up"), this, [this, ref] { m_controller->moveTrack(ref, 1); })
            ->setEnabled(!screenTop);
        menu.addAction(
                tr("Move Track Down"), this, [this, ref] { m_controller->moveTrack(ref, -1); })
            ->setEnabled(!screenBottom);
        addTrackHeightMenu(menu, ref, false);
        menu.addSeparator();
        menu.addAction(tr("Add Track Above"), this,
            [this, ref] { m_controller->addTrack(ref.type, ref.index + 1); });
        menu.addAction(tr("Add Track Below"), this,
            [this, ref] { m_controller->addTrack(ref.type, ref.index); });
        addAction("timeline.add_video_track");
        addAction("timeline.add_audio_track");
        addAction("timeline.add_subtitle_track");
        if (ref.type == TrackType::Subtitle) {
            menu.addSeparator();
            addAction("timeline.add_subtitle");
        }
        menu.addSeparator();
        QAction* del = menu.addAction(
            tr("Delete Track"), this, [this, ref] { m_controller->removeTrack(ref); });
        del->setEnabled(!track->locked);
        addAction("timeline.delete_empty_tracks");
        break;
    }
    case TimelineHit::Area::Clip: {
        if (!m_selected.contains(hit.clipId))
            m_ctx.selection->setClips(clickedWithLinks(hit.clipId));
        addAction("edit.cut");
        addAction("edit.copy");
        addAction("edit.paste");
        menu.addSeparator();
        addAction("edit.split");
        addAction("clip.toggle_enabled");
        addAction("timeline.add_transition");
        menu.addSeparator();
        addAction("clip.change_speed");
        addAction("clip.change_duration");
        addAction("clip.retime_controls");
        addAction("clip.freeze_frame");
        addAction("clip.retime_curve");
        menu.addSeparator();
        addAction("clip.link");
        addAction("clip.unlink");
        menu.addSeparator();
        // Organisation (bins branch): clip colour, flags of the source, find its source.
        addClipColorMenus(menu, m_ctx, m_ctx.selection->clips(), this);
        addAction("timeline.find_in_media_pool");
        menu.addSeparator();
        const quint64 id = hit.clipId;
        const ClipData* clip = editops::clipById(data, id);
        // Compound clips (spec 26): create from the selection; open / decompose an instance.
        addAction("clip.new_compound");
        const auto lookup = m_ctx.project->timelineLookup();
        if (clip && multicam::isMulticamClip(*clip, lookup)) {
            // Multicam clips: angle switch per side, edit the angles, flatten.
            const TimelineData* mc = lookup(clip->nestedUid);
            QMenu* angles = menu.addMenu(tr("Switch Multicam Clip Angle"));
            angles->setObjectName(QStringLiteral("SwitchAngleMenu"));
            const multicam::MulticamParts parts = multicam::parts(data, id);
            const std::pair<multicam::AngleTarget, QString> sides[] = {
                {multicam::AngleTarget::Both, tr("Video and Audio")},
                {multicam::AngleTarget::Video, tr("Video")},
                {multicam::AngleTarget::Audio, tr("Audio")},
            };
            for (const auto& [target, text] : sides) {
                const multicam::AngleTarget side = target;
                if ((side == multicam::AngleTarget::Video && parts.video.isEmpty())
                    || (side == multicam::AngleTarget::Audio && parts.audio.isEmpty()))
                    continue;
                QMenu* sub = angles->addMenu(text);
                const quint64 shownBy
                    = side == multicam::AngleTarget::Audio && !parts.audio.isEmpty()
                    ? parts.audio.first()
                    : (!parts.video.isEmpty() ? parts.video.first() : id);
                const int current = editops::clipById(data, shownBy)->multicamAngle;
                for (int a = 0; a < multicam::angleCount(*mc); ++a) {
                    QAction* act = sub->addAction(
                        QStringLiteral("%1  %2").arg(a + 1).arg(multicam::angleName(*mc, a)), this,
                        [this, id, a, side] { m_controller->switchMulticamAngle(id, a, side); });
                    act->setCheckable(true);
                    act->setChecked(side != multicam::AngleTarget::Both && a == current);
                }
            }
            addAction("clip.open_in_timeline");
            addAction("multicam.flatten");
        } else if (clip && clip->kind == ClipKind::Nested) {
            addAction("clip.open_in_timeline");
            addAction("clip.decompose");
        }
        // Clip Attributes of the clip's media pool item (spec 31.6).
        if (clip && clip->kind == ClipKind::Media)
            addAction("clip.attributes");
        menu.addSeparator();
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
        if (ref.type == TrackType::Subtitle) {
            // Resolve: right-click in a subtitle track adds a caption there (at the playhead).
            menu.addAction(tr("Add Subtitle"), this, [this, frame] {
                m_ctx.project->timeline()->setPlayhead(frame);
                m_controller->addSubtitleAtPlayhead();
            });
            menu.addSeparator();
        }
        addAction("timeline.add_video_track");
        addAction("timeline.add_audio_track");
        addAction("timeline.add_subtitle_track");
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
            // Name, colour and note (the marker editor, as a double-click on it).
            const int64_t markerFrame = data.markers[markerIndex].frame;
            menu.addAction(tr("Modify Marker…"), this,
                [this, markerFrame] { m_controller->editTimelineMarker(markerFrame); });
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
