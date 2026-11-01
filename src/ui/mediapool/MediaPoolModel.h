// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolModel — Qt item model over the project's MediaPool (table: name, duration,
// resolution, fps, codec, audio) with lazily requested poster frames; plus the bin/search
// filter proxy used by the media pool views.
#pragma once

#include <QAbstractTableModel>
#include <QCache>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QSortFilterProxyModel>

#include <functional>

namespace kk {

class Project;
struct MediaItem;

/// Returns a poster frame for @p path about @p height pixels high, or a null image if it is not
/// ready yet (the provider must then report it later through MediaPoolModel::thumbnailReady()).
/// Must not block: it is called while painting.
using ThumbnailProvider = std::function<QImage(const QString& path, int height)>;

/// Table model of all media items, in import order.
///
/// Performance (2,000+ items):
/// - Display strings are formatted once per item change and cached in the row (data() does no
///   formatting), the offline flag is checked once per add/reset (no per-paint file I/O).
/// - Poster frames are requested from the ThumbnailProvider only when a view paints the item,
///   pre-scaled once to the tile size and kept in a byte-bounded QCache.
///
/// Edits made through the model (rename, drop into bin) are undoable Project edits.
/// Thread-safety: GUI thread only.
class MediaPoolModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    /// Columns of the list view.
    enum Column {
        NameColumn,
        DurationColumn,
        ResolutionColumn,
        FpsColumn,
        CodecColumn,
        AudioColumn,
        ColumnCount
    };

    /// Extra roles.
    enum Role {
        IdRole = Qt::UserRole + 1, ///< QString MediaItem::id
        BinRole, ///< QString MediaItem::binId
        OfflineRole, ///< bool: the file is missing
        PosterRole, ///< QPixmap poster (null while pending / for audio)
        KindRole, ///< int MediaKind
        DurationTextRole, ///< QString duration timecode ("" for stills)
        SearchTextRole ///< QString lower-case "name path" for filtering
    };

    /// Coarse media kind, for placeholder glyphs.
    enum MediaKind {
        VideoKind,
        AudioKind,
        ImageKind
    };

    /// Size poster frames are scaled to fit (16:9 tile).
    static constexpr QSize kPosterSize {144, 81};

    /// Creates the model for @p project's media pool.
    explicit MediaPoolModel(Project* project, QObject* parent = nullptr);

    /// Sets the poster frame provider (default: none = placeholders only).
    void setThumbnailProvider(ThumbnailProvider provider);
    /// A poster frame for @p path became available: drops the pending mark and repaints rows.
    void thumbnailReady(const QString& path);
    /// Re-checks which files are offline (e.g. when the application becomes active).
    void recheckOffline();

    /// Row of media item @p id, or -1.
    [[nodiscard]] int rowOf(const QString& id) const { return m_rowById.value(id, -1); }
    /// Media item id of @p row.
    [[nodiscard]] QString idAt(int row) const;

    // ---- QAbstractItemModel ----
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation, int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    [[nodiscard]] Qt::DropActions supportedDragActions() const override;

private:
    /// Everything the views show, formatted once.
    struct Row
    {
        QString id;
        QString name;
        QString path;
        QString binId;
        QString durationText;
        QString resolutionText;
        QString fpsText;
        QString codecText;
        QString audioText;
        QString searchText;
        MediaKind kind = VideoKind;
        bool offline = false;
    };

    static Row makeRow(const MediaItem& item);
    void reload();
    void onItemAdded(const QString& id);
    void onItemRemoved(const QString& id);
    void onItemChanged(const QString& id);
    void reindex(int fromRow);
    QPixmap poster(const Row& row) const;

    Project* m_project = nullptr;
    QVector<Row> m_rows;
    QHash<QString, int> m_rowById;
    ThumbnailProvider m_provider;
    // Poster cache (mutable: filled lazily from the const data()).
    mutable QCache<QString, QPixmap> m_posters; ///< path → scaled poster; cost in KiB
    mutable QSet<QString> m_pending; ///< paths requested but not delivered yet
};

/// Filters MediaPoolModel rows by bin and search text.
///
/// Without search text only the items directly in the current bin are shown; with search text
/// the current bin and all of its sub-bins are searched (name and path, case-insensitive).
class MediaPoolFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    /// Creates the proxy; call setSourceModel() with a MediaPoolModel.
    explicit MediaPoolFilterModel(Project* project, QObject* parent = nullptr);

    /// Shows bin @p binId ("" = Master / root).
    void setBin(const QString& binId);
    /// Current bin id.
    [[nodiscard]] QString bin() const { return m_bin; }
    /// Sets the search text (empty = no search).
    void setSearchText(const QString& text);
    /// Recomputes the bin set after bins changed.
    void binsChanged();

protected:
    [[nodiscard]] bool filterAcceptsRow(
        int sourceRow, const QModelIndex& sourceParent) const override;

private:
    void updateBinSet();

    Project* m_project = nullptr;
    QString m_bin;
    QString m_search; ///< lower case
    QSet<QString> m_bins; ///< bins whose items are accepted
};

} // namespace kk
