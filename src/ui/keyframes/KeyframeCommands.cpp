// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeCommands implementation: thin undo wrappers around core/KeyframeOps.
#include "ui/keyframes/KeyframeCommands.h"

#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"

#include <QAction>
#include <QCoreApplication>

namespace kk::keyframecommands {

namespace ko = keyframeops;

namespace {
/// The one keyframe clipboard of the application (copy in one editor, paste in another).
ko::KeyClipboard& clipboard()
{
    static ko::KeyClipboard board;
    return board;
}

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::KeyframeCommands", text);
}

/// Runs @p op on the timeline inside one undo step labelled @p label.
template<typename Op>
int runOnSelection(AppContext& ctx, const char* label, Op&& op)
{
    const QVector<KeyframeRef> refs = ctx.selection->keyframes();
    if (refs.isEmpty())
        return 0;
    ctx.project->beginEdit(tr(label));
    const int n = op(ctx.project->timeline()->mutableData(), refs);
    ctx.project->endEdit();
    return n;
}
} // namespace

bool deleteSelected(AppContext& ctx)
{
    // A parameter losing every key keeps the value shown at the playhead (spec 49.1).
    const int64_t playhead = ctx.project->timeline()->playhead();
    const int n = runOnSelection(ctx, QT_TRANSLATE_NOOP("kk::KeyframeCommands", "Delete Keyframes"),
        [playhead](TimelineData& d, const QVector<KeyframeRef>& r) {
            return ko::deleteKeys(d, r, playhead);
        });
    if (n > 0)
        ctx.selection->setKeyframes({});
    return n > 0;
}

bool copySelected(AppContext& ctx)
{
    const QVector<KeyframeRef> refs = ctx.selection->keyframes();
    if (refs.isEmpty())
        return false;
    clipboard() = ko::copyKeys(ctx.project->timeline()->data(), refs);
    return !clipboard().isEmpty();
}

bool hasCopied()
{
    return !clipboard().isEmpty();
}

void clearClipboard()
{
    clipboard() = {};
}

bool paste(AppContext& ctx)
{
    const quint64 id = ctx.selection->currentClip();
    const ClipData* clip = editops::clipById(ctx.project->timeline()->data(), id);
    if (!clip || clipboard().isEmpty())
        return false;
    const int64_t local = ctx.project->timeline()->playhead() - clip->start;
    ctx.project->beginEdit(tr(QT_TRANSLATE_NOOP("kk::KeyframeCommands", "Paste Keyframes")));
    const QVector<KeyframeRef> refs
        = ko::pasteKeys(ctx.project->timeline()->mutableData(), id, clipboard(), local);
    ctx.project->endEdit();
    ctx.selection->setKeyframes(refs);
    return !refs.isEmpty();
}

bool setSelectedInterp(AppContext& ctx, Interp interp)
{
    return runOnSelection(ctx,
               QT_TRANSLATE_NOOP("kk::KeyframeCommands", "Change Keyframe Interpolation"),
               [interp](TimelineData& d, const QVector<KeyframeRef>& r) {
                   return ko::setInterp(d, r, interp);
               })
        > 0;
}

bool flattenSelected(AppContext& ctx)
{
    return runOnSelection(ctx,
               QT_TRANSLATE_NOOP("kk::KeyframeCommands", "Flatten Keyframe Handles"),
               [](TimelineData& d, const QVector<KeyframeRef>& r) {
                   return ko::flattenHandles(d, r);
               })
        > 0;
}

QVector<KeyframeRef> extrapolationTargets(AppContext& ctx, const QVector<KeyframeRef>& fallback)
{
    QVector<KeyframeRef> refs = ctx.selection->keyframes();
    if (refs.isEmpty())
        refs = fallback;
    if (refs.isEmpty())
        refs = easingTargets(ctx);
    return refs;
}

