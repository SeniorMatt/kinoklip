// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineWidget input state machine: press -> (threshold) -> drag mode -> release / Esc.
//
// Every editing drag follows the same pattern:
//   startDrag():  copy the timeline into Drag::origin, Project::beginEdit()
//   updateDrag(): restore origin, re-apply the operation with the total delta, notifyChanged()
//   finishDrag(): Project::endEdit()  (one undo step)   |   cancelDrag(): Project::cancelEdit()
// Re-applying from the origin keeps overwrite semantics exact (moving a clip back and forth
// over another one does not chew it up) at the cost of one TimelineData copy per mouse move,
// which is cheap compared with a repaint (a few hundred microseconds for 2,000 clips).
#include "core/EditBehaviour.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/UiAnimation.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>

namespace kk {

using namespace timelinemetrics;
namespace ops = editops;

namespace {
constexpr int kSnapPx = 8; ///< snapping distance in pixels
constexpr int kTrimThresholdPx = 2; ///< trims start almost immediately (frame accuracy)
constexpr int kAutoScrollZonePx = 24; ///< near the left/right edge a drag scrolls the view
constexpr int kAutoScrollMaxPx = 40; ///< per timer tick at the very edge
constexpr int kDragOutMarginPx = 12; ///< a move dragged this far out of the view leaves it
} // namespace

// ---- Helpers ----

int64_t TimelineWidget::snapToleranceFrames() const
{
    return int64_t(kSnapPx / m_layout.pixelsPerFrame());
}

std::optional<int64_t> TimelineWidget::snapFrame(int64_t frame) const
{
    if (!m_controller->snapping())
        return std::nullopt;
    return ops::snapToNearest(m_drag.snaps, frame, snapToleranceFrames());
}

void TimelineWidget::openEdit(const QString& label)
{
    m_drag.origin = m_ctx.project->timeline()->data();
    m_ctx.project->beginEdit(label);
    m_drag.editOpen = true;
}

void TimelineWidget::restoreOrigin()
{
    m_ctx.project->timeline()->mutableData() = m_drag.origin;
}

// ---- Mouse events ----

void TimelineWidget::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);
    if (m_drag.mode != DragMode::None)
        return;
    const QPointF pos = event->position();
    // Keyframe / curve editor toggle buttons on animated clips (KeyframeLanes).
    if (event->button() == Qt::LeftButton && m_keyframeLanes->clickClipButton(pos))
        return;
    // Retime controls (Ctrl+R) win over the clip body under them.
    if (event->button() == Qt::LeftButton && pressRetime(pos))
        return;
    const TimelineData& data = m_ctx.project->timeline()->data();
    m_drag = Drag {};
    m_drag.pressPos = pos;
    m_drag.lastPos = pos;
    m_drag.modifiers = event->modifiers();
    m_drag.hit = m_layout.hitTest(data, pos);
    m_drag.scrollXAtPress = horizontalScrollBar()->value();
    m_drag.scrollYAtPress = verticalScrollBar()->value();
    const TimelineHit& hit = m_drag.hit;

    if (event->button() == Qt::MiddleButton) {
        m_drag.mode = DragMode::Pan;
        viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;

    switch (hit.area) {
    case TimelineHit::Area::Ruler:
        startDrag(DragMode::Scrub);
        return;
    case TimelineHit::Area::Header:
        headerClick(hit);
        return;
    case TimelineHit::Area::HeaderResize:
        startDrag(DragMode::ResizeTrack);
        return;
    case TimelineHit::Area::None:
        return;
    case TimelineHit::Area::Track:
    case TimelineHit::Area::Clip:
        break;
    }

    const TimelineTool tool = m_controller->tool();
    const bool onClip = hit.area == TimelineHit::Area::Clip;
    if (tool == TimelineTool::Blade) {
        if (onClip)
            bladeAt(hit);
        return;
    }
    // Outer edges of a transition change its length (spec 13.9); they win over the clip body.
    if (const auto edge = transitionEdgeAt(pos)) {
        m_drag.transition = *edge;
        m_drag.pending = DragMode::TransitionEdge;
        m_drag.mode = DragMode::Pending;
        return;
    }
    // A click on a transition block selects it (spec 13.10); the cut under it is reached
    // from the clip edges outside the block or with the transition's outer edges.
    if (const auto t = transitionAt(pos)) {
        m_controller->selectTransition(*t);
        return;
    }
    // The fx badge selects the clip and opens its effects in the Inspector.
    if (const quint64 fx = fxBadgeAt(pos); fx && !(m_drag.modifiers & Qt::ControlModifier)) {
        m_ctx.selection->setClips(clickedWithLinks(fx));
        emit inspectRequested(fx, InspectTarget::Effects);
        return;
    }
    // Fade handles, volume keys and the level line (selection tool, spec 31).
    if (onClip && tool == TimelineTool::Select && pressLevels(pos, hit))
        return;
    const EditBehaviour behaviour = EditBehaviour::fromSettings();
    if (!onClip) {
        // Empty track area: a plain click clears the selection and selects the gap under the
        // pointer, if any (spec 10.5); dragging draws a rubber band.
        const bool additive = m_drag.modifiers & (Qt::ShiftModifier | Qt::ControlModifier);
        if (!additive) {
            m_ctx.selection->clear();
            m_controller->setSelectedEdit({});
            m_controller->selectGapAt(hit.track, int64_t(std::floor(hit.frame)));
        }
        m_drag.baseSelection = m_ctx.selection->clips();
        m_drag.pending = DragMode::RubberBand;
        m_drag.mode = DragMode::Pending;
        return;
    }

    const bool edge = hit.part == ClipPart::Head || hit.part == ClipPart::Tail;
    if (edge) {
        // An edge press selects the edit point (spec 7.6) and, when dragged, trims it.
        // At a cut between two touching clips the narrow centre zone rolls (both tools; the
        // selection tool only if EditBehaviour allows it), either side trims that side.
        bool roll = false;
        if (hit.cutLeft && hit.cutRight) {
            const ClipData* left = ops::clipById(data, hit.cutLeft);
            const double cutX = left ? m_layout.frameToX(double(left->end())) : -1e9;
            const bool centre = std::abs(pos.x() - cutX) <= kRollZonePx;
            roll = centre && (tool == TimelineTool::Trim || behaviour.selectToolRollsAtCut);
        }
        if (roll) {
            m_controller->setSelectedEdit({hit.cutLeft, hit.cutRight});
            m_drag.pending = DragMode::Roll;
        } else {
            const bool head = hit.part == ClipPart::Head;
            m_controller->setSelectedEdit(
                head ? EditPointSelection {0, hit.clipId} : EditPointSelection {hit.clipId, 0});
            m_drag.pending = head ? DragMode::TrimHead : DragMode::TrimTail;
            // The Cut page's timeline ripples with the selection tool too.
            m_drag.ripple = tool == TimelineTool::Trim || m_options.rippleEdgeTrims;
        }
        m_drag.mode = DragMode::Pending;
        return;
    }

    // Ctrl+Shift drag on a clip body (selection tool): swap / shuffle insert (spec 21.6).
    // Select the clip without the Ctrl/Shift toggle.
    if (tool == TimelineTool::Select
        && (m_drag.modifiers & (Qt::ControlModifier | Qt::ShiftModifier))
            == (Qt::ControlModifier | Qt::ShiftModifier)) {
        m_ctx.selection->setClips(clickedWithLinks(hit.clipId));
        m_drag.pending = DragMode::Shuffle;
        m_drag.mode = DragMode::Pending;
        return;
    }
    clickSelect(hit, m_drag.modifiers);
    if (!m_ctx.selection->isSelected(hit.clipId))
        return; // Ctrl/Shift-click deselected it: nothing to drag
    if (tool == TimelineTool::Trim) {
        // Trim tool on a clip body: the name band slides, the rest slips (spec 8.6); Alt
        // swaps the two.
        bool slide = false;
        if (behaviour.trimToolSlideInNameBar) {
            if (const ClipData* c = ops::clipById(data, hit.clipId)) {
                const QRectF r = m_layout.clipRect(hit.row, *c);
                slide = pos.y() < r.top() + kClipNameBandPx;
            }
        }
        if (m_drag.modifiers & Qt::AltModifier)
            slide = !slide;
        m_drag.pending = slide ? DragMode::Slide : DragMode::Slip;
    } else {
        m_drag.pending = DragMode::Move;
    }
    m_drag.mode = DragMode::Pending;
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    if (m_drag.mode == DragMode::None) {
        updateHover(pos);
        return;
    }
    m_drag.lastPos = pos;
    if (m_drag.mode == DragMode::Pending) {
        const bool trim = m_drag.pending == DragMode::TrimHead
            || m_drag.pending == DragMode::TrimTail || m_drag.pending == DragMode::Roll
            || m_drag.pending == DragMode::TransitionEdge || m_drag.pending == DragMode::RetimePoint
            || m_drag.pending == DragMode::Fade || m_drag.pending == DragMode::LevelLine
            || m_drag.pending == DragMode::LevelKey;
        const int threshold = trim ? kTrimThresholdPx : QApplication::startDragDistance();
        if ((pos - m_drag.pressPos).manhattanLength() < threshold)
            return;
        startDrag(m_drag.pending);
    }
    // Stacked timelines: a clip move dragged out of the view above or below becomes a copy
    // drag towards the other timeline (spec 31.9).
    if (m_drag.mode == DragMode::Move && m_clipDragOut && !m_drag.duplicate
        && (pos.y() < -kDragOutMarginPx || pos.y() > viewport()->height() + kDragOutMarginPx)) {
        const QVector<quint64> clips = m_drag.clips;
        const int64_t grab
            = int64_t(std::floor(m_layout.xToFrame(m_drag.pressPos.x()))) - m_drag.minStart;
        cancelDrag();
        m_clipDragOut(clips, std::max<int64_t>(0, grab));
        return;
    }
    updateDrag(pos);
    // Auto-scroll while dragging near the left/right edge.
    const bool scrolls = m_drag.mode != DragMode::Pan && m_drag.mode != DragMode::ResizeTrack;
    const bool nearEdge = pos.x() > viewport()->width() - kAutoScrollZonePx
        || (pos.x() < kHeaderWidth + kAutoScrollZonePx && pos.x() > 0);
    if (scrolls && nearEdge) {
        if (!m_autoScroll.isActive())
            m_autoScroll.start();
    } else {
        m_autoScroll.stop();
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_drag.mode == DragMode::None)
        return;
    if (m_drag.mode == DragMode::Pan && event->button() != Qt::MiddleButton)
        return;
    if (m_drag.mode != DragMode::Pan && event->button() != Qt::LeftButton)
        return;
    if (m_drag.mode == DragMode::Pending) {
        // A click without drag on an already selected clip selects just that clip.
        if (m_drag.selectOnRelease)
            m_ctx.selection->setClips(clickedWithLinks(m_drag.hit.clipId));
        m_drag = Drag {};
        return;
    }
    finishDrag();
    updateHover(event->position());
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    // A double-click on a ruler marker opens the marker editor (spec 18.5); anywhere else it
    // behaves like a press.
    if (event->button() == Qt::LeftButton && m_drag.mode == DragMode::None
        && editMarkerNear(event->position()))
        return;
    // A double-click on a transition opens it in the Inspector (spec 13.10).
    if (event->button() == Qt::LeftButton && m_drag.mode == DragMode::None
        && m_controller->tool() != TimelineTool::Blade) {
        if (const auto t = transitionAt(event->position())) {
            m_controller->selectTransition(*t);
            emit inspectRequested(t->clipId, InspectTarget::Transition);
            return;
        }
        // A double-click on the body of a compound clip / nested timeline opens it in a tab,
        // with its playhead at the moment under the pointer (spec 26.14).
        if (openNestedAt(event->position()))
            return;
    }
    mousePressEvent(event);
}

