// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPanel — the project's media: bins tree + items in a poster grid or a detail list,
// search, import (button, OS drops), rename/delete/reveal, drag source for the timeline.
#pragma once

#include "ui/common/PanelFrame.h"
#include "ui/mediapool/MediaPoolModel.h"

#include <QStringList>

class QAbstractItemView;
class QItemSelectionModel;
class QLineEdit;
class QListView;
class QSplitter;
class QStackedWidget;
class QToolButton;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;

namespace kk {

struct AppContext;

/// The media pool panel (Edit page left column, Media page bottom).
///
/// - Left: bins tree ("Master" root + user bins). Create / rename / delete bins from the context
///   menu or the title bar; drop media items on a bin to move them; drop files from the OS on a
///   bin to import them there.
/// - Right: items of the current bin, as a grid of poster frames or a list with columns
///   (name, duration, resolution, fps, codec, audio). Both views share one model, proxy and
///   selection. Search filters the current bin and its sub-bins.
/// - Items drag out as mime::MediaItems (newline-separated ids). Double-click emits
///   openInSourceViewer(). Missing files are marked offline.
/// - Every change (import, rename, move, delete, bins) is one undo step.
///
/// Poster frames come from a PosterProvider; by default media::MediaCache::posterFrame(),
/// repainting items on MediaCache::posterFrameReady().
///
/// Thread-safety: GUI thread only.
class MediaPoolPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// Item presentation.
    enum class ViewMode {
        Grid,
        List
    };

    /// Creates the panel for ctx.project's media pool.
    MediaPoolPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Ids of the selected media items.
    [[nodiscard]] QStringList selectedMediaIds() const;
    /// The current (focused) media item id, or empty.
    [[nodiscard]] QString currentMediaId() const;
    /// Ids of the items currently shown (after bin + search filtering), in view order.
    [[nodiscard]] QStringList visibleMediaIds() const;

    /// Selects (and scrolls to) media item @p id, switching to its bin and clearing the search.
    /// Returns false if the item does not exist.
    bool selectMedia(const QString& id);
    /// Shows or hides the bins tree (remembered in QSettings). Until the user chooses, the tree
    /// is shown only when the panel is wide enough.
    void setBinsVisible(bool visible);

    /// The bin shown ("" = Master).
    [[nodiscard]] QString currentBin() const;
    /// Shows bin @p binId ("" = Master).
    void setCurrentBin(const QString& binId);
    /// Filters the items by @p text (name or path, case-insensitive).
    void setSearchText(const QString& text);

    /// Grid or list.
    [[nodiscard]] ViewMode viewMode() const { return m_viewMode; }
    /// Switches between grid and list (remembered in QSettings).
    void setViewMode(ViewMode mode);

    /// Replaces the poster frame provider (tests, or another cache). Call
    /// model()->thumbnailReady(path) when a pending poster becomes available.
    void setThumbnailProvider(PosterProvider provider);
    /// The item model (all items).
    [[nodiscard]] MediaPoolModel* model() const { return m_model; }

    // ---- Undoable operations (also used by the context menus) ----

    /// Imports @p paths into bin @p binId with user feedback. Returns the new ids.
    QStringList importFiles(const QStringList& paths, const QString& binId);
    /// Creates a bin named @p name inside @p parentId. Returns its id.
    QString createBin(const QString& name, const QString& parentId);
    /// Renames bin @p binId.
    bool renameBin(const QString& binId, const QString& name);
    /// Deletes bin @p binId (its items and sub-bins move to its parent).
    bool deleteBin(const QString& binId);
    /// Moves media items into bin @p binId. Returns the number moved.
    int moveItemsToBin(const QStringList& ids, const QString& binId);
    /// Removes media items (clips using them go offline). Returns the number removed.
    int deleteItems(const QStringList& ids);

signals:
    /// The current item changed (e.g. to update the metadata panel). Empty = none.
    void currentMediaChanged(const QString& mediaId);
    /// The user double-clicked an item: show it in the source viewer.
    void openInSourceViewer(const QString& mediaId);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildToolbar(QWidget* host);
    void applyAutoBins();
    void rebuildBins();
    void showItemMenu(const QPoint& globalPos);
    void showBinMenu(QTreeWidgetItem* item, const QPoint& globalPos);
    void importWithDialog();
    void renameCurrentItem();
    void confirmAndDelete(const QStringList& ids);
    void revealInFileManager(const QString& id);
    [[nodiscard]] QAbstractItemView* currentView() const;

    AppContext& m_ctx;
    MediaPoolModel* m_model = nullptr;
    MediaPoolFilterModel* m_proxy = nullptr;
    QItemSelectionModel* m_selection = nullptr;
    QSplitter* m_splitter = nullptr;
    QTreeWidget* m_bins = nullptr;
    QStackedWidget* m_stack = nullptr;
    QListView* m_grid = nullptr;
    QTreeView* m_list = nullptr;
    QWidget* m_empty = nullptr;
    QLineEdit* m_search = nullptr;
    QToolButton* m_gridButton = nullptr;
    QToolButton* m_listButton = nullptr;
    QToolButton* m_binsButton = nullptr;
    ViewMode m_viewMode = ViewMode::Grid;
    bool m_rebuildingBins = false;
    bool m_binsAuto = true; ///< bins visibility follows the width until the user toggles it
};

} // namespace kk
