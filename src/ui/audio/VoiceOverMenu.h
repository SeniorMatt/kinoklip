// SPDX-License-Identifier: GPL-3.0-or-later
// VoiceOverMenu — the input menu of an audio track's arm button (timeline header and mixer strip).
#pragma once

#include <QString>
#include <QtGlobal>

class QMenu;

namespace kk {

class VoiceOverController;

/// Fills @p menu with the choices of the recording input of track @p trackId: "Default" (the
/// system's or Preferences' input, first) and every system audio input, one of them checked;
/// "No microphone found" (disabled) with none. A choice sets the track's input.
void fillVoiceOverInputMenu(QMenu& menu, VoiceOverController* controller, quint64 trackId);

/// Tooltip of the arm button of track @p trackId: what a click does, or why it cannot (no
/// microphone, permission denied), plus the input in use.
[[nodiscard]] QString voiceOverArmTooltip(const VoiceOverController* controller, quint64 trackId);

} // namespace kk