bool TimelineWidget::openNestedAt(QPointF pos)
{
    Project* project = m_ctx.project;
    const TimelineData& data = project->timeline()->data();
    const TimelineHit hit = m_layout.hitTest(data, pos);
    if (hit.area != TimelineHit::Area::Clip || hit.part != ClipPart::Body || hit.clipId == 0)
        return false;
    const ClipData* clip = ops::clipById(data, hit.clipId);
    if (!clip || clip->kind != ClipKind::Nested)
        return false;
    const auto nested = project->timelineData(clip->nestedUid);
    if (!nested)
        return false; // offline: the menu command reports it
    const int64_t frame
        = std::clamp(m_layout.frameAtX(pos.x()), clip->start, clip->start + clip->duration - 1);
    const int64_t inner = std::max<int64_t>(0, nestedFrameAt(*clip, frame, data.fps, nested->fps));
    const quint64 uid = clip->nestedUid;
    m_ctx.selection->setClips({hit.clipId});
    if (!project->openTab(uid))
        return false;
    project->setTimelinePlayhead(uid, inner);
    return true;
}

void TimelineWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && m_drag.mode != DragMode::None) {
        cancelDrag();
        event->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void TimelineWidget::leaveEvent(QEvent* event)
{
    if (m_hoverClip) {
        m_hoverClip = 0;
        viewport()->update();
    }
    if (m_overlay.bladeFrame) {
        m_overlay.bladeFrame.reset();
        viewport()->update();
    }
    QAbstractScrollArea::leaveEvent(event);
}

// ---- Drag lifecycle ----

void TimelineWidget::startDrag(DragMode mode)
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const TimelineHit& hit = m_drag.hit;
    m_drag.mode = mode;
    m_drag.selectOnRelease = false;
    const int64_t playhead = m_ctx.project->timeline()->playhead();

    switch (mode) {
    case DragMode::Scrub:
        m_drag.snaps = ops::snapCandidates(data, -1); // edges, marks; not the playhead itself
        scrubTo(m_drag.pressPos);
        return;
    case DragMode::RubberBand:
    case DragMode::Pan:
    case DragMode::None:
    case DragMode::Pending:
        return;
    case DragMode::ResizeTrack:
        m_drag.originHeight = TimelineLayout::trackHeight(*data.track(hit.track));
        openEdit(tr("Resize Track"));
        return;
    case DragMode::TransitionEdge:
        openEdit(tr("Change Transition Length"));
        return;
    case DragMode::RetimePoint:
        openEdit(m_drag.retime.kind == retimebar::Hit::Kind::PointTop ? tr("Move Speed Point")
                                                                      : tr("Change Speed"));
        return;
    case DragMode::Fade:
        openEdit(m_drag.fadeHead ? tr("Fade In") : tr("Fade Out"));
        return;
    case DragMode::LevelLine:
        openEdit(tr("Change Volume"));
        return;
    case DragMode::LevelKey:
        openEdit(tr("Move Keyframe"));
        return;
    case DragMode::Shuffle:
        m_drag.primary = hit.clipId;
        m_drag.clips = m_controller->withLinks({hit.clipId});
        openEdit(tr("Swap Edit"));
        break;
    case DragMode::Move: {
        // Alt-drag: the clicked side only (linked partners are not expanded, spec 7.3).
        m_drag.clips = (m_drag.modifiers & Qt::AltModifier) ? m_ctx.selection->clips()
                                                            : m_controller->effectiveSelection();
        if (!m_drag.clips.contains(hit.clipId))
            m_drag.clips.prepend(hit.clipId);
        m_drag.primary = hit.clipId;
        m_drag.duplicate = m_drag.modifiers & Qt::AltModifier;
        openEdit(m_drag.duplicate ? tr("Duplicate Clips") : tr("Move Clips"));
        if (m_drag.duplicate)
            m_drag.board = ops::copyClips(m_drag.origin, m_drag.clips);
        m_drag.minStart = std::numeric_limits<int64_t>::max();
        for (quint64 id : m_drag.clips) {
            if (const ClipData* c = ops::clipById(m_drag.origin, id))
                m_drag.minStart = std::min(m_drag.minStart, c->start);
        }
        break;
    }
    case DragMode::TrimHead:
    case DragMode::TrimTail: {
        m_drag.primary = hit.clipId;
        m_drag.clips = {hit.clipId};
        const ClipData* clip = ops::clipById(data, hit.clipId);
        // Linked clips whose same edge sits on the same frame trim along.
        if (m_controller->linkedSelection() && clip) {
            const int64_t edge = mode == DragMode::TrimHead ? clip->start : clip->end();
            for (quint64 id : ops::linkedClips(data, hit.clipId)) {
                const ClipData* other = ops::clipById(data, id);
                if (id != hit.clipId && other
                    && (mode == DragMode::TrimHead ? other->start : other->end()) == edge)
                    m_drag.clips.append(id);
            }
        }
        openEdit(m_drag.ripple ? tr("Ripple Trim") : tr("Trim Clip"));
        break;
    }
    case DragMode::Roll: {
        m_drag.primary = hit.cutLeft;
        m_drag.rolls = {{hit.cutLeft, hit.cutRight}};
        const ClipData* left = ops::clipById(data, hit.cutLeft);
        // Linked pairs cut at the same frame on other tracks roll along.
        if (m_controller->linkedSelection() && left) {
            const int64_t cut = left->end();
            const QVector<quint64> lefts = ops::linkedClips(data, hit.cutLeft);
            const QVector<quint64> rights = ops::linkedClips(data, hit.cutRight);
            for (quint64 l : lefts) {
                if (l == hit.cutLeft)
                    continue;
                const auto loc = ops::findClip(data, l);
                const auto& clips = data.track(loc->track)->clips;
                const size_t next = size_t(loc->clipIndex) + 1;
                if (next < clips.size() && clips[next].start == cut
                    && clips[size_t(loc->clipIndex)].end() == cut
                    && rights.contains(clips[next].id))
                    m_drag.rolls.append({l, clips[next].id});
            }
        }
        openEdit(tr("Roll Edit"));
        break;
    }
    case DragMode::Slip:
    case DragMode::Slide:
        m_drag.primary = hit.clipId;
        m_drag.clips = m_controller->withLinks({hit.clipId});
        openEdit(mode == DragMode::Slip ? tr("Slip Clip") : tr("Slide Clip"));
        break;
    }

    if (const auto loc = ops::findClip(m_drag.origin, m_drag.primary)) {
        const ClipData& c = m_drag.origin.track(loc->track)->clips[size_t(loc->clipIndex)];
        m_drag.primaryTrack = loc->track;
        m_drag.primaryStart = c.start;
        m_drag.primaryEnd = c.end();
    }
    // Snap to everything except the clips that move with the gesture.
    QVector<quint64> exclude = m_drag.clips;
    for (const auto& [l, r] : m_drag.rolls) {
        exclude.append(l);
        exclude.append(r);
    }
    m_drag.snaps = ops::snapCandidates(data, playhead, exclude);
}

