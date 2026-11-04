// SPDX-License-Identifier: GPL-3.0-or-later
// FolderImport — "Import Media Folder": a whole folder (optionally with its subfolders, as bins)
// into the media pool as one undo step, scanned and probed on a worker thread with Cancel.
// Used by File > Import Media Folder, the media pool's context menu and folder drops, and the
// Media page's storage browser.
#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QWidget;

namespace kk {

struct AppContext;

/// How a folder is imported.
struct FolderImportOptions
{
    bool includeSubfolders = true; ///< walk into subfolders
    /// One bin per folder (sub-bins for subfolders); off: every file into the target bin.
    bool createBins = true;
};

/// What importMediaFolders() did.
struct FolderImportResult
{
    QStringList importedIds; ///< new media pool items, in folder / natural order
    QStringList createdBinIds; ///< bins created for the folders
    QStringList failures; ///< "path: reason" of each file that could not be imported
    QStringList notes; ///< "path: remark" of imported files worth telling about (empty cues)
    int duplicates = 0; ///< files skipped because they are already in the media pool
    int skippedHidden = 0; ///< hidden files and folders, system junk (.DS_Store ...)
    int skippedUnsupported = 0; ///< files of a type that is not imported
    int skippedLinks = 0; ///< folders not entered twice (symlink loops)
    bool cancelled = false; ///< the user cancelled: nothing was imported
};

/// Imports @p folders into bin @p binId ("" = Master) as ONE undo step "Import Media Folder"
/// (the items and the bins it creates). Only files Kinoklip imports are taken (the extensions
/// of File > Import Media, then the same probe), hidden and system files are skipped, numbered
/// stills become one image sequence clip, files already in the pool are skipped and counted,
/// and each folder is read in natural order ("clip2" before "clip10"). Scanning and probing
/// run on a worker thread behind a progress dialog (parented to @p parent) with Cancel; a
/// single-file project then copies the files in (MediaStore, as Import Media does).
///
/// **Cancel rolls back:** nothing is added to the pool when the user cancels the scan, the
/// probe or the copy into a single-file project (result.cancelled). Bins are only created for
/// folders that receive at least one item (directly or below). Shows no other dialogs.
/// GUI thread only.
FolderImportResult importMediaFolders(AppContext& ctx, const QStringList& folders,
    const FolderImportOptions& options, QWidget* parent, const QString& binId = {});

/// importMediaFolders() followed by one summary dialog when something was not imported
/// (failures, duplicates) or nothing was found. Returns the new item ids.
/// @p binId: the bin ("" = Master); none = importTargetBin() (ui/common/MediaImport.h).
QStringList importMediaFoldersWithFeedback(AppContext& ctx, const QStringList& folders,
    const FolderImportOptions& options, QWidget* parent,
    const std::optional<QString>& binId = std::nullopt);

/// File > Import Media Folder… and the media pool's context menu: asks for a folder (the
/// platform folder chooser), then for the options (askFolderImportOptions()), then imports it
/// with feedback into bin @p binId. Returns the new item ids (empty when cancelled).
QStringList importMediaFolderWithDialogs(AppContext& ctx, QWidget* parent, const QString& binId);

/// Shows the small options dialog (Include subfolders, Create bins for subfolders), starting
/// from the last choice (remembered in QSettings, both on by default). Returns false if
/// cancelled; on OK stores the choice and fills @p options.
bool askFolderImportOptions(QWidget* parent, const QString& folder, FolderImportOptions* options);

/// Tests: while set, askFolderImportOptions() calls @p handler instead of showing the dialog
/// (return false = cancelled). Pass an empty function to restore the dialog.
void setFolderImportOptionsHandlerForTests(std::function<bool(FolderImportOptions*)> handler);

/// Moves the existing folders out of @p paths (dropped or chosen paths) and returns them, in
/// order; files stay in @p paths. One stat per path.
[[nodiscard]] QStringList takeFolders(QStringList& paths);

} // namespace kk
