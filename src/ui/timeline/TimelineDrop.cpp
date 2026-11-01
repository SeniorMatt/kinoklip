// SPDX-License-Identifier: GPL-3.0-or-later
// TimelineWidget drag and drop: media items, source ranges, effects, transitions, generators
// (MIME contract in ui/common/DragMime.h). planDrop() turns a MIME payload and a position into
// a DropPlan; the same plan drives the ghost preview while dragging and the edit on drop, so
// what the user sees is exactly what they get.
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QDragEnterEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>

namespace kk {

using namespace timelinemetrics;
namespace ops = editops;

namespace {
/// Default transition length for drops (frames = 1 s).
constexpr int kDropTransitionSeconds = 1;

bool hasTimelineFormat(const QMimeData* mime)
{
    return mime->hasFormat(QLatin1String(mime::MediaItems))
        || mime->hasFormat(QLatin1String(mime::SourceRange))
        || mime->hasFormat(QLatin1String(mime::Effect));
}
} // namespace

TimelineWidget::DropPlan TimelineWidget::planDrop(
    const QMimeData* mime, QPointF pos, Qt::KeyboardModifiers modifiers)
{
    DropPlan plan;
    const TimelineData& data = m_ctx.project->timeline()->data();
    const TimelineHit hit = m_layout.hitTest(data, pos);
    if (hit.area != TimelineHit::Area::Track && hit.area != TimelineHit::Area::Clip)
        return plan;
    plan.insert = modifiers & Qt::ControlModifier;

    // Destination tracks: the row under the mouse for its type, the same index (clamped) for
    // the other type, so V2 pairs with A2 like the source patch would.
    const int nv = int(data.videoTracks.size());
    const int na = int(data.audioTracks.size());
    const int idx = hit.track.index;
    const int videoTrack = nv == 0 ? -1 : std::min(idx, nv - 1);
    const int audioTrack = na == 0 ? -1 : std::min(idx, na - 1);
    int64_t frame = m_layout.nearestFrameAtX(pos.x());

    auto snapStart = [&](int64_t f, int64_t duration) {
        if (!m_controller->snapping())
            return f;
        const auto snaps = ops::snapCandidates(data, m_ctx.project->timeline()->playhead());
        const int64_t tol = snapToleranceFrames();
        const auto s1 = ops::snapToNearest(snaps, f, tol);
        const auto s2 = ops::snapToNearest(snaps, f + duration, tol);
        if (s1 && (!s2 || std::abs(*s1 - f) <= std::abs(*s2 - f - duration)))
            return *s1;
        if (s2)
            return std::max<int64_t>(0, *s2 - duration);
        return f;
    };

    // ---- Media items / source range: one group of clips per source, placed in sequence ----
    QVector<SourceSelection> sources;
    if (mime->hasFormat(QLatin1String(mime::SourceRange))) {
        const QJsonObject o
            = QJsonDocument::fromJson(mime->data(QLatin1String(mime::SourceRange))).object();
        sources.append({o.value(QLatin1String("mediaId")).toString(),
            int64_t(o.value(QLatin1String("inUs")).toDouble()),
            int64_t(o.value(QLatin1String("outUs")).toDouble())});
    } else if (mime->hasFormat(QLatin1String(mime::MediaItems))) {
        const QString text = QString::fromUtf8(mime->data(QLatin1String(mime::MediaItems)));
        for (const QString& id : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            sources.append({id.trimmed(), 0, 0});
    }
    if (!sources.isEmpty()) {
        int64_t at = -1;
        for (const SourceSelection& s : sources) {
            QVector<ops::PlacedClip> group
                = m_controller->makeMediaClips(s.mediaId, s.inUs, s.outUs, videoTrack, audioTrack);
            if (group.isEmpty())
                continue;
            if (at < 0)
                at = snapStart(frame, group.front().clip.duration);
            for (ops::PlacedClip& p : group)
                p.clip.start = at;
            at += group.front().clip.duration;
            plan.groups.append(group);
        }
        if (!plan.groups.isEmpty())
            plan.kind = DropPlan::Kind::Clips;
        return plan;
    }

    // ---- Effects library: effect, transition, generator or title ----
    if (!mime->hasFormat(QLatin1String(mime::Effect)))
        return plan;
    const QString effectId = QString::fromUtf8(mime->data(QLatin1String(mime::Effect))).trimmed();
    const EffectDescriptor* desc = EffectRegistry::instance().find(effectId);
    if (!desc)
        return plan;
    plan.effectId = effectId;

    switch (desc->category) {
    case EffectCategory::Generator:
    case EffectCategory::Title: {
        if (videoTrack < 0)
            return plan;
        std::optional<ClipData> clip = TimelineController::makeGeneratorClip(effectId, data.fps);
        if (!clip)
            return plan;
        clip->start = snapStart(frame, clip->duration);
        // Titles and generators always go onto a video track: the row under the mouse when it
        // is one, else V1.
        const int track = hit.track.type == TrackType::Video ? hit.track.index : 0;
        plan.groups.append({ops::PlacedClip {TrackRef {TrackType::Video, track}, *clip}});
        plan.kind = DropPlan::Kind::Clips;
        return plan;
    }
    case EffectCategory::VideoEffect:
    case EffectCategory::AudioEffect: {
        const TrackType wanted
            = desc->category == EffectCategory::VideoEffect ? TrackType::Video : TrackType::Audio;
        if (hit.area != TimelineHit::Area::Clip || hit.track.type != wanted)
            return plan;
        plan.kind = DropPlan::Kind::Effect;
        plan.clipId = hit.clipId;
        return plan;
    }
    case EffectCategory::Transition: {
        if (hit.area != TimelineHit::Area::Clip)
            return plan;
        const auto loc = ops::findClip(data, hit.clipId);
        const TrackData& track = *data.track(loc->track);
        size_t index = size_t(loc->clipIndex);
        const ClipData* c = &track.clips[index];
        bool head = hit.frame < (c->start + c->end()) / 2.0;
        // The tail of a clip touching its right neighbour is that neighbour's head.
        if (!head && index + 1 < track.clips.size() && track.clips[index + 1].start == c->end()) {
            ++index;
            head = true;
            c = &track.clips[index];
        }
        const bool touching = head && index > 0 && track.clips[index - 1].end() == c->start;
        plan.kind = DropPlan::Kind::Transition;
        plan.clipId = c->id;
        plan.head = head;
        plan.transitionFrames = std::min(
            std::max<int64_t>(1, usToFrames(int64_t(kDropTransitionSeconds) * 1000000, data.fps)),
            c->duration);
        const int64_t d = plan.transitionFrames;
        if (head) {
            plan.previewFrom = touching ? c->start - d / 2 : c->start;
        } else {
            plan.previewFrom = c->end() - d;
        }
        plan.previewTo = plan.previewFrom + d;
        plan.previewRow = m_layout.rowOf(loc->track);
        return plan;
    }
    case EffectCategory::ClipProperties:
        return plan;
    }
    return plan;
}

void TimelineWidget::showDropPreview(const DropPlan& plan)
{
    m_overlay.ghosts.clear();
    m_overlay.dropClip = 0;
    m_overlay.dropTransitionRow = -1;
    switch (plan.kind) {
    case DropPlan::Kind::Clips:
        for (const auto& group : plan.groups) {
            for (const ops::PlacedClip& p : group) {
                m_overlay.ghosts.push_back(
                    {m_layout.rowOf(p.track), p.clip.start, p.clip.duration, p.clip.name});
            }
        }
        break;
    case DropPlan::Kind::Effect:
        m_overlay.dropClip = plan.clipId;
        break;
    case DropPlan::Kind::Transition:
        m_overlay.dropTransitionRow = plan.previewRow;
        m_overlay.dropTransitionFrom = plan.previewFrom;
        m_overlay.dropTransitionTo = plan.previewTo;
        break;
    case DropPlan::Kind::None:
        break;
    }
    viewport()->update();
}

void TimelineWidget::clearDropPreview()
{
    showDropPreview(DropPlan {});
}

bool TimelineWidget::executeDrop(const DropPlan& plan)
{
    Project* project = m_ctx.project;
    QVector<quint64> created;
    switch (plan.kind) {
    case DropPlan::Kind::None:
        return false;
    case DropPlan::Kind::Clips: {
        project->beginEdit(plan.insert ? tr("Insert Clip") : tr("Add Clip"));
        TimelineData& data = project->timeline()->mutableData();
        for (const auto& group : plan.groups) {
            const QVector<quint64> ids = plan.insert
                ? ops::insertClips(data, group, ops::RippleScope::AllTracks)
                : ops::overwriteClips(data, group);
            if (ids.size() > 1)
                ops::linkClips(data, ids);
            created += ids;
        }
        if (created.isEmpty()) {
            project->cancelEdit();
            return false;
        }
        project->endEdit();
        m_ctx.selection->setClips(created);
        return true;
    }
    case DropPlan::Kind::Effect: {
        project->beginEdit(tr("Add Effect"));
        TimelineData& data = project->timeline()->mutableData();
        const auto loc = ops::findClip(data, plan.clipId);
        if (!loc || data.track(loc->track)->locked) {
            project->cancelEdit();
            return false;
        }
        const quint64 fxId = data.allocateId();
        ClipData& clip = data.track(loc->track)->clips[size_t(loc->clipIndex)];
        clip.effects.append(EffectRegistry::instance().createEffect(plan.effectId, fxId));
        project->endEdit();
        m_ctx.selection->selectClip(plan.clipId);
        return true;
    }
    case DropPlan::Kind::Transition: {
        project->beginEdit(tr("Add Transition"));
        TimelineData& data = project->timeline()->mutableData();
        const auto loc = ops::findClip(data, plan.clipId);
        if (!loc || data.track(loc->track)->locked) {
            project->cancelEdit();
            return false;
        }
        ClipData& clip = data.track(loc->track)->clips[size_t(loc->clipIndex)];
        const TransitionData t
            = EffectRegistry::instance().createTransition(plan.effectId, plan.transitionFrames);
        (plan.head ? clip.transitionIn : clip.transitionOut) = t;
        project->endEdit();
        return true;
    }
    }
    return false;
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (!hasTimelineFormat(event->mimeData())) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    showDropPreview(planDrop(event->mimeData(), event->position(), event->modifiers()));
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* event)
{
    const DropPlan plan = planDrop(event->mimeData(), event->position(), event->modifiers());
    showDropPreview(plan);
    if (plan.kind == DropPlan::Kind::None)
        event->ignore();
    else
        event->acceptProposedAction();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*)
{
    clearDropPreview();
}

void TimelineWidget::dropEvent(QDropEvent* event)
{
    const DropPlan plan = planDrop(event->mimeData(), event->position(), event->modifiers());
    clearDropPreview();
    if (executeDrop(plan))
        event->acceptProposedAction();
    else
        event->ignore();
}

} // namespace kk