void TimelineWidget::updateDrag(QPointF pos)
{
    m_drag.lastPos = pos;
    switch (m_drag.mode) {
    case DragMode::Scrub:
        scrubTo(pos);
        break;
    case DragMode::RubberBand:
        updateRubberBand(pos);
        break;
    case DragMode::Move:
        updateMove(pos);
        break;
    case DragMode::TrimHead:
    case DragMode::TrimTail:
        updateTrim(pos);
        break;
    case DragMode::Roll:
        updateRoll(pos);
        break;
    case DragMode::Slip:
    case DragMode::Slide:
        updateSlipSlide(pos);
        break;
    case DragMode::ResizeTrack: {
        // The stored height is unscaled; the drag distance is in scaled view pixels.
        const int h = std::clamp(m_drag.originHeight
                + int(std::lround((pos.y() - m_drag.pressPos.y()) / m_layout.heightScale())),
            kMinTrackHeight, kMaxTrackHeight);
        if (TrackData* t = m_ctx.project->timeline()->mutableData().track(m_drag.hit.track))
            t->height = h;
        m_ctx.project->timeline()->notifyChanged();
        break;
    }
    case DragMode::TransitionEdge:
        updateTransitionEdge(pos);
        break;
    case DragMode::RetimePoint:
        updateRetimeDrag(pos);
        break;
    case DragMode::Shuffle:
        updateShuffle(pos);
        break;
    case DragMode::Fade:
    case DragMode::LevelLine:
    case DragMode::LevelKey:
        updateLevelDrag(pos);
        break;
    case DragMode::Pan:
        horizontalScrollBar()->setValue(m_drag.scrollXAtPress - int(pos.x() - m_drag.pressPos.x()));
        verticalScrollBar()->setValue(m_drag.scrollYAtPress - int(pos.y() - m_drag.pressPos.y()));
        break;
    case DragMode::None:
    case DragMode::Pending:
        break;
    }
    viewport()->update();
}

