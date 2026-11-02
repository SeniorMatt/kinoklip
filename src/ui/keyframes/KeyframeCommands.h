// SPDX-License-Identifier: GPL-3.0-or-later
// KeyframeCommands — the keyframe commands shared by the timeline lanes and the Keyframes
// panel (delete, copy, paste, interpolation presets, flatten), each one undo step, plus the
// process-wide keyframe clipboard.
#pragma once

#include "core/Animation.h"

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
/// Empties the clipboard (tests).
void clearClipboard();

} // namespace keyframecommands

} // namespace kk
