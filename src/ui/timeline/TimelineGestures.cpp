// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineWidget gestures added for Resolve parity: dragging the outer edges of a transition
// (spec 13.9), Ctrl+Shift swap / shuffle drag (spec 21.6) and the ruler marker editor (18.5).
// The drag lifecycle (start / update from the origin / finish) is the one of TimelineInput.cpp.
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kk {

using namespace timelinemetrics;
namespace ops = editops;

namespace {
constexpr double kTransitionGrabPx = 4.0; ///< distance at which a transition edge is grabbed
constexpr double kMarkerGrabPx = 6.0; ///< ruler marker hit distance
} // namespace

std::optional<TimelineWidget::TransitionEdgeHit> TimelineWidget::transitionEdgeAt(QPointF pos) const
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const TimelineHit hit = m_layout.hitTest(data, pos);
    if (hit.area != TimelineHit::Area::Clip && hit.area != TimelineHit::Area::Track)
        return std::nullopt;
    const TrackData* track = data.track(hit.track);
    if (!track || track->clips.empty())
        return std::nullopt;
    // A transition lies within its clip and the neighbour at the cut, so the clips around the
    // pointer (one binary search) are the only candidates.
    const auto& clips = track->clips;
    const auto it = std::upper_bound(clips.cbegin(), clips.cend(), hit.frame,
        [](double f, const ClipData& c) { return f < double(c.start); });
    const size_t pivot = size_t(it - clips.cbegin());
    const size_t from = pivot >= 2 ? pivot - 2 : 0;
    const size_t to = std::min(clips.size(), pivot + 2);
    for (size_t i = from; i < to; ++i) {
        const ClipData& c = clips[i];
        for (const bool head : {true, false}) {
            const auto window = ops::transitionWindow(data, c.id, head);
            if (!window)
                continue;
            // The anchor (the cut, or the clip edge of a fade) belongs to trimming.
            const int64_t anchor = head ? c.start : c.end();
            for (const bool left : {true, false}) {
                const int64_t edge = left ? window->first : window->second;
                if (edge == anchor)
                    continue;
                if (std::abs(m_layout.frameToX(double(edge)) - pos.x()) <= kTransitionGrabPx)
                    return TransitionEdgeHit {c.id, head, left};
            }
        }
    }
    return std::nullopt;
}

std::optional<TransitionRef> TimelineWidget::transitionAt(QPointF pos) const
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const TimelineHit hit = m_layout.hitTest(data, pos);
    if (hit.area != TimelineHit::Area::Clip && hit.area != TimelineHit::Area::Track)
        return std::nullopt;
    const TrackData* track = data.track(hit.track);
    if (!track || track->clips.empty())
        return std::nullopt;
    // The block is drawn inset by 3 px in the row (TimelinePainter::paintTransitions).
    const QRect row = m_layout.rowRect(m_layout.rowOf(hit.track));
    if (pos.y() < row.top() + 3 || pos.y() > row.bottom() - 3)
        return std::nullopt;
    const auto& clips = track->clips;
    const auto it = std::upper_bound(clips.cbegin(), clips.cend(), hit.frame,
        [](double f, const ClipData& c) { return f < double(c.start); });
    const size_t pivot = size_t(it - clips.cbegin());
    const size_t from = pivot >= 2 ? pivot - 2 : 0;
    const size_t to = std::min(clips.size(), pivot + 2);
    for (size_t i = from; i < to; ++i) {
        for (const bool head : {true, false}) {
            const auto window = ops::transitionWindow(data, clips[i].id, head);
            if (!window)
                continue;
            const double x0 = m_layout.frameToX(double(window->first));
            const double x1 = m_layout.frameToX(double(window->second));
            if (x1 - x0 >= 3 && pos.x() >= x0 && pos.x() < x1)
                return TransitionRef {clips[i].id, head};
        }
    }
    return std::nullopt;
}

quint64 TimelineWidget::fxBadgeAt(QPointF pos) const
{
    const TimelineData& data = m_ctx.project->timeline()->data();
    const TimelineHit hit = m_layout.hitTest(data, pos);
    if (hit.area != TimelineHit::Area::Clip)
        return 0;
    const ClipData* clip = ops::clipById(data, hit.clipId);
    if (!clip || clip->effects.isEmpty())
        return 0;
    const QRectF badge = TimelinePainter::fxBadgeRect(
        m_layout.clipRect(hit.row, *clip), double(m_layout.viewSize().width()));
    return badge.contains(pos) ? clip->id : 0;
}