void TimelineWidget::finishDrag()
{
    m_autoScroll.stop();
    const DragMode mode = m_drag.mode;
    const bool edited = m_drag.editOpen;
    if (m_drag.editOpen) {
        m_drag.editOpen = false;
        m_ctx.project->endEdit();
    }
    // Animations: the trimmed / rolled edges flash once on release (the drag itself was 1:1).
    if (edited && uianim::enabled()
        && (mode == DragMode::TrimHead || mode == DragMode::TrimTail || mode == DragMode::Roll)) {
        const TimelineData& data = m_ctx.project->timeline()->data();
        auto flash = [&](quint64 id, bool head) {
            if (const auto loc = editops::findClip(data, id)) {
                const ClipData& c = data.track(loc->track)->clips[size_t(loc->clipIndex)];
                m_anim.flashEdge(loc->track, head ? c.start : c.end());
            }
        };
        if (mode == DragMode::Roll) {
            for (const auto& [left, right] : m_drag.rolls)
                flash(left, false);
        } else {
            for (const quint64 id : m_drag.clips)
                flash(id, mode == DragMode::TrimHead);
        }
    }
    if (mode == DragMode::Move && m_drag.duplicate && !m_drag.resultIds.isEmpty())
        m_ctx.selection->setClips(m_drag.resultIds);
    if (mode == DragMode::Pan)
        viewport()->unsetCursor();
    m_drag = Drag {};
    m_overlay.snapFrame.reset();
    m_overlay.rubberBand = QRect();
    if (mode == DragMode::Scrub && m_options.centredPlayhead)
        centreOnPlayhead(); // the view stayed put under the mouse while scrubbing
    syncAnimations();
    viewport()->update();
}

