// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeCommands implementation: thin undo wrappers around core/KeyframeOps.
#include "ui/keyframes/KeyframeCommands.h"

#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/AppContext.h"

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
    const int n = runOnSelection(ctx, QT_TRANSLATE_NOOP("kk::KeyframeCommands", "Delete Keyframes"),
        [](TimelineData& d, const QVector<KeyframeRef>& r) { return ko::deleteKeys(d, r); });
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

} // namespace kk::keyframecommands
