// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController, part 3: the default transition (Ctrl+T) with the insufficient-handles
// question, transition alignment and length (spec 13.1-13.9).
#include "core/EditBehaviour.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kk {

namespace ops = editops;
using timelineedit::runEdit;

namespace {

/// Default transition id (Ctrl+T); its length comes from EditBehaviour.
const QString kDefaultTransition = QStringLiteral("cross_dissolve");

/// Ripple-trims clip @p clipId (and, with @p linked, linked clips whose same edge is on the
/// same frame) by @p delta frames at its head or tail.
void rippleTrimWithLinks(TimelineData& data, quint64 clipId, bool head, int64_t delta, bool linked)
{
    const ClipData* clip = ops::clipById(data, clipId);
    if (!clip || delta == 0)
        return;
    const int64_t edge = head ? clip->start : clip->end();
    QVector<quint64> ids {clipId};
    if (linked) {
        for (quint64 id : ops::linkedClips(data, clipId)) {
            const ClipData* other = ops::clipById(data, id);
            if (id != clipId && other && (head ? other->start : other->end()) == edge)
                ids.append(id);
        }
    }
    for (quint64 id : ids) {
        if (head)
            ops::trimHead(data, id, delta, true);
        else
            ops::trimTail(data, id, delta, true);
    }
}

} // namespace

void TimelineController::addDefaultTransition()
{
    const TimelineData& d = timeline()->data();
    const int64_t playhead = timeline()->playhead();

    // Edges that get a transition: (clip id, head?).
    QVector<std::pair<quint64, bool>> edges;
    const QVector<quint64> selected = effectiveSelection();
    if (!m_selectedEdit.isEmpty()) {
        // The selected edit point (spec 13.2).
        if (m_selectedEdit.headClip)
            edges.append({m_selectedEdit.headClip, true});
        else
            edges.append({m_selectedEdit.tailClip, false});
    } else if (!selected.isEmpty()) {
        // Both ends of every selected clip (spec 13.2).
        for (quint64 id : selected) {
            edges.append({id, true});
            edges.append({id, false});
        }
    } else {
        // The edge closest to the playhead on the auto-select video tracks (spec 13.2).
        quint64 bestId = 0;
        bool bestHead = true;
        int64_t bestDistance = std::numeric_limits<int64_t>::max();
        const ops::TrackFilter accept = ops::autoSelectTracks();
        for (size_t i = 0; i < d.videoTracks.size(); ++i) {
            if (!accept(TrackRef {TrackType::Video, int(i)}, d.videoTracks[i]))
                continue;
            for (const ClipData& c : d.videoTracks[i].clips) {
                const int64_t dh = std::abs(c.start - playhead);
                const int64_t dt = std::abs(c.end() - playhead);
                if (dh < bestDistance) {
                    bestId = c.id;
                    bestHead = true;
                    bestDistance = dh;
                }
                if (dt < bestDistance) {
                    bestId = c.id;
                    bestHead = false;
                    bestDistance = dt;
                }
            }
        }
        if (bestId)
            edges.append({bestId, bestHead});
    }

    // A tail that touches the next clip is the same cut as that clip's head: the transition
    // is stored on the right-hand clip (transitionIn). Existing transitions are kept.
    struct Edge
    {
        quint64 clip;
        bool head;
        int64_t frames;
        ops::HandleShortfall shortfall;
    };
    QVector<Edge> todo;
    const int64_t standard
        = EditBehaviour::frames(EditBehaviour::fromSettings().transitionSeconds, d.fps);
    int64_t missing = 0;
    bool canShorten = false;
    for (auto [clipId, head] : std::as_const(edges)) {
        if (!head) {
            if (const quint64 next = ops::cutNeighbour(d, clipId, false)) {
                clipId = next;
                head = true;
            }
        }
        const auto loc = ops::findClip(d, clipId);
        if (!loc || d.track(loc->track)->locked)
            continue;
        const ClipData& clip = d.track(loc->track)->clips[size_t(loc->clipIndex)];
        if ((head ? clip.transitionIn : clip.transitionOut).has_value())
            continue;
        if (std::any_of(todo.cbegin(), todo.cend(),
                [&](const Edge& e) { return e.clip == clipId && e.head == head; }))
            continue;
        const int64_t frames = std::min(standard,
            ops::maxTransitionDuration(d, clipId, head, TransitionAlignment::Centre, false));
        if (frames < 1)
            continue;
        const ops::HandleShortfall s
            = ops::transitionShortfall(d, clipId, head, frames, TransitionAlignment::Centre);
        missing = std::max(missing, s.outgoing + s.incoming);
        canShorten |= s.any()
            && ops::maxTransitionDuration(d, clipId, head, TransitionAlignment::Centre, true) >= 1;
        todo.append({clipId, head, frames, s});
    }
    if (todo.isEmpty())
        return;

    // Not enough media for the transition (spec 13.7): ask.
    HandleChoice choice = HandleChoice::AddAnyway;
    if (missing > 0 && m_handlePrompt)
        choice = m_handlePrompt(missing, canShorten);
    if (choice == HandleChoice::Cancel)
        return;

    runEdit(project(), tr("Add Transition"), [&](TimelineData& data) {
        bool changed = false;
        for (Edge e : std::as_const(todo)) {
            if (choice == HandleChoice::TrimClips && e.shortfall.any()) {
                // Shorten the outgoing tail and the incoming head (rippling, so the cut stays
                // closed) until the media covers the transition.
                if (const quint64 out = ops::cutNeighbour(data, e.clip, true))
                    rippleTrimWithLinks(data, out, false, -e.shortfall.outgoing, m_linked);
                rippleTrimWithLinks(data, e.clip, true, e.shortfall.incoming, m_linked);
                e.frames = std::min(e.frames,
                    ops::maxTransitionDuration(
                        data, e.clip, e.head, TransitionAlignment::Centre, false));
            } else if (choice == HandleChoice::Shorten && e.shortfall.any()) {
                e.frames = std::min(e.frames,
                    ops::maxTransitionDuration(
                        data, e.clip, e.head, TransitionAlignment::Centre, true));
            }
            if (e.frames < 1)
                continue;
            ClipData* clip = ops::clipById(data, e.clip);
            std::optional<TransitionData>& slot = e.head ? clip->transitionIn : clip->transitionOut;
            slot = EffectRegistry::instance().createTransition(kDefaultTransition, e.frames);
            changed = true;
        }
        return changed;
    });
}

bool TimelineController::setTransitionAlignment(
    quint64 clipId, bool head, TransitionAlignment alignment)
{
    return runEdit(project(), tr("Transition Alignment"), [&](TimelineData& data) {
        return ops::setTransitionAlignment(data, clipId, head, alignment);
    });
}

bool TimelineController::setTransitionDuration(quint64 clipId, bool head, int64_t frames)
{
    return runEdit(project(), tr("Change Transition Length"), [&](TimelineData& data) {
        const ClipData* clip = ops::clipById(data, clipId);
        const auto& slot = clip ? (head ? clip->transitionIn : clip->transitionOut)
                                : std::optional<TransitionData> {};
        const int64_t before = slot ? slot->duration : 0;
        return ops::setTransitionDuration(data, clipId, head, frames) != before && before > 0;
    });
}

} // namespace kk