void TimelineWidget::cancelDrag()
{
    m_autoScroll.stop();
    if (m_drag.editOpen) {
        m_drag.editOpen = false;
        m_ctx.project->cancelEdit();
    }
    if (m_drag.mode == DragMode::RubberBand)
        m_ctx.selection->setClips(m_drag.baseSelection);
    m_drag = Drag {};
    m_overlay.snapFrame.reset();
    m_overlay.rubberBand = QRect();
    syncAnimations();
    viewport()->unsetCursor();
    viewport()->update();
}

void TimelineWidget::autoScrollTick()
{
    if (m_drag.mode == DragMode::None || m_drag.mode == DragMode::Pending) {
        m_autoScroll.stop();
        return;
    }
    const double x = m_drag.lastPos.x();
    int step = 0;
    if (x > viewport()->width() - kAutoScrollZonePx)
        step = int(kAutoScrollMaxPx
                   * std::min(
                       1.0, (x - (viewport()->width() - kAutoScrollZonePx)) / kAutoScrollZonePx))
            + 1;
    else if (x < kHeaderWidth + kAutoScrollZonePx)
        step = -(int(kAutoScrollMaxPx
                     * std::min(1.0, (kHeaderWidth + kAutoScrollZonePx - x) / kAutoScrollZonePx))
            + 1);
    if (step == 0) {
        m_autoScroll.stop();
        return;
    }
    QScrollBar* h = horizontalScrollBar();
    if (step > 0 && h->value() + step > h->maximum())
        h->setMaximum(h->value() + step); // dragging past the end extends the timeline
    h->setValue(h->value() + step); // scrollContentsBy() re-runs updateDrag()
}

// ---- Individual gestures ----

void TimelineWidget::scrubTo(QPointF pos)
{
    int64_t frame = m_layout.frameAtX(std::max<double>(pos.x(), kHeaderWidth));
    m_overlay.snapFrame.reset();
    if (const auto s = snapFrame(frame)) {
        frame = *s;
        m_overlay.snapFrame = *s;
    }
    m_ctx.project->timeline()->setPlayhead(frame);
}

