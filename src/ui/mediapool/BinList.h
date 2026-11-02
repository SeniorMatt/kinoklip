// SPDX-License-Identifier: GPL-3.0-or-later
// BinList — the media pool's bin list sidebar: Master with the project's bins, the smart bins
// and the power bins, with drag and drop to reorder / re-parent bins and to move items.
#pragma once

#include "core/MediaItem.h"

#include <QRect>
#include <QTreeWidget>

#include <functional>

class QMimeData;

namespace kk {

class MediaPool;

/// The bin list (Resolve layout): "Master" (project root) with the project's bins as a tree,
/// a "Smart Bins" section and a "Power Bins" section (root of the power bins, shared by every
/// project). Every row carries a bin key (MediaPoolFilterModel keys: "" = Master, a bin id,
/// "smart:<id>", "power:" = power root, "power:<id>").
///
/// Drag and drop:
/// - a bin dragged onto another bin of the same section becomes its sub-bin; dropped on the
///   upper / lower edge of a row it is placed before / after that row (same parent);
/// - media items dropped on a bin are moved (or copied between project and power bins);
/// - files from the file manager dropped on a bin are imported into it.
/// The widget only reports what was dropped (callbacks); the media pool panel performs the
/// undoable operations and calls rebuild().
/// Thread-safety: GUI thread only.
class BinList final : public QTreeWidget
{
    Q_OBJECT
public:
    /// What a row stands for.
    enum class RowKind {
        Master, ///< the project's root bin
        ProjectBin,
        SmartHeader, ///< the "Smart Bins" section title (not a bin)
        SmartBin,
        PowerRoot, ///< the "Power Bins" section: root of the power bins
        PowerBin
    };
    /// Where a bin drop goes relative to the target row.
    enum class DropPlace {
        Into,
        Before,
        After
    };

    /// Item role holding the bin key (QString).
    static constexpr int kKeyRole = Qt::UserRole;
    /// Item role holding the RowKind (int).
    static constexpr int kKindRole = Qt::UserRole + 1;
    /// MIME type of a dragged bin (UTF-8 bin key).
    static constexpr char kBinMime[] = "application/x-kinoklip-bin-key";

    explicit BinList(QWidget* parent = nullptr);

    /// Rebuilds the rows from @p project bins, @p smart bins (defaults first) and the @p power
    /// bins (null = no power section). Keeps the current key and the collapsed rows.
    void rebuild(const MediaPool& project, const QVector<SmartBin>& smart, const MediaPool* power);
    /// Key of the current row ("" = Master).
    [[nodiscard]] QString currentKey() const;
    /// Makes the row with @p key current without emitting keyActivated(). False if unknown.
    bool setCurrentKey(const QString& key);
    /// Row of bin @p key, or nullptr.
    [[nodiscard]] QTreeWidgetItem* itemForKey(const QString& key) const;
    /// True while rebuild() runs (item signals then are not user edits).
    [[nodiscard]] bool isRebuilding() const { return m_rebuilding; }
    /// Kind of @p item.
    [[nodiscard]] static RowKind kindOf(const QTreeWidgetItem* item);

    /// Media items (mime::MediaItems ids; power bin items under kPowerItemsMime) dropped on @p key.
    std::function<void(const QStringList& ids, const QStringList& powerIds, const QString& key)>
        onItemsDropped;
    /// Files from the OS dropped on @p key.
    std::function<void(const QStringList& paths, const QString& key)> onFilesDropped;
    /// Bin @p key dropped @p place relative to bin @p targetKey.
    std::function<void(const QString& key, const QString& targetKey, DropPlace place)> onBinDropped;

signals:
    /// The user picked another bin (click, keyboard).
    void keyActivated(const QString& key);

protected:
    [[nodiscard]] QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    [[nodiscard]] QStringList mimeTypes() const override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    /// Target row and place for a drop at @p pos, or nullptr when it cannot be dropped there.
    QTreeWidgetItem* dropTarget(const QMimeData* data, const QPoint& pos, DropPlace* place) const;
    QTreeWidgetItem* addRow(QTreeWidgetItem* parent, const QString& text, const QString& key,
        RowKind kind, const QString& iconName);

    bool m_rebuilding = false;
    QRect m_dropLine; ///< insertion line of a bin drag (viewport coordinates), empty = none
};

} // namespace kk
