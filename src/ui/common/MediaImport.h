// SPDX-License-Identifier: GPL-3.0-or-later
// Shared "import these files and tell the user what was skipped" helper for all import paths
// (File ▸ Import, drag & drop onto the media pool, double-click in the media browser).
#pragma once

#include <QStringList>

class QWidget;

namespace cw {

struct AppContext;

/// Glob patterns of importable media files ("*.mp4", "*.wav", ...).
[[nodiscard]] QStringList mediaFileNamePatterns();

/// File dialog name filter for importable media ("Media files (*.mp4 *.mov ...);;All files (*)").
[[nodiscard]] QString mediaFileDialogFilter();

/// Imports @p paths into the media pool as one undoable step and shows a warning dialog
/// (parented to @p parent) listing files that could not be imported. Returns the new item ids.
QStringList importMediaWithFeedback(
    AppContext& ctx, const QStringList& paths, QWidget* parent, const QString& binId = {});

} // namespace cw