void TimelineWidget::updateMove(QPointF pos)
{
    const TimelineData& origin = m_drag.origin;
    int64_t dx = std::llround(m_layout.xToFrame(pos.x()) - m_layout.xToFrame(m_drag.pressPos.x()));
    // Snap whichever edge of the grabbed clip is closer to a candidate.
    m_overlay.snapFrame.reset();
    if (m_controller->snapping()) {
        const auto s1 = snapFrame(m_drag.primaryStart + dx);
        const auto s2 = snapFrame(m_drag.primaryEnd + dx);
        const int64_t d1 = s1 ? std::abs(*s1 - (m_drag.primaryStart + dx)) : INT64_MAX;
        const int64_t d2 = s2 ? std::abs(*s2 - (m_drag.primaryEnd + dx)) : INT64_MAX;
        if (s1 && d1 <= d2) {
            dx = *s1 - m_drag.primaryStart;
            m_overlay.snapFrame = *s1;
        } else if (s2) {
            dx = *s2 - m_drag.primaryEnd;
            m_overlay.snapFrame = *s2;
        }
    }
    dx = std::max(dx, -m_drag.minStart);

    // Vertical: only over a row of the grabbed clip's type; elsewhere keep the last value.
    // Past the outermost track of that type (above the top video track, below the bottom
    // audio track) a new track is created for the clips, like Resolve (spec 21.8).
    const TrackType type = m_drag.primaryTrack.type;
    const int row = m_layout.rowAtY(pos.y());
    if (row >= 0 && m_layout.rows()[size_t(row)].ref.type == type)
        m_drag.lastDy = m_layout.rows()[size_t(row)].ref.index - m_drag.primaryTrack.index;
    else if (beyondOutermostTrack(type, pos.y()))
        m_drag.lastDy = INT_MAX / 2; // capped to one new track below
    int dy = m_drag.lastDy;

    // Highest track index of the dragged clips per type, in the origin.
    int maxSame = -1;
    int maxOther = -1;
    for (quint64 id : m_drag.clips)
        if (const auto loc = ops::findClip(origin, id))
            (loc->track.type == type ? maxSame : maxOther)
                = std::max(loc->track.type == type ? maxSame : maxOther, loc->track.index);
    const auto trackCount = [&](TrackType t) {
        return int(t == TrackType::Video ? origin.videoTracks.size()
                : t == TrackType::Audio  ? origin.audioTracks.size()
                                         : origin.subtitleTracks.size());
    };
    const TrackType otherType = type == TrackType::Video ? TrackType::Audio : TrackType::Video;
    if (type != TrackType::Subtitle && maxSame >= 0)
        dy = std::min(dy, trackCount(type) - maxSame); // at most one new track at a time

    restoreOrigin();
    TimelineData& data = m_ctx.project->timeline()->mutableData();
    if (type != TrackType::Subtitle && maxSame >= 0 && maxSame + dy >= trackCount(type)) {
        for (int n = trackCount(type); n <= maxSame + dy; ++n)
            ops::addTrack(data, type);
        // Linked clips of the other type get a new track too when they need one, so the new
        // V3 pairs with A3 as for drops into the new-track area.
        if (maxOther >= 0)
            for (int n = trackCount(otherType); n <= maxOther + dy; ++n)
                ops::addTrack(data, otherType);
    }
    if (m_drag.duplicate) {
        // Linked halves move by the same track offset (V1->V2 takes A1->A2), like Resolve.
        const int vBase = m_drag.board.baseVideoTrack + dy;
        const int aBase = m_drag.board.baseAudioTrack + dy;
        m_drag.resultIds = vBase >= 0 && aBase >= 0
            ? ops::pasteClips(data, m_drag.board, m_drag.minStart + dx, vBase, aBase)
            : QVector<quint64> {};
    } else {
        // Like Resolve, linked clips of the other type move by the same track offset: dragging
        // a video clip from V1 to V2 takes its audio from A1 to A2 (away from the V/A divider).
        // Each type is clamped to its own existing tracks by moveClips.
        QVector<quint64> same;
        QVector<quint64> other;
        for (quint64 id : m_drag.clips) {
            const auto loc = ops::findClip(origin, id);
            if (loc)
                (loc->track.type == m_drag.primaryTrack.type ? same : other).append(id);
        }
        const bool movedSame = ops::moveClips(data, same, dx, dy);
        // If the other type can't follow vertically (no such track, or it is locked), it keeps
        // its tracks and only follows in time; a false result means "unchanged".
        const bool movedOther = other.isEmpty() || ops::moveClips(data, other, dx, dy)
            || ops::moveClips(data, other, dx, 0) || dx == 0;
        if (!movedOther && movedSame)
            restoreOrigin(); // one half was blocked (locked track): keep V/A in sync
    }
    m_ctx.project->timeline()->notifyChanged();
}

bool TimelineWidget::beyondOutermostTrack(TrackType type, double y) const
{
    if (type == TrackType::Subtitle)
        return false;
    // The outermost row of the type: the top video track (highest index, drawn highest) or
    // the bottom audio track (highest index, drawn lowest).
    const auto& rows = m_layout.rows();
    int outer = -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].ref.type == type
            && (outer < 0 || rows[i].ref.index > rows[size_t(outer)].ref.index))
            outer = int(i);
    if (outer < 0)
        return false;
    const QRect r = m_layout.rowRect(outer);
    return type == TrackType::Video ? y < r.top() : y > r.bottom();
}