bool setExtrapolation(
    AppContext& ctx, bool pre, bool post, Extrapolation mode, const QVector<KeyframeRef>& fallback)
{
    const QVector<KeyframeRef> refs = extrapolationTargets(ctx, fallback);
    if (refs.isEmpty())
        return false;
    const char* label = mode == Extrapolation::Constant
        ? QT_TRANSLATE_NOOP("kk::KeyframeCommands", "Remove Keyframe Loop")
        : QT_TRANSLATE_NOOP("kk::KeyframeCommands", "Set Keyframe Loop");
    ctx.project->beginEdit(tr(label));
    const int n
        = ko::setExtrapolation(ctx.project->timeline()->mutableData(), refs, pre, post, mode);
    ctx.project->endEdit();
    return n > 0;
}

bool reverseSelected(AppContext& ctx)
{
    QVector<KeyframeRef> moved;
    const int n
        = runOnSelection(ctx, QT_TRANSLATE_NOOP("kk::KeyframeCommands", "Reverse Keyframes"),
            [&moved](TimelineData& d, const QVector<KeyframeRef>& r) {
                moved = ko::reverseKeys(d, r);
                return int(moved.size());
            });
    if (n > 0)
        ctx.selection->setKeyframes(moved);
    return n > 0;
}

bool stretchSelected(AppContext& ctx, double pivot, double factor)
{
    if (!(factor > 0.0))
        return false;
    QVector<KeyframeRef> moved;
    const int n = runOnSelection(ctx,
        QT_TRANSLATE_NOOP("kk::KeyframeCommands", "Stretch Keyframes in Time"),
        [&](TimelineData& d, const QVector<KeyframeRef>& r) {
            moved = ko::stretchKeys(d, r, pivot, factor);
            return int(moved.size());
        });
    if (n > 0)
        ctx.selection->setKeyframes(moved);
    return n > 0;
}

bool stretchSelectedAtPlayhead(AppContext& ctx, double factor)
{
    const QVector<KeyframeRef> refs = ctx.selection->keyframes();
    if (refs.isEmpty())
        return false;
    const ClipData* clip = editops::clipById(ctx.project->timeline()->data(), refs.front().clipId);
    if (!clip)
        return false;
    return stretchSelected(ctx, double(ctx.project->timeline()->playhead() - clip->start), factor);
}

bool stepSelected(AppContext& ctx, bool stepIn)
{
    return runOnSelection(ctx,
               QT_TRANSLATE_NOOP("kk::KeyframeCommands", "Change Keyframe Interpolation"),
               [stepIn](TimelineData& d, const QVector<KeyframeRef>& r) {
                   return ko::stepKeys(d, r, stepIn);
               })
        > 0;
}

const QVector<SplineAction>& splineActions()
{
    using K = SplineAction::Kind;
    static const QVector<SplineAction> list {
        {"clip.keyframe_loop", K::Extrapolate, false, Extrapolation::Loop, Interp::Linear},
        {"clip.keyframe_ping_pong", K::Extrapolate, false, Extrapolation::PingPong, Interp::Linear},
        {"clip.keyframe_relative", K::Extrapolate, false, Extrapolation::Relative, Interp::Linear},
        {"clip.keyframe_extrapolate_linear", K::Extrapolate, false, Extrapolation::Linear,
            Interp::Linear},
        {"clip.keyframe_no_loop", K::Extrapolate, false, Extrapolation::Constant, Interp::Linear},
        {"clip.keyframe_pre_loop", K::Extrapolate, true, Extrapolation::Loop, Interp::Linear},
        {"clip.keyframe_pre_ping_pong", K::Extrapolate, true, Extrapolation::PingPong,
            Interp::Linear},
        {"clip.keyframe_pre_relative", K::Extrapolate, true, Extrapolation::Relative,
            Interp::Linear},
        {"clip.keyframe_pre_extrapolate_linear", K::Extrapolate, true, Extrapolation::Linear,
            Interp::Linear},
        {"clip.keyframe_pre_no_loop", K::Extrapolate, true, Extrapolation::Constant,
            Interp::Linear},
        {"clip.keyframe_smooth", K::Interpolate, false, Extrapolation::Constant, Interp::Smooth},
        {"clip.keyframe_step_in", K::StepIn, false, Extrapolation::Constant, Interp::Linear},
        {"clip.keyframe_step_out", K::StepOut, false, Extrapolation::Constant, Interp::Hold},
        {"clip.keyframe_reverse", K::Reverse, false, Extrapolation::Constant, Interp::Linear},
    };
    return list;
}

