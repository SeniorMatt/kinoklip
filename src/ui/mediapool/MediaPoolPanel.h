// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPanel — the project's media: bin list (Master, smart bins, power bins) + items in a
// poster grid or a detail list, search, colour filter, import (button, OS drops),
// rename/delete/reveal, clip colours / flags, usage commands, drag source for the timeline.
#pragma once

#include "core/Multicam.h"
#include "ui/common/PanelFrame.h"
#include "ui/mediapool/MediaPoolModel.h"

#include <QStringList>

#include <functional>

class QAbstractItemView;
class QItemSelectionModel;
class QLineEdit;
class QListView;
class QMenu;
class QSplitter;
class QStackedWidget;
class QToolButton;
class QTreeView;
class QTreeWidgetItem;

namespace kk {

struct AppContext;
class BinList;

/// The media pool panel (Edit page left column, Media page bottom).
///
/// - Left: the bin list (BinList): "Master" with the project's nested bins, "Smart Bins"
///   (rule-based, live; the default ones when enabled in Preferences > Editing) and
///   "Power Bins" (shared by every project, AppContext::powerBins). Create / rename / delete
///   bins from the context menu or the title bar; drag bins to reorder or re-parent them;
///   drop media items on a bin to move them; drop files from the OS on a bin to import them.
///   Show Bin List (button, `view.show_bin_list`) toggles it.
/// - Right: items of the current bin, as a grid of poster frames or a list with selectable,
///   sortable columns (right-click the header). View mode and sort order are remembered per
///   bin (Project::binViewState()). Search filters the current bin and its sub-bins by name,
///   path and metadata; the colour filter shows one clip colour.
/// - Items drag out as mime::MediaItems (newline-separated ids). Double-click emits
///   openInSourceViewer(). Missing files are marked offline; tiles show the clip colour,
///   flags and whether the item is used in a timeline.
/// - Every change of the project's pool (import, rename, move, delete, bins, smart bins,
///   colours, flags, metadata) is one undo step. Power bin changes are saved at once and are
///   not undoable.
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
        List,
        Strip ///< film strips of thumbnails across each clip (Cut page; setStripViewAvailable())
    };

    /// Creates the panel for ctx.project's media pool (and ctx.powerBins when set).
    MediaPoolPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Ids of the selected media items.
    [[nodiscard]] QStringList selectedMediaIds() const;
    /// Selects exactly the items @p ids that are visible in the current bin.
    void setSelectedMediaIds(const QStringList& ids);
    /// Stores the selected ids in the media pool commands (`mediapool.*` actions' data) and
    /// enables them for what the selection allows.
    void publishSelection();
    /// The current (focused) media item id, or empty.
    [[nodiscard]] QString currentMediaId() const;
    /// Ids of the items currently shown (after bin + search filtering), in view order.
    [[nodiscard]] QStringList visibleMediaIds() const;

    /// Selects (and scrolls to) media item @p id, switching to its bin and clearing the search
    /// and the colour filter. Returns false if the item does not exist.
    bool selectMedia(const QString& id);
    /// Shows or hides the bin list (remembered in QSettings). Until the user chooses, the list
    /// is shown only when the panel is wide enough.
    void setBinsVisible(bool visible);
    /// True while the bin list is shown.
    [[nodiscard]] bool binsVisible() const;

    /// The project bin shown ("" = Master, also while a smart or power bin is shown): where
    /// new timelines and imports of the timeline go.
    [[nodiscard]] QString currentBin() const;
    /// Shows project bin @p binId ("" = Master).
    void setCurrentBin(const QString& binId);
    /// Key of the bin shown (MediaPoolFilterModel keys: "", bin id, "smart:<id>",
    /// "power:", "power:<id>").
    [[nodiscard]] QString currentBinKey() const;
    /// Shows the bin with key @p key (unknown keys show Master).
    void setCurrentBinKey(const QString& key);
    /// Filters the items by @p text (name, path, metadata, case-insensitive).
    void setSearchText(const QString& text);
    /// Shows only items of clip colour @p colorId ("" = all, "none" = without colour).
    void setColorFilter(const QString& colorId);

    /// Grid, list or strip.
    [[nodiscard]] ViewMode viewMode() const { return m_viewMode; }
    /// Switches between grid, list and strip (remembered for the current bin and as the
    /// default). Strip falls back to grid where the strip view is not available.
    void setViewMode(ViewMode mode);
    /// Offers the strip view (button in the toolbar; off by default: only the Cut page's media
    /// pool has it, like Resolve).
    void setStripViewAvailable(bool available);
    [[nodiscard]] bool isStripViewAvailable() const { return m_stripAvailable; }
    /// Sorts the items by list column @p column (-1 = import order); remembered per bin.
    void setSort(int column, Qt::SortOrder order);
    /// Shows or hides list column @p column (remembered in QSettings). The name column
    /// cannot be hidden.
    void setColumnVisible(int column, bool visible);
    /// True if list column @p column is shown.
    [[nodiscard]] bool isColumnVisible(int column) const;
    /// Re-reads the preferences (default smart bins on / off).
    void applyPreferences();

    /// Replaces the poster frame provider (tests, or another cache). Call
    /// model()->thumbnailReady(path) when a pending poster becomes available.
    void setThumbnailProvider(PosterProvider provider);
    /// The item model (all items).
    [[nodiscard]] MediaPoolModel* model() const { return m_model; }
    /// The bin list (tests).
    [[nodiscard]] BinList* binList() const { return m_bins; }

    // ---- Undoable operations (also used by the context menus; MediaPoolOrganize.cpp) ----

    /// Imports @p paths into bin @p binKey with user feedback (a power bin key imports into
    /// the power bins). Returns the new ids.
    QStringList importFiles(const QStringList& paths, const QString& binKey);
    /// Creates a bin named @p name inside @p parentKey (project or power bin). Returns its key.
    QString createBin(const QString& name, const QString& parentKey);
    /// Renames bin @p binKey.
    bool renameBin(const QString& binKey, const QString& name);
    /// Deletes bin @p binKey with its sub-bins and their media (Resolve), asking first when it
    /// is not empty if @p confirm. Timelines inside move to Master.
    bool deleteBin(const QString& binKey, bool confirm = false);
    /// Moves bin @p binKey into / before / after bin @p targetKey (same section).
    bool moveBin(const QString& binKey, const QString& targetKey, int place);
    /// Moves media items into bin @p binKey. Items move between project and power bins as
    /// copies (power bins keep their items, the project keeps its own). Returns the count.
    int moveItemsToBin(const QStringList& ids, const QString& binKey);
    /// Removes media items (clips using them go offline). Returns the number removed.
    int deleteItems(const QStringList& ids);

    /// Adds smart bin @p bin to the project. Returns its key ("smart:<id>").
    QString createSmartBin(const SmartBin& bin);
    /// Replaces a user smart bin (same id).
    bool updateSmartBin(const SmartBin& bin);
    /// Deletes user smart bin @p key.
    bool deleteSmartBin(const QString& key);
    /// New Smart Bin… / Edit Smart Bin…: the rules dialog. Returns the key, empty if cancelled.
    QString newSmartBinWithDialog();
    void editSmartBinWithDialog(const QString& key);

    /// Sets the clip colour of @p ids (palette id, empty = clear).
    void setClipColor(const QStringList& ids, const QString& colorId);
    /// Sets (@p on) or clears flag @p flagIndex on @p ids.
    void setFlag(const QStringList& ids, int flagIndex, bool on);
    /// Clears every flag of @p ids.
    void clearFlags(const QStringList& ids);

    /// Project ids for media items @p ids: project ids unchanged, power bin items copied into
    /// project bin @p binId (or found there by path). One undo step when anything is added.
    QStringList ensureInProject(const QStringList& ids, const QString& binId = {});
    /// Select Clips in Timeline: selects the clips of the active timeline that use @p ids.
    /// Returns the number of clips selected.
    int selectClipsInTimeline(const QStringList& ids);
    /// Media files of the project used in no timeline (Usage = 0).
    [[nodiscard]] QStringList unusedClips() const;
    /// Remove Unused Clips: deletes unusedClips() (one undo step), asking first if @p confirm.
    /// Returns the number removed.
    int removeUnusedClips(bool confirm = true);