void TimelineWidget::updateTransitionEdge(QPointF pos)
{
    restoreOrigin();
    TimelineData& data = m_ctx.project->timeline()->mutableData();
    const TransitionEdgeHit& t = m_drag.transition;
    const ClipData* clip = ops::clipById(m_drag.origin, t.clipId);
    if (!clip)
        return;
    const auto& slot = t.head ? clip->transitionIn : clip->transitionOut;
    if (!slot)
        return;
    const int64_t frame = m_layout.nearestFrameAtX(pos.x());
    const int64_t anchor = t.head ? clip->start : clip->end();
    const bool atCut = ops::cutNeighbour(m_drag.origin, t.clipId, t.head) != 0;
    // New length from the dragged edge: centred transitions grow symmetrically around the
    // cut; a transition starting / ending on the cut (or a fade) only on the dragged side.
    int64_t frames = 0;
    if (!atCut)
        frames = t.head ? frame - clip->start : clip->end() - frame; // a fade
    else if (slot->alignment == TransitionAlignment::Centre)
        frames = 2 * std::abs(frame - anchor);
    else if (t.leftEdge)
        frames = anchor - frame; // ends on the cut: [cut - d, cut)
    else
        frames = frame - anchor; // starts on the cut: [cut, cut + d)
    // Media handles limit the length (no repeated frames), but never below the length the
    // transition already has.
    const int64_t handles
        = ops::maxTransitionDuration(m_drag.origin, t.clipId, t.head, slot->alignment, true);
    frames = std::clamp<int64_t>(frames, 1, std::max(handles, slot->duration));
    ops::setTransitionDuration(data, t.clipId, t.head, frames);
    m_ctx.project->timeline()->notifyChanged();
}

void TimelineWidget::updateShuffle(QPointF pos)
{
    restoreOrigin();
    TimelineData& data = m_ctx.project->timeline()->mutableData();
    const auto loc = ops::findClip(m_drag.origin, m_drag.primary);
    if (!loc)
        return;
    const TrackData& track = *m_drag.origin.track(loc->track);
    const int64_t duration = m_drag.primaryEnd - m_drag.primaryStart;
    const int64_t delta
        = m_layout.nearestFrameAtX(pos.x()) - m_layout.nearestFrameAtX(m_drag.pressPos.x());
    // Proposed start in the coordinates of the track without the clip (later clips close up).
    int64_t proposed = m_drag.primaryStart + delta;
    if (proposed >= m_drag.primaryEnd)
        proposed -= duration;
    else if (proposed > m_drag.primaryStart)
        proposed = m_drag.primaryStart;
    // The clip lands on the nearest edit point of its track (that is what makes it a
    // shuffle, not an overwrite).
    int64_t target = 0;
    int64_t best = std::abs(proposed);
    for (const ClipData& c : track.clips) {
        if (c.id == m_drag.primary)
            continue;
        const int64_t shift = c.start >= m_drag.primaryEnd ? duration : 0;
        for (const int64_t edge : {c.start - shift, c.end() - shift}) {
            if (std::abs(edge - proposed) < best) {
                best = std::abs(edge - proposed);
                target = edge;
            }
        }
    }
    if (!ops::shuffleClip(data, m_drag.primary, target)) {
        m_ctx.project->timeline()->notifyChanged();
        return;
    }
    // Linked parts that start with the clip move to the same frame on their tracks.
    for (quint64 id : std::as_const(m_drag.clips)) {
        const ClipData* other = ops::clipById(m_drag.origin, id);
        if (id != m_drag.primary && other && other->start == m_drag.primaryStart)
            ops::shuffleClip(data, id, target);
    }
    m_ctx.project->timeline()->notifyChanged();
}

bool TimelineWidget::editMarkerNear(QPointF pos)
{
    if (pos.y() >= kRulerHeight || pos.x() < kHeaderWidth)
        return false;
    const TimelineData& data = m_ctx.project->timeline()->data();
    int64_t frame = -1;
    double best = kMarkerGrabPx;
    for (const Marker& m : data.markers) {
        const double d = std::abs(m_layout.frameToX(double(m.frame)) - pos.x());
        if (d <= best) {
            best = d;
            frame = m.frame;
        }
    }
    return frame >= 0 && (m_controller->editTimelineMarker(frame), true);
}

} // namespace kk
