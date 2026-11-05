// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolModel — Qt item model over the project's MediaPool and the power bins (table: name,
// duration, resolution, fps, codec, audio, clip colour, flags, usage, metadata columns) with
// lazily requested poster frames; plus the bin / smart bin / search / colour filter proxy used
// by the media pool views.
#pragma once

#include "core/MediaItem.h"
#include "core/MediaUsage.h"
#include "ui/common/MediaManagement.h"

#include <QAbstractTableModel>
#include <QCache>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTimer>

#include <functional>

namespace kk {

class MediaPool;
class Project;

/// Drag payload of power bin items (UTF-8, newline-separated power bin item ids), next to
/// mime::MediaItems in drags that start in a power bin. Only the media pool reads it.
inline constexpr char kPowerItemsMime[] = "application/x-kinoklip-power-ids";

/// Maps power bin item ids to project media pool item ids (adding copies to the project when
/// needed). Used lazily by drags, see MediaPoolModel::setPowerResolver().
using PowerResolver = std::function<QStringList(const QStringList& powerIds)>;

/// Returns a poster frame for @p path about @p height pixels high, or a null image if it is not
/// ready yet (the provider must then report it later through MediaPoolModel::thumbnailReady()).
/// Must not block: it is called while painting.
using PosterProvider = std::function<QImage(const QString& path, int height)>;

/// Table model of all media items, in import order: one folder row per project bin
/// (FolderRole; the filter shows the sub-bins of the current bin before its clips, as in
/// Resolve), the project's items, then the power bin items (setPowerBins(); PowerRole = true).
///
/// Usage (UsageColumn, UsageRole, smart bin "Usage" rules) is recounted over every timeline
/// shortly after the timelines or the pool change (coalesced: one count per burst of changes,
/// e.g. a drag), and only rows whose count changed are reported.
///
/// Performance (2,000+ items):
/// - Display strings are formatted once per item change and cached in the row (data() does no
///   formatting), the offline flag is checked once per add/reset (no per-paint file I/O).
/// - Poster frames are requested from the PosterProvider only when a view paints the item,
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
        ProxyColumn, ///< proxy media: "None", "960x540", "Generating 42%", "Stale"...
        OptimizedColumn, ///< optimized media, same texts
        // ---- Organisation and metadata (bins branch); hidden by default except colour,
        // flags and usage ----
        ClipColorColumn, ///< clip colour name with a swatch
        FlagsColumn, ///< flag names with swatches
        UsageColumn, ///< number of timeline uses
        FileNameColumn,
        PathColumn,
        DateAddedColumn,
        DateModifiedColumn,
        DescriptionColumn,
        CommentsColumn,
        KeywordsColumn,
        SceneColumn,
        ShotColumn,
        TakeColumn,
        CameraColumn,
        AngleColumn,
        GoodTakeColumn,
        RatingColumn,
        /// "Project file" (stored inside a single-file project) or "Linked" (a file on disk)
        LocationColumn,
        /// kind of item: "Bin", "Timeline", "Video", "Audio", "Still", "Compound Clip"...;
        /// sorts by kind (Sort by Type), then by name
        TypeColumn,
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
        SearchTextRole, ///< QString lower-case "name path" for filtering
        LinkBadgeRole, ///< QString proxy / optimized badge ("P", "O", "P 42%", "P!", "")
        LinkProblemRole, ///< bool: the badge is a warning (stale / missing)
        ClipColorRole, ///< QColor clip colour (invalid = none)
        ClipColorIdRole, ///< QString palette id of the clip colour
        FlagsRole, ///< uint flag bits (core/ClipColors.h)
        UsageRole, ///< int number of timeline uses
        PowerRole, ///< bool: a power bin item (shared by every project)
        LocationRole, ///< int Location of the item's media
        /// int Location for the thumbnail badge: set only in a single-file project (where
        /// both kinds can be mixed), NoLocation otherwise
        LocationBadgeRole,
        SortRole, ///< raw value of the column for sorting (numbers, ms, lower-case text)
        FolderRole, ///< bool: a folder row (a project bin shown in its parent bin's items)
        FolderBinRole ///< QString bin id of a folder row ("" for media rows)
    };

    /// Coarse media kind, for placeholder glyphs.
    enum MediaKind {
        VideoKind,
        AudioKind,
        ImageKind,
        TimelineKind, ///< a timeline of the project (double-click opens it in a tab)
        CompoundKind, ///< a compound clip (its timeline)
        MulticamKind, ///< a multicam clip (its timeline; opens in the source viewer)
        SubtitleKind, ///< an imported subtitle file (drag onto the timeline for captions)
        FolderKind ///< a folder row (a sub-bin; double-click opens it)
    };

    /// Where an item's media is (LocationRole).
    enum Location {
        NoLocation, ///< timelines, compound and multicam clips
        StoredLocation, ///< stored inside the open single-file project
        LinkedLocation ///< a file on disk
    };

    /// Default size poster frames are scaled to fit (16:9 tile; setPosterSize() changes it).
    static constexpr QSize kPosterSize {144, 81};

    /// Creates the model for @p project's media pool.
    explicit MediaPoolModel(Project* project, QObject* parent = nullptr);

    /// Also lists the items of the power bins @p power (null = none).
    void setPowerBins(MediaPool* power);
    /// The power bins pool, or nullptr.
    [[nodiscard]] MediaPool* powerBins() const { return m_power; }
    /// Called when a drag of power bin items is dropped somewhere that reads mime::MediaItems
    /// (the timeline, the source viewer): the items are copied into the project then, not
    /// when the drag starts, so moving them between power bins never touches the project.
    void setPowerResolver(PowerResolver resolver) { m_powerResolver = std::move(resolver); }
    /// The pool that holds item @p id (project or power bins), or nullptr.
    [[nodiscard]] MediaPool* poolOf(const QString& id) const;
    /// The item @p id from either pool, or nullptr.
    [[nodiscard]] const MediaItem* item(const QString& id) const;

    /// Current usage counts (media item id -> timeline uses).
    [[nodiscard]] const usage::Counts& usageCounts() const { return m_usage; }
    /// Recounts the usage now (normally done automatically, coalesced).
    void refreshUsage();

    /// Stable key of column @p column for settings and project files ("name", "clipColor", ...).
    [[nodiscard]] static QString columnKey(int column);
    /// Column with key @p key, or -1.
    [[nodiscard]] static int columnForKey(const QString& key);
    /// True for the columns shown in a new list view.
    [[nodiscard]] static bool columnVisibleByDefault(int column);

    /// Sets the poster frame provider (default: none = placeholders only).
    void setThumbnailProvider(PosterProvider provider);
    /// Scales posters to fit @p size (the grid's thumbnail size). Posters are requested at
    /// least at kPosterSize's height, so zooming out reuses the frames already decoded.
    void setPosterSize(QSize size);
    /// The size posters are scaled to fit.
    [[nodiscard]] QSize posterSize() const { return m_posterSize; }
    /// A poster frame for @p path became available: drops the pending mark and repaints rows.
    void thumbnailReady(const QString& path);
    /// Re-checks which files are offline (e.g. when the application becomes active).
    void recheckOffline();
    /// Sets where proxy / optimized media status comes from (src/ui/common/MediaManagement;
    /// default: none = empty columns, no badges).
    void setLinkStatusProvider(std::function<MediaLinkStatus(const QString& id)> provider);
    /// Re-reads the link status of item @p id (empty = every item).
    void linkStatusChanged(const QString& id = {});

    /// Row of media item @p id, or -1.
    [[nodiscard]] int rowOf(const QString& id) const { return m_rowById.value(id, -1); }
    /// Media item id of @p row ("" for a folder row).
    [[nodiscard]] QString idAt(int row) const;
    /// Row of the folder of project bin @p binId, or -1.
    [[nodiscard]] int folderRowOf(const QString& binId) const;
    /// Number of folder rows (they come first).
    [[nodiscard]] int folderCount() const { return m_folderCount; }

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
        QString colorText; ///< clip colour name
        QString flagsText; ///< flag names, comma separated
        QString fileName;
        QString addedText;
        QString modifiedText;
        QString keywordsText;
        MediaMetadata metadata;
        QString clipColorId;
        QColor clipColor;
        quint32 flags = 0;
        int usage = 0;
        qint64 durationUs = 0;
        double fps = 0;
        qint64 pixels = 0;
        qint64 addedMs = 0;
        qint64 modifiedMs = 0;
        MediaKind kind = VideoKind;
        bool offline = false;
        bool power = false; ///< item of the power bins
        Location location = NoLocation;
        MediaLinkStatus links; ///< from the link status provider (cached)
        bool folder = false; ///< a folder row: binId = parent bin, folderBin = the bin
        QString folderBin;
    };

    static Row makeRow(const MediaItem& item, bool power);
    static Row makeFolderRow(const Bin& bin);
    /// Folder rows for the project's bins, in bin order.
    [[nodiscard]] QVector<Row> folderRows() const;
    /// The project's bins changed: replaces the folder rows (or updates them in place when
    /// the same bins remain).
    void rebuildFolders();
    /// Items or bins changed: the folder item counts are recounted when next shown.
    void folderCountsChanged();
    [[nodiscard]] int folderItemCount(const Row& row) const;
    /// True while the open project is a single-file project (location badges shown).
    [[nodiscard]] bool singleFileProject() const;
    [[nodiscard]] QVariant displayText(const Row& row, int column) const;
    [[nodiscard]] QVariant sortValue(const Row& row, int column) const;
    void connectPool(MediaPool* pool, bool power);
    void scheduleUsage();
    void fillLinks(Row& row) const;
    void reload();
    void onItemAdded(const QString& id);
    void onItemRemoved(const QString& id);
    void onItemChanged(const QString& id);
    void reindex(int fromRow);
    QPixmap poster(const Row& row) const;

    Project* m_project = nullptr;
    MediaPool* m_power = nullptr;
    PowerResolver m_powerResolver;
    usage::Counts m_usage;
    QTimer m_usageTimer; ///< coalesces usage recounts
    QVector<Row> m_rows;
    QHash<QString, int> m_rowById;
    PosterProvider m_provider;
    QSize m_posterSize = kPosterSize;
    std::function<MediaLinkStatus(const QString&)> m_linkStatus;
    // Poster cache (mutable: filled lazily from the const data()).
    mutable QCache<QString, QPixmap> m_posters; ///< path → scaled poster; cost in KiB
    mutable QSet<QString> m_pending; ///< paths requested but not delivered yet
    int m_folderCount = 0; ///< folder rows at the start of m_rows
    mutable QHash<QString, int> m_folderCounts; ///< bin id -> items + sub-bins directly inside
    mutable bool m_folderCountsDirty = true;
};

