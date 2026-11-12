// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeCommands — the keyframe commands shared by the timeline lanes and the Keyframes
// panel (delete, copy, paste, interpolation presets, flatten), each one undo step, plus the
// process-wide keyframe clipboard.
#pragma once

#include "core/Animation.h"
#include "core/SelectionModel.h"

#include <QVector>

namespace kk {

struct AppContext;

/// Commands on the selected keyframes (SelectionModel::keyframes()). Every command is one undo
/// step through Project::beginEdit()/endEdit() and returns true if it changed something.
/// The clipboard is shared by every editor (copy in the panel, paste in a lane).
/// Thread-safety: GUI thread only.
namespace keyframecommands {

/// Deletes the selected keyframes and clears the keyframe selection.
bool deleteSelected(AppContext& ctx);
/// Copies the selected keyframes (of the first selected key's clip) to the clipboard.
bool copySelected(AppContext& ctx);
/// True if the clipboard holds keyframes.
[[nodiscard]] bool hasCopied();
/// Pastes the clipboard into the current clip (SelectionModel::currentClip()) with the
/// earliest key at the playhead; the pasted keys become the selection.
bool paste(AppContext& ctx);
/// Applies interpolation preset @p interp to the selected keys.
bool setSelectedInterp(AppContext& ctx, Interp interp);
/// Makes the handles of the selected keys horizontal.
bool flattenSelected(AppContext& ctx);
/// The keys the easing commands act on: the selected keys, else every key at the playhead of
/// the selected clips (spec 49.4).
[[nodiscard]] QVector<KeyframeRef> easingTargets(AppContext& ctx);
/// Applies preset @p interp to easingTargets() in one undo step ("Keyframe Ease In", ...).
bool easeTargets(AppContext& ctx, Interp interp);
/// Connects the registry's easing commands (clip.keyframe_linear, clip.keyframe_ease_in,
/// clip.keyframe_ease_out, clip.keyframe_ease_in_out) to easeTargets(), once per registry
/// however many editors call it.
void bindEasingActions(AppContext& ctx);
/// The easing command ids with their presets, in menu order.
struct EasingAction
{
    const char* id;
    Interp interp;
};
[[nodiscard]] const QVector<EasingAction>& easingActions();
/// The parameters the extrapolation commands act on: the parameters of the selected keys, else
/// @p fallback (refs of parameters; the frame is ignored, e.g. the curves shown), else the
/// parameters keyed at the playhead of the selected clips (easingTargets()).
[[nodiscard]] QVector<KeyframeRef> extrapolationTargets(
    AppContext& ctx, const QVector<KeyframeRef>& fallback = {});
/// Sets what the targets do before (@p pre) and / or after (@p post) their keys (Fusion's Set
/// Loop, Set Ping-Pong, Set Relative and the Pre-Loop variants; Constant removes the loop).
/// One undo step.
bool setExtrapolation(AppContext& ctx, bool pre, bool post, Extrapolation mode,
    const QVector<KeyframeRef>& fallback = {});
/// Reverse: mirrors the selected keys of each parameter in time. One undo step; the moved keys
/// stay selected.
bool reverseSelected(AppContext& ctx);
/// Time Stretch: scales the time of the selected keys around clip-local frame @p pivot by
/// @p factor (> 0; 2 = twice as slow). One undo step; the moved keys stay selected.
bool stretchSelected(AppContext& ctx, double pivot, double factor);
/// Time Stretch around the playhead (the pivot Fusion uses); returns false without keys.
bool stretchSelectedAtPlayhead(AppContext& ctx, double factor);
/// Step In (@p stepIn) or Step Out (hold) on the selected keys. One undo step.
bool stepSelected(AppContext& ctx, bool stepIn);
/// The command ids of the extrapolation / spline commands with what they do, in menu order
/// (bound by bindSplineActions()).
struct SplineAction
{
    const char* id;
    enum class Kind {
        Extrapolate,
        Interpolate,
        Reverse,
        StepIn,
        StepOut
    } kind;
    bool pre; ///< Extrapolate: before the first key
    Extrapolation mode;
    Interp interp; ///< Interpolate
};
[[nodiscard]] const QVector<SplineAction>& splineActions();
/// Connects the splineActions() commands of the registry (selection based), once per registry.
void bindSplineActions(AppContext& ctx);
/// Empties the clipboard (tests).
void clearClipboard();

} // namespace keyframecommands

} // namespace kk
