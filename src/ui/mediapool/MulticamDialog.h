// SPDX-License-Identifier: GPL-3.0-or-later
// MulticamDialog — "New Multicam Clip" options: name, angle sync, angle names, move the
// source clips into an "Original Clips" bin.
#pragma once

#include "core/Multicam.h"

#include <QString>

#include <optional>

class QWidget;

namespace kk {

/// What the user chose in the New Multicam Clip dialog.
struct MulticamOptions
{
    QString name;
    multicam::SyncMethod sync = multicam::SyncMethod::Waveform;
    /// Angle names from the clip names (true) or "Angle 1", "Angle 2", ... (false).
    bool namesFromClips = true;
    /// Move the source clips into a new "Original Clips" bin.
    bool moveSources = false;
};

/// Shows the dialog for @p clipCount clips (@p suggestion = proposed name, @p defaultSync =
/// preselected method). The last choices are remembered (QSettings "multicam/*").
/// nullopt when cancelled.
[[nodiscard]] std::optional<MulticamOptions> askMulticamOptions(
    QWidget* parent, int clipCount, const QString& suggestion);

} // namespace kk