/// Filters MediaPoolModel rows by bin (or smart bin), search text and clip colour.
///
/// Folder rows (the sub-bins of the current project bin) are shown first, whatever the sort
/// (Descending reverses the folders among themselves but keeps them first), only in project
/// bins (never in smart or power bins), and with a search only when their name matches; a
/// clip colour filter hides them.
///
/// Bin keys: "" = Master, a project bin id, "smart:<id>" (a user or default smart bin: every
/// project item matching its rules), "power:" (the power bins root) or "power:<id>".
/// Without search text only the items directly in the current bin are shown; with search text
/// the current bin and all of its sub-bins are searched (name, path, clip colour and flag
/// names, keywords, comments and the other metadata; case-insensitive). Smart bins re-filter
/// whenever an item, a rule or the usage changes.
class MediaPoolFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    /// Key prefixes of smart bins and power bins.
    static constexpr char kSmartPrefix[] = "smart:";
    static constexpr char kPowerPrefix[] = "power:";

    /// Creates the proxy; call setSourceModel() with a MediaPoolModel.
    explicit MediaPoolFilterModel(Project* project, QObject* parent = nullptr);

    /// Shows the bin with key @p binKey (see the class comment).
    void setBin(const QString& binKey);
    /// Current bin key.
    [[nodiscard]] QString bin() const { return m_bin; }
    /// Sets the search text (empty = no search).
    void setSearchText(const QString& text);
    /// Shows only items with clip colour @p colorId ("" = every item, "none" = no colour).
    void setColorFilter(const QString& colorId);
    /// Current clip colour filter.
    [[nodiscard]] QString colorFilter() const { return m_color; }
    /// Recomputes the bin set after bins changed.
    void binsChanged();
    /// Re-evaluates a smart bin after its rules or the default smart bin preference changed.
    void smartBinsChanged();

    /// The smart bin behind key @p key (user or default), or nullptr.
    [[nodiscard]] const SmartBin* smartBinFor(const QString& key) const;
    /// True if @p key names a smart bin / a power bin.
    [[nodiscard]] static bool isSmartKey(const QString& key);
    [[nodiscard]] static bool isPowerKey(const QString& key);
    /// The bin id inside a key ("smart:x" -> "x", "power:" -> "", "b" -> "b").
    [[nodiscard]] static QString idOfKey(const QString& key);

protected:
    [[nodiscard]] bool filterAcceptsRow(
        int sourceRow, const QModelIndex& sourceParent) const override;
    [[nodiscard]] bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    void updateBinSet();
    /// Re-runs filterAcceptsRow() for every row after m_bin / m_search changed.
    void refilter();

    Project* m_project = nullptr;
    QString m_bin;
    QString m_search; ///< lower case
    QString m_color;
    QSet<QString> m_bins; ///< bins whose items are accepted (ids without prefix)
    QVector<SmartBin> m_defaults; ///< the default smart bins (names translated once)
};

} // namespace kk
