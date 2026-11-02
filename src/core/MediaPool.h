// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPool — QObject holding the project's imported media items and bins.
#pragma once

#include "core/MediaItem.h"

#include <QObject>
#include <QStringList>

namespace kk {

/// Probe-like description of a subtitle file (valid, path, duration = end of the last cue,
/// container = suffix; no video or audio).
[[nodiscard]] media::MediaInfo subtitleInfo(const QString& path, const QVector<SubtitleCue>& cues);

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
    /// Replaces the Clip Attributes of item @p id (call inside a Project edit: the clips of
    /// the item follow in Project::syncDerived()). Returns false if it does not exist.
    bool setItemAttributes(const QString& id, const ClipAttributes& attributes);
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
    /// A bin was added, removed or renamed.
    void binsChanged();
    /// The whole contents were replaced (load, undo, new project).
    void reset();
    /// Emitted after every change of any kind.
    void changed();

private:
    int indexOfItem(const QString& id) const;
    int indexOfBin(const QString& id) const;
    void touch();

    MediaPoolState m_state;
    quint64 m_revision = 0;
};

} // namespace kk