void TimelineWidget::updateTrim(QPointF pos)
{
    const bool head = m_drag.mode == DragMode::TrimHead;
    const int64_t edge0 = head ? m_drag.primaryStart : m_drag.primaryEnd;
    int64_t target = m_layout.nearestFrameAtX(pos.x());
    m_overlay.snapFrame.reset();
    if (const auto s = snapFrame(target)) {
        target = *s;
        m_overlay.snapFrame = *s;
    }
    int64_t delta = target - edge0;
    // Apply to every clip; if one of them is limited more than the primary, redo all with the
    // smaller delta so linked edges stay together.
    for (int attempt = 0; attempt < 3; ++attempt) {
        restoreOrigin();
        TimelineData& data = m_ctx.project->timeline()->mutableData();
        const auto trim = [&](quint64 id, int64_t d) {
            return head ? ops::trimHead(data, id, d, m_drag.ripple)
                        : ops::trimTail(data, id, d, m_drag.ripple);
        };
        const int64_t applied = trim(m_drag.primary, delta);
        bool consistent = true;
        for (int i = 1; i < m_drag.clips.size(); ++i) {
            const int64_t b = trim(m_drag.clips[i], applied);
            if (b != applied) {
                delta = b;
                consistent = false;
                break;
            }
        }
        if (applied != delta && consistent)
            m_overlay.snapFrame.reset(); // clamped: the indicator would lie
        if (consistent)
            break;
    }
    m_ctx.project->timeline()->notifyChanged();
}

void TimelineWidget::updateRoll(QPointF pos)
{
    const int64_t cut0 = m_drag.primaryEnd;
    int64_t target = m_layout.nearestFrameAtX(pos.x());
    m_overlay.snapFrame.reset();
    if (const auto s = snapFrame(target)) {
        target = *s;
        m_overlay.snapFrame = *s;
    }
    int64_t delta = target - cut0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        restoreOrigin();
        TimelineData& data = m_ctx.project->timeline()->mutableData();
        const int64_t applied
            = ops::rollEdit(data, m_drag.rolls[0].first, m_drag.rolls[0].second, delta);
        bool consistent = true;
        for (int i = 1; i < m_drag.rolls.size(); ++i) {
            const int64_t b
                = ops::rollEdit(data, m_drag.rolls[i].first, m_drag.rolls[i].second, applied);
            if (b != applied) {
                delta = b;
                consistent = false;
                break;
            }
        }
        if (consistent)
            break;
    }
    m_ctx.project->timeline()->notifyChanged();
}

void TimelineWidget::updateSlipSlide(QPointF pos)
{
    const int64_t d
        = std::llround(m_layout.xToFrame(pos.x()) - m_layout.xToFrame(m_drag.pressPos.x()));
    restoreOrigin();
    TimelineData& data = m_ctx.project->timeline()->mutableData();
    if (m_drag.mode == DragMode::Slip) {
        // Dragging right reveals earlier material (the media moves with the mouse under a
        // fixed window), hence the negative delta.
        const int64_t applied = ops::slipClip(data, m_drag.primary, -d);
        for (int i = 1; i < m_drag.clips.size(); ++i)
            ops::slipClip(data, m_drag.clips[i], applied);
    } else {
        const int64_t applied = ops::slideClip(data, m_drag.primary, d);
        for (int i = 1; i < m_drag.clips.size(); ++i)
            ops::slideClip(data, m_drag.clips[i], applied);
    }
    m_ctx.project->timeline()->notifyChanged();
}

void TimelineWidget::updateRubberBand(QPointF pos)
{
    const QRect band = QRectF(m_drag.pressPos, pos).normalized().toRect();
    m_overlay.rubberBand = band.intersected(QRect(
        kHeaderWidth, kRulerHeight, viewport()->width() - kHeaderWidth, viewport()->height()));
    const TimelineData& data = m_ctx.project->timeline()->data();
    const double f0 = m_layout.xToFrame(band.left());
    const double f1 = m_layout.xToFrame(band.right());
    QVector<quint64> ids = m_drag.baseSelection;
    for (int row = 0; row < int(m_layout.rows().size()); ++row) {
        const QRect r = m_layout.rowRect(row);
        if (r.bottom() < band.top() || r.top() > band.bottom())
            continue;
        const TrackData* track = data.track(m_layout.rows()[size_t(row)].ref);
        const auto [first, last] = TimelineLayout::clipRange(*track, f0, f1);
        for (size_t i = first; i < last; ++i) {
            if (!ids.contains(track->clips[i].id))
                ids.append(track->clips[i].id);
        }
    }
    m_ctx.selection->setClips(m_controller->withLinks(ids));
}

void TimelineWidget::bladeAt(const TimelineHit& hit)
{
    int64_t frame = m_layout.nearestFrameAtX(m_layout.frameToX(hit.frame));
    // With snapping on, the blade snaps to the playhead and other edit points nearby.
    if (m_controller->snapping()) {
        const auto snaps = ops::snapCandidates(
            m_ctx.project->timeline()->data(), m_ctx.project->timeline()->playhead());
        if (const auto s = ops::snapToNearest(snaps, frame, snapToleranceFrames()))
            frame = *s;
    }
    const QVector<quint64> ids = m_controller->withLinks({hit.clipId});
    m_ctx.project->beginEdit(tr("Blade"));
    const QVector<quint64> right
        = ops::splitClips(m_ctx.project->timeline()->mutableData(), ids, frame);
    if (right.isEmpty())
        m_ctx.project->cancelEdit();
    else
        m_ctx.project->endEdit();
    m_drag = Drag {};
}

