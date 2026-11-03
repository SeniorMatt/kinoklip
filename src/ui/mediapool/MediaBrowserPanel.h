// SPDX-License-Identifier: GPL-3.0-or-later
// MediaBrowserPanel — file-system browser of the Media page: places, folder tree, media file
// list and "Add to Media Pool".
#pragma once

#include "ui/common/PanelFrame.h"

#include <QStringList>

class QComboBox;
class QFileSystemModel;
class QPushButton;
class QSortFilterProxyModel;
class QTreeView;

namespace kk {

struct AppContext;

/// Browses the local file system (Media page, left column).
///
/// - A places combo (Home, Videos, Pictures, Music, Desktop, file system root) picks the root
///   of the folder tree; selecting a folder lists its media files (only importable extensions,
///   see mediaFileNamePatterns()) with size and date.
/// - Selecting a file emits fileSelected() (preview); double-click, the button or the context
///   menu adds the selected files to the media pool (one undo step).
/// - The folder tree's context menu adds a whole folder: "Add Folder into Media Pool" (its own
///   files) and "Add Folder and Subfolders into Media Pool (Create Bins)" (FolderImport.h).
///
/// Both QFileSystemModels read the disk on Qt's own worker thread, so the GUI never blocks.
/// Thread-safety: GUI thread only.
class MediaBrowserPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Creates the browser rooted at the user's home directory.
    MediaBrowserPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Changes the root of the folder tree (and lists that folder).
    void setRootPath(const QString& path);
    /// Lists the media files of folder @p path (selects it in the tree if visible).
    void showFolder(const QString& path);
    /// Folder whose files are listed.
    [[nodiscard]] QString currentFolder() const { return m_folder; }
    /// Absolute paths of the selected files.
    [[nodiscard]] QStringList selectedFiles() const;
    /// Imports the selected files into the media pool. Returns the new media ids.
    QStringList addSelectedToMediaPool();
    /// Imports folder @p path into the media pool (Master): only its own files, or with
    /// @p withSubfolders its subfolders too, as a bin named after it with sub-bins. One undo
    /// step. Returns the new media ids.
    QStringList addFolderToMediaPool(const QString& path, bool withSubfolders);

signals:
    /// A file was selected (e.g. to preview it in the viewer).
    void fileSelected(const QString& path);

private:
    void updateButton();

    AppContext& m_ctx;
    QComboBox* m_places = nullptr;
    QFileSystemModel* m_dirs = nullptr;
    QFileSystemModel* m_files = nullptr;
    QSortFilterProxyModel* m_filesProxy = nullptr; ///< hides folders from the file list
    QTreeView* m_tree = nullptr;
    QTreeView* m_list = nullptr;
    QPushButton* m_add = nullptr;
    QString m_folder;
};

} // namespace kk
