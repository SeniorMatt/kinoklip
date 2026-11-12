// SPDX-License-Identifier: GPL-3.0-or-later
// Transitions from the keyboard (spec section 83, 5.4 and 83.xx): `]t` `[t` select the next /
// previous transition and put the playhead on its edit point; Space e d deletes it, Space e + and
// Space e - lengthen and shorten it, Space e a cycles its alignment; Space f e (the effects
// picker) with the playhead on the edit point sets another type, and the Inspector's Transition
// tab is a form (Ctrl+A on its Duration, a choice for the type).
#include "app/VimController.h"
#include "app/VimModeLine.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/effects/EffectActions.h"
#include "ui/timeline/VimTimelineHost.h"

#include <algorithm>

namespace kk {

namespace {

struct EdgeTransition
{
    int64_t frame = 0; ///< the edit point (timeline frame)
    quint64 clipId = 0;
    bool head = true;
    friend bool operator==(const EdgeTransition&, const EdgeTransition&) = default;
};

/// Every transition of the timeline with the frame of its edit point, in order.
QVector<EdgeTransition> allTransitions(const TimelineData& data)
{
    QVector<EdgeTransition> out;
    for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
        for (const TrackData& track : data.tracks(type)) {
            for (const ClipData& clip : track.clips) {
                if (clip.transitionIn)
                    out.append({clip.start, clip.id, true});
                if (clip.transitionOut)
                    out.append({clip.start + clip.duration, clip.id, false});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const EdgeTransition& a, const EdgeTransition& b) {
        return a.frame != b.frame  ? a.frame < b.frame
            : a.clipId != b.clipId ? a.clipId < b.clipId
                                   : a.head > b.head;
    });
    return out;
}

} // namespace

bool VimController::transitionCommand(const QString& name, int count, bool hasCount)
{
    Q_UNUSED(hasCount);
    Project& project = *m_ctx.project;
    const TimelineData& data = project.timeline()->data();
    const int n = std::max(1, count);
    const QVector<EdgeTransition> all = allTransitions(data);

    // The transition the command works on: the selected one, else the one at the edit point
    // under the cursor.
    const auto target = [&]() -> std::optional<EdgeTransition> {
        if (const auto sel = m_ctx.selection->transition()) {
            for (const EdgeTransition& t : all) {
                if (t.clipId == sel->clipId && t.head == sel->head)
                    return t;
            }
        }
        const int64_t at = project.timeline()->playhead();
        for (const EdgeTransition& t : all) {
            if (t.frame == at)
                return t;
        }
        return std::nullopt;
    };

    if (name == QLatin1String("transition_next") || name == QLatin1String("transition_prev")) {
        if (all.isEmpty()) {
            m_modeLine->showError(tr("No transitions"), 4000);
            return true;
        }
        const bool forward = name == QLatin1String("transition_next");
        int to = 0;
        if (const auto here = target()) {
            to = int(all.indexOf(*here)) + (forward ? n : -n);
        } else {
            // From the playhead: the first one after (the last one before) it, then n - 1 more.
            const int64_t ph = project.timeline()->playhead();
            int first = -1;
            for (int i = 0; i < all.size(); ++i) {
                if (forward ? all[i].frame > ph : all[i].frame < ph) {
                    first = i;
                    if (forward)
                        break;
                }
            }
            if (first < 0) {
                m_modeLine->showError(tr("No more transitions"), 4000);
                return true;
            }
            to = first + (forward ? n - 1 : -(n - 1));
        }
        to = std::clamp(to, 0, int(all.size()) - 1);
        const EdgeTransition& t = all[to];
        m_ctx.selection->selectTransition({t.clipId, t.head});
        project.timeline()->setPlayhead(t.frame);
        m_modeLine->showMessage(tr("Transition %1 of %2").arg(to + 1).arg(all.size()), 3000);
        return true;
    }
    const auto t = target();
    if (!t) {
        m_modeLine->showError(tr("No transition at the cursor: ]t selects one"), 4000);
        return true;
    }
    if (name == QLatin1String("transition_delete")) {
        effectactions::removeTransition(project, t->clipId, t->head);
        return true;
    }
    const ClipData* clip = editops::clipById(data, t->clipId);
    const std::optional<TransitionData>& tr0 = t->head ? clip->transitionIn : clip->transitionOut;
    if (!clip || !tr0)
        return true;
    if (name == QLatin1String("transition_longer") || name == QLatin1String("transition_shorter")) {
        const int64_t to = tr0->duration + (name == QLatin1String("transition_longer") ? n : -n);
        project.beginEdit(tr("Change Transition Length"));
        editops::setTransitionDuration(project.timeline()->mutableData(), t->clipId, t->head, to);
        project.endEdit();
        return true;
    }
    if (name == QLatin1String("transition_align")) {
        const TransitionAlignment next = tr0->alignment == TransitionAlignment::Centre
            ? TransitionAlignment::StartOnCut
            : tr0->alignment == TransitionAlignment::StartOnCut ? TransitionAlignment::EndOnCut
                                                                : TransitionAlignment::Centre;
        project.beginEdit(tr("Change Transition Alignment"));
        const bool changed = editops::setTransitionAlignment(
            project.timeline()->mutableData(), t->clipId, t->head, next);
        project.endEdit();
        if (!changed)
            m_modeLine->showError(tr("The clips have no room for that alignment"), 4000);
        return true;
    }
    return false;
}

} // namespace kk