QVector<quint64> TimelineWidget::clickedWithLinks(quint64 clipId) const
{
    QVector<quint64> ids = m_controller->withLinks({clipId});
    ids.removeAll(clipId);
    ids.append(clipId); // the clicked clip is the current one (the Inspector follows it)
    return ids;
}

void TimelineWidget::clickSelect(const TimelineHit& hit, Qt::KeyboardModifiers modifiers)
{
    // Alt-click picks just the clicked clip, not its linked partners (spec 7.3).
    const QVector<quint64> ids = (modifiers & Qt::AltModifier) ? QVector<quint64> {hit.clipId}
                                                               : clickedWithLinks(hit.clipId);
    if (modifiers & (Qt::ShiftModifier | Qt::ControlModifier)) {
        if (m_ctx.selection->isSelected(hit.clipId))
            m_ctx.selection->removeClips(ids);
        else
            m_ctx.selection->addClips(ids);
    } else if (modifiers & Qt::AltModifier) {
        m_ctx.selection->setClips(ids);
    } else if (m_ctx.selection->isSelected(hit.clipId)) {
        m_drag.selectOnRelease = true; // keep the group for dragging; reduce on plain click
    } else {
        m_ctx.selection->setClips(ids);
    }
}

void TimelineWidget::headerClick(const TimelineHit& hit)
{
    switch (hit.button) {
    case HeaderButton::Target:
        m_controller->toggleTarget(hit.track);
        break;
    case HeaderButton::Lock:
        m_controller->toggleTrackFlag(hit.track, TimelineController::FlagLocked);
        break;
    case HeaderButton::Visible:
        m_controller->toggleTrackFlag(hit.track, TimelineController::FlagEnabled);
        break;
    case HeaderButton::Mute:
        m_controller->toggleTrackFlag(hit.track, TimelineController::FlagMuted);
        break;
    case HeaderButton::Solo:
        m_controller->toggleTrackFlag(hit.track, TimelineController::FlagSolo);
        break;
    case HeaderButton::AutoSelect:
        if (m_drag.modifiers & Qt::AltModifier)
            m_controller->soloAutoSelect(hit.track); // spec 6.6
        else
            m_controller->toggleTrackFlag(hit.track, TimelineController::FlagAutoSelect);
        break;
    case HeaderButton::Arm: // recording is deferred: the button is shown disabled
    case HeaderButton::None:
        break;
    }
    m_drag = Drag {};
}

void TimelineWidget::updateHover(QPointF pos)
{
    const TimelineHit hit = m_layout.hitTest(m_ctx.project->timeline()->data(), pos);
    const TimelineTool tool = m_controller->tool();
    Qt::CursorShape shape = Qt::ArrowCursor;
    std::optional<int64_t> blade;
    int bladeRow = -1;
    quint64 retimeClip = 0;
    const retimebar::Hit::Kind retimeKind = retimeHitAt(pos, &retimeClip).kind;
    if (hit.area == TimelineHit::Area::HeaderResize) {
        shape = Qt::SizeVerCursor;
    } else if (retimeKind == retimebar::Hit::Kind::PointTop
        || retimeKind == retimebar::Hit::Kind::PointBottom
        || retimeKind == retimebar::Hit::Kind::EndHandle) {
        shape = Qt::SizeHorCursor; // speed point / retime handle (spec 29.4)
    } else if (tool != TimelineTool::Blade && transitionEdgeAt(pos)) {
        shape = Qt::SizeHorCursor; // transition length (spec 13.9)
    } else if (const auto level
        = hit.area == TimelineHit::Area::Clip && tool == TimelineTool::Select
            ? levelCursor(pos, hit)
            : std::nullopt) {
        shape = *level; // fade handle / level line / volume key (spec 31)
    } else if (hit.area == TimelineHit::Area::Clip) {
        const bool edge = hit.part == ClipPart::Head || hit.part == ClipPart::Tail;
        if (tool == TimelineTool::Blade) {
            shape = Qt::IBeamCursor;
            blade = m_layout.nearestFrameAtX(pos.x());
            bladeRow = hit.row;
        } else if (edge) {
            shape
                = tool == TimelineTool::Trim && hit.cutLeft ? Qt::SplitHCursor : Qt::SizeHorCursor;
        } else if (tool == TimelineTool::Trim) {
            shape = Qt::SizeAllCursor; // slip / slide
        }
    }
    viewport()->setCursor(shape);
    const quint64 hover = hit.area == TimelineHit::Area::Clip ? hit.clipId : 0;
    if (hover != m_hoverClip) {
        m_hoverClip = hover; // fade handles follow the mouse
        viewport()->update();
    }
    if (blade != m_overlay.bladeFrame || bladeRow != m_overlay.bladeRow) {
        m_overlay.bladeFrame = blade;
        m_overlay.bladeRow = bladeRow;
        viewport()->update();
    }
}

} // namespace kk