signals:
    /// The current item changed (e.g. to update the metadata panel). Empty = none.
    void currentMediaChanged(const QString& mediaId);
    /// The selection changed: ids of the selected items (metadata batch editing).
    void selectedMediaChanged(const QStringList& ids);
    /// The user double-clicked an item: show it in the source viewer.
    void openInSourceViewer(const QString& mediaId);
    /// The items shown changed: another bin, search, filter, sort order, or items added /
    /// removed (the Cut page rebuilds its source tape from visibleMediaIds()).
    void shownItemsChanged();
    /// A timeline / compound clip item was opened in a timeline tab (double-click, menu).
    void timelineOpened(quint64 uid);

public:
    /// Opens timeline @p uid in a tab (Project::openTab) and emits timelineOpened().
    void openTimeline(quint64 uid);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

public:
    /// Relinks offline media found by file name in @p folder (one undo step). Returns the
    /// number of items relinked.
    int relinkFromFolder(const QString& folder, const QStringList& onlyIds = {});
    /// "Relink Offline Media…" (all) / "Relink Selected Clips…" (@p onlyIds): asks for a
    /// folder, then relinkFromFolder() and a summary.
    void relinkWithDialog(const QStringList& onlyIds = {});

    // ---- Multicam and sync (MediaPoolMulticam.cpp) ----

    /// Source in point of a media item in µs (0 = none); Sync Based on In Points asks it
    /// (EditPage connects the source viewer's remembered marks).
    using InPointProvider = std::function<int64_t(const QString& mediaId)>;
    void setInPointProvider(InPointProvider provider) { m_inPoints = std::move(provider); }
    /// Media items among @p ids that can be synced / be angles (files with a duration).
    [[nodiscard]] QStringList syncableIds(const QStringList& ids) const;
    /// Creates a multicam clip from @p ids (sync by @p sync, progress / problem dialogs for
    /// waveform sync) in the current bin; angle names from the clip names or numbers. Returns
    /// the uid, 0 when cancelled or refused.
    quint64 createMulticamClip(const QStringList& ids, const QString& name,
        multicam::SyncMethod sync, bool namesFromClips, bool moveSources);
    /// Create New Multicam Clip Using Selected Clips: the options dialog, then
    /// createMulticamClip().
    void newMulticamClipWithDialog();
    /// Sync and Place on Timeline: @p ids synced by @p sync on new tracks of the active
    /// timeline from its playhead (one undo step). Returns the number of clips placed.
    int syncAndPlaceOnTimeline(const QStringList& ids, multicam::SyncMethod sync);

