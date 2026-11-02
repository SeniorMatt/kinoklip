// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPool — QObject holding the project's imported media items and bins.
#pragma once

#include "core/MediaItem.h"

#include <QHash>
#include <QObject>
#include <QStringList>

namespace kk {

/// Outcome of MediaPool::importFiles().
struct ImportResult
{
    QStringList importedIds; ///< ids of the new MediaItems, in input order
    QStringList warnings; ///< one human-readable line per skipped file
};

/// The project's media pool: imported files (MediaItem) organised in bins (Bin).
///
/// Changes made through this class are not undoable on their own; wrap them in
/// Project::beginEdit()/endEdit() (Project::importMedia() does that).
/// Thread-safety: GUI thread only.
class MediaPool : public QObject
{
    Q_OBJECT
public:
    /// Creates an empty pool.
    explicit MediaPool(QObject* parent = nullptr);

    /// All items, in import order.
    [[nodiscard]] const QVector<MediaItem>& items() const { return m_state.items; }
    /// All bins (the implicit root bin with id "" is not listed).
    [[nodiscard]] const QVector<Bin>& bins() const { return m_state.bins; }
    /// Item with id @p id, or nullptr. Invalidated by the next change.
    [[nodiscard]] const MediaItem* item(const QString& id) const;
    /// Bin with id @p id, or nullptr (also for the root id "").
    [[nodiscard]] const Bin* bin(const QString& id) const;
    /// Ids of the items directly inside bin @p binId ("" = root).
    [[nodiscard]] QStringList itemIdsInBin(const QString& binId) const;
    /// File item whose path is @p path, or nullptr.
    [[nodiscard]] const MediaItem* itemByPath(const QString& path) const;
    /// Ids of the direct sub-bins of @p parentId ("" = root), in order.
    [[nodiscard]] QStringList childBins(const QString& parentId) const;
    /// True if bin @p binId is @p ancestorId or lies inside it ("" = root contains every bin).
    [[nodiscard]] bool binIsInside(const QString& binId, const QString& ancestorId) const;

    /// Probes and adds each file to bin @p binId. Files that fail media::probe() or are already
    /// in the pool are skipped and reported in ImportResult::warnings.
    ImportResult importFiles(const QStringList& paths, const QString& binId = {});
    /// Adds a ready-made item (id assigned if empty or taken; name defaults to the file name).
    /// Returns the item's id.
    QString addItem(MediaItem item);
    /// Removes an item. Returns false if it does not exist. Clips using it become offline.
    bool removeItem(const QString& id);
    /// Renames an item. Returns false if it does not exist.
    bool renameItem(const QString& id, const QString& name);
    /// Replaces name and info of item @p id (timeline items, kept in sync by the Project).
    /// Emits itemChanged() only when something differs. Returns false if it does not exist.
    bool updateItem(const QString& id, const QString& name, const media::MediaInfo& info);
    /// Relinks offline items: every item whose file is missing and whose file name exists in
    /// @p folder points there afterwards (the cached probe result is kept; the media must be
    /// the same file, as with Resolve's "Relink Clips"). Returns the number relinked.
    int relinkFromFolder(const QString& folder);
    /// Moves an item into bin @p binId ("" = root). Returns false if item or bin is unknown.
    bool moveItemToBin(const QString& itemId, const QString& binId);

    /// Creates a bin named @p name inside @p parentId ("" = root). Returns the new bin id.
    QString addBin(const QString& name, const QString& parentId = {});
    /// Removes a bin; its items and sub-bins move to its parent. Returns false if unknown.
    bool removeBin(const QString& id);
    /// Renames a bin. Returns false if unknown.
    bool renameBin(const QString& id, const QString& name);
    /// Moves bin @p id into @p parentId ("" = root) at position @p index among the new
    /// siblings (-1 or past the end = last). Refused (false) for an unknown bin or parent and
    /// when @p parentId is @p id or one of its sub-bins (a bin cannot contain itself).
    bool moveBin(const QString& id, const QString& parentId, int index = -1);
    /// Deletes bin @p id with its sub-bins and every media file item inside them (Resolve's
    /// Delete Bin). Timeline / compound clip items inside move to the root bin: timelines are
    /// only deleted explicitly. Returns the ids of the removed items (empty if the bin is
    /// unknown, which also leaves the pool unchanged).
    QStringList removeBinWithContents(const QString& id);

    // ---- Clip colour, flags, metadata ----

    /// Sets the clip colour of item @p id (palette id, empty = none). False if unknown.
    bool setClipColor(const QString& id, const QString& colorId);
    /// Replaces the flags of item @p id (bit set, see core/ClipColors.h). False if unknown.
    bool setFlags(const QString& id, quint32 flags);
    /// Replaces the user metadata of item @p id (keywords are trimmed and de-duplicated,
    /// the rating is clamped to 0 - 5). False if unknown.
    bool setMetadata(const QString& id, MediaMetadata metadata);

    // ---- Smart bins ----

    /// User smart bins, in list order.
    [[nodiscard]] const QVector<SmartBin>& smartBins() const { return m_state.smartBins; }
    /// Smart bin with id @p id, or nullptr.
    [[nodiscard]] const SmartBin* smartBin(const QString& id) const;
    /// Appends a smart bin (id assigned when empty or taken). Returns its id.
    QString addSmartBin(SmartBin bin);
    /// Replaces the smart bin with the same id. False if unknown.
    bool updateSmartBin(const SmartBin& bin);
    /// Removes a smart bin (items are not touched). False if unknown.
    bool removeSmartBin(const QString& id);

    /// Copy of the complete contents (for undo snapshots and saving).
    [[nodiscard]] const MediaPoolState& state() const { return m_state; }
    /// Replaces the complete contents and emits reset() + changed().
    void setState(MediaPoolState state);
    /// Removes everything.
    void clear() { setState({}); }
    /// Monotonic counter incremented on every change.
    [[nodiscard]] quint64 revision() const { return m_revision; }

signals:
    /// A new item with id @p id was added.
    void itemAdded(const QString& id);
    /// The item with id @p id was removed.
    void itemRemoved(const QString& id);
    /// Name or bin of item @p id changed.
    void itemChanged(const QString& id);
    /// A bin was added, removed, renamed or moved.
    void binsChanged();
    /// A smart bin was added, removed or edited.
    void smartBinsChanged();
    /// The whole contents were replaced (load, undo, new project).
    void reset();
    /// Emitted after every change of any kind.
    void changed();

private:
    int indexOfItem(const QString& id) const;
    int indexOfBin(const QString& id) const;
    void touch();
    /// The item index (m_index) must be rebuilt: call after adding / removing items.
    void invalidateIndex() { m_indexValid = false; }

    MediaPoolState m_state;
    quint64 m_revision = 0;
    // id -> position in m_state.items, rebuilt lazily after structural changes: item() is
    // called per painted timeline clip and per media pool row, so it must not be a scan.
    mutable QHash<QString, int> m_index;
    mutable bool m_indexValid = false;
};

} // namespace kk
