// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectLibraryModel — one folder of the project library (or the Recent list) as a table
// for the Project Manager's card and list views; ProjectSortProxy sorts and searches it.
#pragma once

#include "projects/ProjectLibrary.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStringList>

namespace kk {

/// Rows = folders and projects of the shown location. Metadata and posters come lazily from
/// the ProjectLibrary (asked for in data(), i.e. only for rows a view paints); posters missing
/// from the library cache are made from the media cache's thumbnail of the poster frame and
/// stored back into the library. Renaming (EditRole on the name column) and drag and drop
/// (moving between folders, importing project files dropped from outside) go through the
/// library; failures are reported with operationFailed().
class ProjectLibraryModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column {
        NameColumn,
        ModifiedColumn,
        DurationColumn,
        ResolutionColumn,
        FpsColumn,
        KindColumn,
        LocationColumn,
        ColumnCount
    };
    enum Role {
        PathRole = Qt::UserRole + 1, ///< QString, absolute path
        IsFolderRole, ///< bool
        PosterRole, ///< QImage (null while unknown / none)
        ErrorRole, ///< QString, non-empty for an unreadable project
        MissingMediaRole, ///< int, offline media files
        IsOpenRole, ///< bool, the project open in the editor
        ModifiedTextRole, ///< QString, "last modified" line of a card
        DetailsRole, ///< QString, "duration · resolution · fps" line of a card
        SortRole, ///< raw value of the column for sorting
        ExistsRole, ///< bool, false for a missing recent project
        LoadedRole, ///< bool, metadata known (or the row is a folder)
        KindBadgeRole ///< QString, ProjectKind::badge ("Linked", "Single file"); folders: empty
    };
    static constexpr int kPosterHeight = 144; ///< px; cards scale it

    explicit ProjectLibraryModel(ProjectLibrary* library, QObject* parent = nullptr);

    /// Shows the folders and projects of @p folder (absolute; empty = the library root).
    void showFolder(const QString& folder);
    /// Shows the recent projects @p paths (any location, missing files marked).
    void showRecent(const QStringList& paths);
    /// Re-reads the shown location (after operations, or changes made by a file manager).
    void refresh();
    /// True while the Recent list is shown.
    [[nodiscard]] bool isRecent() const { return m_recent; }
    /// Shown folder (absolute; the library root when Recent is shown).
    [[nodiscard]] QString folder() const { return m_folder; }
    /// The entry of @p row.
    [[nodiscard]] const ProjectEntry& entry(int row) const { return m_entries.at(row); }
    /// Row of @p path, or -1.
    [[nodiscard]] int rowOf(const QString& path) const { return m_rowOf.value(path, -1); }
    [[nodiscard]] ProjectLibrary* library() const { return m_library; }

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
        const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
        const QModelIndex& parent) override;
    Qt::DropActions supportedDropActions() const override;
    Qt::DropActions supportedDragActions() const override;

    /// Moves (library items) or imports (outside project files) @p paths into @p folder.
    /// Returns the number of items handled; failures emit operationFailed().
    int dropPaths(const QStringList& paths, const QString& folder);

    /// Mime type of library paths dragged inside the Project Manager.
    static constexpr char kMimeType[] = "application/x-kinoklip-library-paths";

signals:
    /// An operation started from the model (rename, drop) failed with @p message.
    void operationFailed(const QString& message);

private:
    void reload();
    void rowChanged(const QString& path);
    void mediaThumbnailReady(const QString& mediaPath);
    QImage posterFor(const ProjectEntry& e) const;

    ProjectLibrary* m_library;
    QString m_folder;
    bool m_recent = false;
    QStringList m_recentPaths;
    QVector<ProjectEntry> m_entries;
    QHash<QString, int> m_rowOf;
    /// media path -> projects waiting for that file's thumbnail from the media cache
    mutable QHash<QString, QSet<QString>> m_waitingThumbnails;
};

/// Sorting and searching over a ProjectLibraryModel: folders always come first (in both sort
/// orders), the search text filters by name (case-insensitive substring).
class ProjectSortProxy : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit ProjectSortProxy(QObject* parent = nullptr);
    /// Shows only the rows whose name contains @p text (empty = all).
    void setSearchText(const QString& text);

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QString m_search;
};

} // namespace kk