private:
    void buildToolbar(QWidget* host);
    void connectActions();
    void addMulticamMenu(QMenu& menu, const QStringList& ids);
    void connectMulticamActions();
    void applyAutoBins();
    void rebuildBins();
    void showBinKey(const QString& key);
    void applyBinView();
    void storeBinView();
    void applyColumnVisibility();
    [[nodiscard]] QVector<SmartBin> shownSmartBins() const;
    [[nodiscard]] MediaPool* poolForKey(const QString& key) const;
    // ---- Context menus (MediaPoolMenus.cpp) ----
    void showItemMenu(const QPoint& globalPos);
    void showBinMenu(QTreeWidgetItem* item, const QPoint& globalPos);
    void showHeaderMenu(const QPoint& globalPos);
    void addColorMenu(QMenu& menu, const QStringList& ids);
    void addFlagMenu(QMenu& menu, const QStringList& ids);
    void fillColorFilterMenu(QMenu* menu);
    void importWithDialog();
    /// "Import Media Folder…" of the context menus: into project bin @p binKey ("" = Master).
    void importFolderWithDialog(const QString& binKey);
    void renameCurrentItem();
    void confirmAndDelete(const QStringList& ids);
    void revealInFileManager(const QString& id);
    [[nodiscard]] QAbstractItemView* currentView() const;
    /// Runs @p change on the pool of @p ids: inside one undo step for project items, directly
    /// for power bin items.
    void editItems(const QString& label, const QStringList& ids,
        const std::function<void(MediaPool*, const QString&)>& change);

    AppContext& m_ctx;
    InPointProvider m_inPoints;
    MediaPoolModel* m_model = nullptr;
    MediaPoolFilterModel* m_proxy = nullptr;
    QItemSelectionModel* m_selection = nullptr;
    QSplitter* m_splitter = nullptr;
    BinList* m_bins = nullptr;
    QStackedWidget* m_stack = nullptr;
    QListView* m_grid = nullptr;
    QTreeView* m_list = nullptr;
    QListView* m_strip = nullptr; ///< strip view (MediaPoolStripDelegate)
    QWidget* m_empty = nullptr;
    QLineEdit* m_search = nullptr;
    QToolButton* m_gridButton = nullptr;
    QToolButton* m_listButton = nullptr;
    QToolButton* m_stripButton = nullptr;
    QToolButton* m_binsButton = nullptr;
    QToolButton* m_colorButton = nullptr;
    ViewMode m_viewMode = ViewMode::Grid;
    bool m_stripAvailable = false;
    /// Stack page showing the items in @p mode (the strip page only where available).
    [[nodiscard]] int stackIndexFor(ViewMode mode) const;
    bool m_binsAuto = true; ///< bins visibility follows the width until the user toggles it
    bool m_showDefaultSmartBins = false; ///< Preferences > Editing
    bool m_applyingView = false; ///< applyBinView() running: do not store the view back
};

} // namespace kk