void bindSplineActions(AppContext& ctx)
{
    if (!ctx.actions)
        return;
    AppContext* context = &ctx;
    for (const SplineAction& a : splineActions()) {
        const QString id = QLatin1String(a.id);
        QAction* action = ctx.actions->contains(id) ? ctx.actions->action(id) : nullptr;
        if (!action || action->property("kk_splineBound").toBool())
            continue;
        action->setProperty("kk_splineBound", true);
        const SplineAction spec = a;
        QObject::connect(action, &QAction::triggered, ctx.actions, [context, spec] {
            using K = SplineAction::Kind;
            switch (spec.kind) {
            case K::Extrapolate:
                setExtrapolation(*context, spec.pre, !spec.pre, spec.mode);
                break;
            case K::Interpolate:
                easeTargets(*context, spec.interp);
                break;
            case K::Reverse:
                reverseSelected(*context);
                break;
            case K::StepIn:
                stepSelected(*context, true);
                break;
            case K::StepOut:
                stepSelected(*context, false);
                break;
            }
        });
    }
}

const QVector<EasingAction>& easingActions()
{
    static const QVector<EasingAction> list {
        {"clip.keyframe_linear", Interp::Linear},
        {"clip.keyframe_ease_in", Interp::EaseIn},
        {"clip.keyframe_ease_out", Interp::EaseOut},
        {"clip.keyframe_ease_in_out", Interp::EaseInOut},
    };
    return list;
}

QVector<KeyframeRef> easingTargets(AppContext& ctx)
{
    QVector<KeyframeRef> refs = ctx.selection->keyframes();
    if (!refs.isEmpty())
        return refs;
    const TimelineData& data = ctx.project->timeline()->data();
    const int64_t playhead = ctx.project->timeline()->playhead();
    for (quint64 id : ctx.selection->clips()) {
        const ClipData* clip = editops::clipById(data, id);
        if (clip && playhead >= clip->start && playhead < clip->start + clip->duration)
            refs += ko::keysAt(*clip, playhead - clip->start);
    }
    return refs;
}

bool easeTargets(AppContext& ctx, Interp interp)
{
    const QVector<KeyframeRef> refs = easingTargets(ctx);
    if (refs.isEmpty())
        return false;
    ctx.project->beginEdit(
        tr(QT_TRANSLATE_NOOP("kk::KeyframeCommands", "Change Keyframe Interpolation")));
    const int n = ko::setInterp(ctx.project->timeline()->mutableData(), refs, interp);
    ctx.project->endEdit();
    return n > 0;
}

void bindEasingActions(AppContext& ctx)
{
    if (!ctx.actions)
        return;
    AppContext* context = &ctx;
    for (const EasingAction& e : easingActions()) {
        const QString id = QLatin1String(e.id);
        QAction* action = ctx.actions->contains(id) ? ctx.actions->action(id) : nullptr;
        // Several editors (each timeline's lanes, the Keyframes panel) call this; the first
        // one connects, so one key press is one edit.
        if (!action || action->property("kk_easingBound").toBool())
            continue;
        action->setProperty("kk_easingBound", true);
        const Interp interp = e.interp;
        QObject::connect(action, &QAction::triggered, ctx.actions,
            [context, interp] { easeTargets(*context, interp); });
    }
}

} // namespace kk::keyframecommands
