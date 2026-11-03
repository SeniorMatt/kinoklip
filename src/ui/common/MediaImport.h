// SPDX-License-Identifier: GPL-3.0-or-later
// Shared "import these files and tell the user what was skipped" helper for all import paths
// (File ▸ Import, drag & drop onto the media pool, double-click in the media browser).
#pragma once

#include <QStringList>

class QWidget;

namespace kk {

struct AppContext;

/// Glob patterns of importable media files ("*.mp4", "*.wav", ...).
[[nodiscard]] QStringList mediaFileNamePatterns();

/// File dialog name filter for importable media ("Media files (*.mp4 *.mov ...);;All files (*)").
[[nodiscard]] QString mediaFileDialogFilter();
/// File dialog filter of File > Import Subtitle (SubRip .srt, WebVTT .vtt).
[[nodiscard]] QString subtitleFileDialogFilter();

/// Imports @p paths into the media pool as one undoable step and shows a warning dialog
/// (parented to @p parent) listing files that could not be imported. Returns the new item ids.
QStringList importMediaWithFeedback(
    AppContext& ctx, const QStringList& paths, QWidget* parent, const QString& binId = {});

/// Single-file project: copies the linked files of media pool items @p ids (empty = every
/// item) into the project file and points the items at the stored copies, as one undoable
/// step (relinked clips, "Store in Project"). Returns the number of items changed (0 for a
/// linked project or when cancelled).
int storeItemsInProject(AppContext& ctx, const QStringList& ids, QWidget* parent);

/// True if @p path is an existing media file outside any single-file project (a linked file).
[[nodiscard]] bool isLinkedFile(const QString& path);
/// True if @p path names media stored inside a single-file project ("film.kinoklip/media/...").
/// A cheap string test first; one stat of the container for candidates.
[[nodiscard]] bool isStoredInProject(const QString& path);
/// "Stored in the project file (film.kinoklip)" or "Linked" for the media at @p path.
[[nodiscard]] QString mediaLocationText(const QString& path);

} // namespace kk
