// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineController, part 3: the default transition (Ctrl+T) with the insufficient-handles
// question, transition alignment and length (spec 13.1-13.9).
#include "core/EditBehaviour.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/effects/EffectActions.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineEditUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kk {

namespace ops = editops;
using timelineedit::runEdit;

namespace {

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

bool TimelineController::addTransition(const QString& typeId)
{
    const TimelineData& d = timeline()->data();
    // A given type fits only its own kind of track.
    std::optional<TrackType> onlyType;
    if (!typeId.isEmpty()) {
        const EffectDescriptor* desc = EffectRegistry::instance().find(typeId);
        if (!desc
            || (desc->category != EffectCategory::Transition
                && desc->category != EffectCategory::AudioTransition))
            return false;
        onlyType
            = desc->category == EffectCategory::Transition ? TrackType::Video : TrackType::Audio;
    }
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
        TrackType type; ///< picks the default transition: Cross Dissolve / Cross Fade +3 dB
        ops::HandleShortfall shortfall;
    };
    QVector<Edge> todo;
    QVector<std::pair<quint64, bool>> retype; ///< existing transitions to change to typeId
    const int64_t standard = project()->standardTransitionFrames();
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
        if (!loc || d.track(loc->track)->locked || (onlyType && loc->track.type != *onlyType))
            continue;
        const ClipData& clip = d.track(loc->track)->clips[size_t(loc->clipIndex)];
        if ((head ? clip.transitionIn : clip.transitionOut).has_value()) {
            // Ctrl+T keeps it; a chosen type replaces the type only.
            if (onlyType && (head ? clip.transitionIn : clip.transitionOut)->type != typeId)
                retype.append({clipId, head});
            continue;
        }
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
        todo.append({clipId, head, frames, loc->track.type, s});
    }
    if (todo.isEmpty() && retype.isEmpty())
        return false;

    // Not enough media for the transition (spec 13.7): ask.
    HandleChoice choice = HandleChoice::AddAnyway;
    if (missing > 0 && m_handlePrompt)
        choice = m_handlePrompt(missing, canShorten);
    if (choice == HandleChoice::Cancel)
        return false;

    return runEdit(project(), tr("Add Transition"), [&](TimelineData& data) {
        bool changed = false;
        for (const auto& [clipId, head] : std::as_const(retype)) {
            ClipData* clip = ops::clipById(data, clipId);
            std::optional<TransitionData>& slot = head ? clip->transitionIn : clip->transitionOut;
            TransitionData made
                = EffectRegistry::instance().createTransition(typeId, slot->duration);
            made.duration = slot->duration;
            made.alignment = slot->alignment;
            slot = made;
            changed = true;
        }
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
            // Default transition (Ctrl+T): the project's standard transition and length.
            slot = EffectRegistry::instance().createTransition(
                typeId.isEmpty() ? effectactions::defaultTransitionId(*project(), e.type) : typeId,
                e.frames);
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

void TimelineController::selectTransition(const TransitionRef& ref)
{
    const bool hadEdit = !m_selectedEdit.isEmpty() || m_selectedGap;
    m_selectedEdit = {};
    m_selectedGap.reset();
    m_ctx.selection->selectTransition(ref);
    if (hadEdit)
        emit stateChanged();
}

bool TimelineController::deleteTransition(const TransitionRef& ref)
{
    const bool changed = runEdit(project(), tr("Delete Transition"), [&](TimelineData& data) {
        const auto loc = ops::findClip(data, ref.clipId);
        const TrackData* track = loc ? data.track(loc->track) : nullptr;
        ClipData* clip = ops::clipById(data, ref.clipId);
        if (!clip || !track || track->locked)
            return false;
        auto& slot = ref.head ? clip->transitionIn : clip->transitionOut;
        if (!slot)
            return false;
        slot.reset();
        return true;
    });
    if (changed && m_ctx.selection->transition() == ref)
        m_ctx.selection->clear();
    return changed;
}

} // namespace kk
