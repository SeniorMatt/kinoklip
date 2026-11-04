// SPDX-License-Identifier: GPL-3.0-or-later
// Shared "import these files and tell the user what was skipped" helper for all import paths
// (File ▸ Import, drag & drop onto the media pool, double-click in the media browser).
#pragma once

#include "core/Rational.h"

#include <QStringList>

#include <functional>
#include <optional>

class QWidget;

namespace kk {

struct AppContext;

/// Glob patterns of importable media files ("*.mp4", "*.wav", ...).
[[nodiscard]] QStringList mediaFileNamePatterns();

/// File dialog name filter for importable media ("Media files (*.mp4 *.mov ...);;All files (*)").
[[nodiscard]] QString mediaFileDialogFilter();
/// File dialog filter of File > Import Subtitle (SubRip .srt, WebVTT .vtt).
[[nodiscard]] QString subtitleFileDialogFilter();

/// The project bin new imports go into ("" = Master), for every import path that has no bin
/// of its own (File > Import, Ctrl+I, the Media page's Add to Media Pool, drops onto the media
/// pool's clip area, media imported by a timeline drop): the bin shown in the media pool
/// (AppContext::currentMediaPoolBin), or Master when Preferences > Editing > "Import new media
/// into" is "Master bin" (prefs::importIntoMasterBin()). A drop onto a bin passes that bin
/// explicitly instead.
[[nodiscard]] QString importTargetBin(const AppContext& ctx);

/// Imports @p paths into the media pool as one undoable step and shows a warning dialog
/// (parented to @p parent) listing files that could not be imported. Returns the new item ids.
/// @p binId: the bin ("" = Master); none = importTargetBin().
QStringList importMediaWithFeedback(AppContext& ctx, const QStringList& paths, QWidget* parent,
    const std::optional<QString>& binId = std::nullopt);

/// Single-file project: copies the linked files of media pool items @p ids (empty = every
/// item) into the project file and points the items at the stored copies, as one undoable
/// step (relinked clips, "Store in Project"). Returns the number of items changed (0 for a
/// linked project or when cancelled).
int storeItemsInProject(AppContext& ctx, const QStringList& ids, QWidget* parent);

/// True while the project's media pool holds no media files yet (timelines do not count):
/// the next import is the project's first.
[[nodiscard]] bool hasNoMediaYet(const AppContext& ctx);
/// After the project's first import (@p firstImport, from hasNoMediaYet() before it), as in
/// Resolve: when the first imported video of @p importedIds runs at another frame rate than
/// the project and the rate can still change (no timeline that follows the project has
/// clips, spec 25.13), asks "Change the project frame rate?" (Change / Don't Change); Change
/// sets the project frame rate (one undo step). Returns true when the rate changed.
bool offerProjectFrameRate(
    AppContext& ctx, bool firstImport, const QStringList& importedIds, QWidget* parent);
/// Tests: while set, offerProjectFrameRate() asks @p handler (clip rate, project rate; true =
/// Change) instead of showing the question. Pass an empty function to restore the dialog.
void setFrameRatePromptHandlerForTests(std::function<bool(Rational, Rational)> handler);

/// True if @p path is an existing media file outside any single-file project (a linked file).
[[nodiscard]] bool isLinkedFile(const QString& path);
/// True if @p path names media stored inside a single-file project ("film.kinoklip/media/...").
/// A cheap string test first; one stat of the container for candidates.
[[nodiscard]] bool isStoredInProject(const QString& path);
/// "Stored in the project file (film.kinoklip)" or "Linked" for the media at @p path.
[[nodiscard]] QString mediaLocationText(const QString& path);

} // namespace kk
