// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPanel implementation: construction, views, bin list, per-bin view state, drops.
// Context menus live in MediaPoolMenus.cpp, the undoable operations in MediaPoolOrganize.cpp.
#include "ui/mediapool/MediaPoolPanel.h"

#include "core/ClipColors.h"
#include "core/MediaPool.h"
#include "core/PowerBins.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SmartBins.h"
#include "export/ProxyQueue.h"
#include "media/MediaCache.h"
#include "platform/DroppedFiles.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragAndDrop.h"
#include "ui/common/DragMime.h"
#include "ui/common/Icons.h"
#include "ui/common/MediaManagement.h"
#include "ui/common/PanelSplitter.h"
#include "ui/common/SelectedRows.h"
#include "ui/mediapool/BinList.h"
#include "ui/mediapool/MediaPoolDelegate.h"
#include "ui/mediapool/MediaPoolStripDelegate.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
constexpr int kSearchMaxWidth = 280; ///< px; Resolve keeps the media pool search compact

constexpr char kViewModeKey[] = "mediaPool/viewMode";
constexpr char kBinsVisibleKey[] = "mediaPool/binsVisible";
constexpr char kColumnsKey[] = "mediaPool/columns"; ///< list of visible column keys
constexpr char kThumbnailWidthKey[] = "mediaPool/thumbnailWidth"; ///< poster width, px
constexpr int kDefaultThumbnailStep = 5;
static_assert(
    MediaPoolPanel::kThumbnailWidths[kDefaultThumbnailStep] == MediaPoolModel::kPosterSize.width());
/// A wheel notch scrolls the grid / strips by this fraction of a row (QListView's own step is
/// three whole rows of 115 px).
constexpr int kWheelRowFraction = 3;
constexpr int kWheelNotch = 120; ///< QWheelEvent::angleDelta() of one notch
constexpr int kMaxHistory = 100; ///< bins kept in the Back / Forward history
constexpr int kZoomSliderWidth = 70;
constexpr int kZoomSliderMinWidth = 30; ///< px; narrower than this the slider is hidden
constexpr int kSearchChrome = 56; ///< px: search icon, clear button and text margins
/// Below this panel width the bins sidebar hides itself (unless the user chose explicitly), so
/// the grid keeps at least two tile columns in the narrow Edit page column.
constexpr int kAutoBinsMinWidth = 460;
constexpr int kBinsWidth = 130;
constexpr int kItemsWidth = 400;
constexpr int kGridSpacing = 4;
constexpr int kStackGrid = 0;
constexpr int kStackList = 1;
constexpr int kStackEmpty = 2;
constexpr int kStackStrip = 3;

/// Stored names of the view modes (QSettings default and per-bin BinViewState::mode).
QString viewModeKey(MediaPoolPanel::ViewMode mode)
{
    switch (mode) {
    case MediaPoolPanel::ViewMode::List:
        return QStringLiteral("list");
    case MediaPoolPanel::ViewMode::Strip:
        return QStringLiteral("strip");
    case MediaPoolPanel::ViewMode::Grid:
        break;
    }
    return QStringLiteral("grid");
}

MediaPoolPanel::ViewMode viewModeFromKey(const QString& key)
{
    if (key == QLatin1String("list"))
        return MediaPoolPanel::ViewMode::List;
    if (key == QLatin1String("strip"))
        return MediaPoolPanel::ViewMode::Strip;
    return MediaPoolPanel::ViewMode::Grid;
}

/// Index of the thumbnail size nearest @p width.
int nearestThumbnailStep(int width)
{
    const auto& widths = MediaPoolPanel::kThumbnailWidths;
    const auto nearest = std::min_element(widths.begin(), widths.end(),
        [width](int a, int b) { return std::abs(a - width) < std::abs(b - width); });
    return int(nearest - widths.begin());
}

/// The thumbnail width stored by the last change of either page's media pool.
int storedThumbnailWidth(int fallback)
{
    return QSettings().value(QLatin1String(kThumbnailWidthKey), fallback).toInt();
}
} // namespace

MediaPoolPanel::MediaPoolPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Media Pool"), parent)
    , m_ctx(ctx)
{
    setPanelKey(QStringLiteral("media_pool")); // in-app help: panel:media_pool
    setAcceptDrops(true);
    Project* project = ctx.project;
    m_showDefaultSmartBins = prefs::showDefaultSmartBins();

    m_model = new MediaPoolModel(project, this);
    if (ctx.powerBins)
        m_model->setPowerBins(ctx.powerBins->pool());
    // A drag of power bin items reaching the timeline copies them into the project then.
    m_model->setPowerResolver(
        [this](const QStringList& powerIds) { return ensureInProject(powerIds); });
    m_proxy = new MediaPoolFilterModel(project, this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_selection = new QItemSelectionModel(m_proxy, this);
    if (!ctx.currentMediaPoolBin)
        becomeImportTarget(); // until a media pool is shown

    // Default poster source: the shared media cache (async; repaint on posterFrameReady).
    setThumbnailProvider([](const QString& path, int height) {
        return media::MediaCache::instance().posterFrame(path, height);
    });
    connect(&media::MediaCache::instance(), &media::MediaCache::posterFrameReady, m_model,
        &MediaPoolModel::thumbnailReady);
    // Proxy / optimized media badges and columns.
    if (MediaManagement* mm = ctx.media) {
        m_model->setLinkStatusProvider([mm](const QString& id) { return mm->status(id); });
        connect(mm, &MediaManagement::statusChanged, m_model,
            [this](const QString& id) { m_model->linkStatusChanged(id); });
        connect(
            mm, &MediaManagement::statusReset, m_model, [this] { m_model->linkStatusChanged(); });
    }

    // ---- Title bar ----
    if (ctx.actions) {
        auto* import = new QAction(icon(QStringLiteral("import")), tr("Import Media…"), this);
        import->setToolTip(tr("Import media into the current bin"));
        connect(import, &QAction::triggered, this, &MediaPoolPanel::importWithDialog);
        addTitleBarAction(import);
    }
    auto* newBin = new QAction(icon(QStringLiteral("folder")), tr("New Bin"), this);
    connect(
        newBin, &QAction::triggered, this, [this] { createBin(tr("New Bin"), currentBinKey()); });
    addTitleBarAction(newBin);

    // ---- Body ----
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* toolbar = new QWidget;
    buildToolbar(toolbar);
    layout->addWidget(toolbar);

    m_bins = new BinList;
    m_bins->setObjectName(QStringLiteral("MediaPoolBins"));
    m_bins->installEventFilter(this); // Delete / Backspace delete the current bin
    m_bins->onItemsDropped = [this](const QStringList& ids, const QStringList& powerIds,
                                 const QString& key) { moveItemsToBin(ids + powerIds, key); };
    m_bins->onFilesDropped
        = [this](const QStringList& paths, const QString& key) { importFiles(paths, key); };
    m_bins->onBinDropped = [this](const QString& key, const QString& target,
                               BinList::DropPlace place) { moveBin(key, target, int(place)); };

    m_grid = new QListView;
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setMovement(QListView::Static);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setUniformItemSizes(true); // O(1) layout per item, required for 2,000+ items
    m_grid->setLayoutMode(QListView::Batched);
    m_grid->setBatchSize(200);
    m_grid->setSpacing(kGridSpacing);
    m_grid->setWrapping(true);
    m_grid->setItemDelegate(new MediaPoolDelegate(m_grid)); // tile size: applyThumbnailStep()

    // Strip view (Cut page): one row per clip with a film strip of thumbnails across it.
    m_strip = new QListView;
    m_strip->setObjectName(QStringLiteral("MediaPoolStrip"));
    m_strip->setViewMode(QListView::ListMode);
    m_strip->setUniformItemSizes(true);
    m_strip->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); // scrollByWheel()
    m_strip->setResizeMode(QListView::Adjust);
    m_strip->setItemDelegate(new MediaPoolStripDelegate(
        [this](const QString& id) {
            MediaPoolStripDelegate::StripSource source;
            if (const MediaItem* item = m_model->item(id); item && !item->isTimeline()) {
                source.path = item->path;
                source.durationUs = item->info.isStillImage ? 0 : item->info.durationUs;
                source.hasVideo = item->info.hasVideo;
            }
            return source;
        },
        m_strip));
    connect(&media::MediaCache::instance(), &media::MediaCache::thumbnailReady, m_strip, [this] {
        if (m_strip->isVisible())
            m_strip->viewport()->update();
    });

    m_list = new QTreeView;
    m_list->setObjectName(QStringLiteral("MediaPoolList"));
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(-1, Qt::AscendingOrder); // import order until the user sorts
    m_list->setAllColumnsShowFocus(true);

    for (QAbstractItemView* view : {static_cast<QAbstractItemView*>(m_grid),
             static_cast<QAbstractItemView*>(m_list), static_cast<QAbstractItemView*>(m_strip)}) {
        view->setModel(m_proxy);
        view->setSelectionModel(m_selection);
        view->setFrameShape(QFrame::NoFrame);
        view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        view->setDragEnabled(true);
        view->setDragDropMode(QAbstractItemView::DragOnly);
        view->setDefaultDropAction(Qt::CopyAction); // see MediaPoolModel::supportedDragActions
        view->setAcceptDrops(false); // OS file drops are handled by the panel
        view->setEditTriggers(QAbstractItemView::EditKeyPressed);
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        view->setMouseTracking(true);
        view->installEventFilter(this); // Delete / Backspace / Ctrl+A act on the media pool
        connect(view, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
            if (index.data(MediaPoolModel::FolderRole).toBool()) {
                // A folder opens its bin: the bin list selects it, the items show its contents.
                setCurrentBinKey(index.data(MediaPoolModel::FolderBinRole).toString());
                return;
            }
            const QString id = index.data(MediaPoolModel::IdRole).toString();
            if (id.isEmpty())
                return;
            // Timelines and compound clips open in a timeline tab (Resolve), media in the
            // source viewer (power bin media first becomes part of the project; multicam clips show
            // their angles there).
            const MediaItem* item = m_model->item(id);
            if (item && item->isTimeline() && item->kind != MediaItemKind::MulticamClip) {
                openTimeline(item->timelineUid);
                return;
            }
            if (item && item->isSubtitle())
                return; // no picture to view: drag it onto the timeline for captions
            const QStringList projectIds = ensureInProject({id});
            if (!projectIds.isEmpty())
                emit openInSourceViewer(projectIds.front());
        });
        connect(view, &QWidget::customContextMenuRequested, this, [this, view](const QPoint& pos) {
            const QModelIndex index = view->indexAt(pos);
            if (index.data(MediaPoolModel::FolderRole).toBool())
                showFolderMenu(view, index, view->viewport()->mapToGlobal(pos));
            else
                showItemMenu(view->viewport()->mapToGlobal(pos));
        });
    }
    // A double-click on a folder tile's name renames the bin (elsewhere it opens it); the
    // wheel scrolls gently and Ctrl+wheel zooms; the Back / Forward mouse buttons go through
    // the bin history (also over the bin list).
    for (QWidget* viewport :
        {m_grid->viewport(), m_list->viewport(), m_strip->viewport(), m_bins->viewport()})
        viewport->installEventFilter(this);
    QHeaderView* header = m_list->header();
    header->setStretchLastSection(true);
    header->setSectionsMovable(true);
    header->resizeSection(MediaPoolModel::NameColumn, 200);
    header->resizeSection(MediaPoolModel::DurationColumn, 90);
    header->resizeSection(MediaPoolModel::ResolutionColumn, 95);
    header->resizeSection(MediaPoolModel::FpsColumn, 55);
    header->resizeSection(MediaPoolModel::CodecColumn, 70);
    header->resizeSection(MediaPoolModel::ClipColorColumn, 90);
    header->resizeSection(MediaPoolModel::FlagsColumn, 90);
    header->resizeSection(MediaPoolModel::UsageColumn, 55);
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QWidget::customContextMenuRequested, this,
        [this, header](const QPoint& pos) { showHeaderMenu(header->mapToGlobal(pos)); });
    connect(header, &QHeaderView::sortIndicatorChanged, this, [this] { storeBinView(); });
    applyColumnVisibility();

    // The import shortcut comes from the keymap (remappable), never a hard-coded key.
    QString importKey;
    if (ctx.actions && ctx.actions->contains(QStringLiteral("file.import_media"))) {
        const QList<QKeySequence> keys
            = ctx.actions->shortcuts(QStringLiteral("file.import_media"));
        if (!keys.isEmpty())
            importKey = keys.first().toString(QKeySequence::NativeText);
    }
    m_empty = createPlaceholderContent(importKey.isEmpty()
            ? tr("No media yet.\nImport files or drop them here.")
            : tr("No media yet.\nImport files with %1 or drop them here.").arg(importKey));
    m_stack = new QStackedWidget;
    m_stack->insertWidget(kStackGrid, m_grid);
    m_stack->insertWidget(kStackList, m_list);
    m_stack->insertWidget(kStackEmpty, m_empty);
    m_stack->insertWidget(kStackStrip, m_strip);

    m_splitter = new PanelSplitter(Qt::Horizontal);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(1);
    m_splitter->addWidget(m_bins);
    m_splitter->addWidget(m_stack);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({kBinsWidth, kItemsWidth});
    layout->addWidget(m_splitter, 1);
    setContentWidget(content);

    // ---- Wiring ----
    connect(m_selection, &QItemSelectionModel::currentChanged, this,
        [this] { emit currentMediaChanged(currentMediaId()); });
    // The media pool commands (proxy / optimized media) act on the selection: keep it in
    // their data so menus, shortcuts and the context menu all see it.
    connect(m_selection, &QItemSelectionModel::selectionChanged, this, [this] {
        publishSelection();
        emit selectedMediaChanged(selectedMediaIds());
    });
    connect(m_bins, &BinList::keyActivated, this, &MediaPoolPanel::showBinKey);
    connect(m_bins, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item) {
        // In-place rename of a bin or a user smart bin (not the rows rebuild() creates).
        if (m_bins->isRebuilding())
            return;
        const QString key = item->data(0, BinList::kKeyRole).toString();
        const BinList::RowKind kind = BinList::kindOf(item);
        if (kind == BinList::RowKind::ProjectBin || kind == BinList::RowKind::PowerBin)
            renameBin(key, item->text(0));
        else if (kind == BinList::RowKind::SmartBin) {
            const SmartBin* smart = m_proxy->smartBinFor(key);
            const QString name = item->text(0).trimmed();
            if (!smart || name == smart->name) {
                // unchanged (icon / data updates also emit itemChanged): nothing to do
            } else if (!name.isEmpty() && m_ctx.project->mediaPool()->smartBin(smart->id)) {
                SmartBin renamed = *smart;
                renamed.name = name;
                updateSmartBin(renamed);
            } else {
                QTimer::singleShot(0, this, &MediaPoolPanel::rebuildBins); // restore the name
            }
        }
    });
    connect(m_bins, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        showBinMenu(m_bins->itemAt(pos), m_bins->viewport()->mapToGlobal(pos));
    });
    MediaPool* pool = project->mediaPool();
    connect(pool, &MediaPool::binsChanged, this, &MediaPoolPanel::rebuildBins);
    connect(pool, &MediaPool::smartBinsChanged, this, &MediaPoolPanel::rebuildBins);
    connect(pool, &MediaPool::reset, this, &MediaPoolPanel::rebuildBins);
    if (ctx.powerBins) {
        connect(ctx.powerBins->pool(), &MediaPool::binsChanged, this, &MediaPoolPanel::rebuildBins);
        connect(ctx.powerBins->pool(), &MediaPool::reset, this, &MediaPoolPanel::rebuildBins);
    }
    connect(project, &Project::projectReset, this, &MediaPoolPanel::applyBinView);
    auto updateEmpty = [this] {
        const bool empty = m_model->rowCount() == 0;
        m_stack->setCurrentIndex(empty ? kStackEmpty : stackIndexFor(m_viewMode));
    };
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, &MediaPoolPanel::shownItemsChanged);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, &MediaPoolPanel::shownItemsChanged);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, &MediaPoolPanel::shownItemsChanged);
    connect(m_proxy, &QAbstractItemModel::layoutChanged, this, &MediaPoolPanel::shownItemsChanged);
    connect(m_model, &QAbstractItemModel::rowsInserted, this, updateEmpty);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, updateEmpty);
    connect(m_model, &QAbstractItemModel::modelReset, this, updateEmpty);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) {
        if (s == Qt::ApplicationActive) {
            m_model->recheckOffline();
            m_ctx.project->mediaPool()->recheckOffline(); // the timeline and viewers' markers
        }
    });
    connectActions();

    rebuildBins();
    connectMulticamActions();
    const QVariant binsChoice = QSettings().value(QLatin1String(kBinsVisibleKey));
    if (binsChoice.isValid())
        setBinsVisible(binsChoice.toBool());
    else
        applyAutoBins();
    const QString stored = QSettings().value(QLatin1String(kViewModeKey)).toString();
    setViewMode(viewModeFromKey(stored));
    applyBinView();
    applyThumbnailStep(nearestThumbnailStep(storedThumbnailWidth(thumbnailWidth())), false);
    m_history = {currentBinKey()};
}

void MediaPoolPanel::buildToolbar(QWidget* host)
{
    host->setObjectName(QStringLiteral("MediaPoolToolbar"));
    m_toolbar = host;
    host->installEventFilter(this);
    auto* layout = new QHBoxLayout(host);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(4);
    m_binsButton = new QToolButton(host);
    m_binsButton->setObjectName(QStringLiteral("MediaPoolShowBins"));
    m_binsButton->setIcon(icon(QStringLiteral("folder")));
    m_binsButton->setToolTip(tr("Show Bin List"));
    m_binsButton->setCheckable(true);
    m_binsButton->setAutoRaise(true);
    connect(m_binsButton, &QToolButton::clicked, this, &MediaPoolPanel::setBinsVisible);
    layout->addWidget(m_binsButton);
    layout->addStretch(1);
    m_search = new QLineEdit(host);
    m_search->setPlaceholderText(tr("Search media"));
    m_search->setToolTip(tr("Searches names, file paths, clip colors, flags, keywords, "
                            "comments and the other metadata"));
    m_search->setClearButtonEnabled(true);
    m_search->addAction(icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    connect(m_search, &QLineEdit::textChanged, this,
        [this](const QString& text) { m_proxy->setSearchText(text); });
    m_search->setMaximumWidth(kSearchMaxWidth);
    setSearchField(m_search); // spec 49.5
    layout->addWidget(m_search, 100); // takes the room before the spacer does

    // Clip colour filter: shows one colour (Resolve's media pool colour filter).
    m_colorButton = new QToolButton(host);
    m_colorButton->setObjectName(QStringLiteral("MediaPoolColorFilter"));
    m_colorButton->setIcon(icon(QStringLiteral("flag")));
    m_colorButton->setToolTip(tr("Show clips of one clip color"));
    m_colorButton->setAutoRaise(true);
    m_colorButton->setPopupMode(QToolButton::InstantPopup);
    auto* colorMenu = new QMenu(m_colorButton);
    connect(colorMenu, &QMenu::aboutToShow, this,
        [this, colorMenu] { fillColorFilterMenu(colorMenu); });
    m_colorButton->setMenu(colorMenu);
    layout->addWidget(m_colorButton);

    // Thumbnail size (Resolve's media pool slider; Ctrl+wheel over the grid too). Shown with
    // the grid only.
    m_zoomSlider = new QSlider(Qt::Horizontal, host);
    m_zoomSlider->setObjectName(QStringLiteral("MediaPoolThumbnailSize"));
    m_zoomSlider->setToolTip(tr("Thumbnail size (Ctrl+wheel over the clips)"));
    m_zoomSlider->setRange(0, int(kThumbnailWidths.size()) - 1);
    m_zoomSlider->setPageStep(1);
    m_zoomSlider->setValue(m_thumbnailStep);
    m_zoomSlider->setFixedWidth(kZoomSliderWidth);
    m_zoomSlider->setFocusPolicy(Qt::NoFocus);
    connect(m_zoomSlider, &QSlider::valueChanged, this,
        [this](int step) { applyThumbnailStep(step, true); });
    layout->addWidget(m_zoomSlider);

    auto makeToggle = [host, layout](const QString& iconName, const QString& tip) {
        auto* button = new QToolButton(host);
        button->setIcon(icon(iconName));
        button->setToolTip(tip);
        button->setCheckable(true);
        button->setAutoRaise(true);
        layout->addWidget(button);
        return button;
    };
    m_gridButton = makeToggle(QStringLiteral("grid_view"), tr("Thumbnail view"));
    m_listButton = makeToggle(QStringLiteral("list_view"), tr("List view"));
    m_stripButton = makeToggle(QStringLiteral("strip_view"), tr("Strip view"));
    m_stripButton->setObjectName(QStringLiteral("MediaPoolStripButton"));
    m_stripButton->setVisible(false); // setStripViewAvailable()
    connect(m_gridButton, &QToolButton::clicked, this, [this] { setViewMode(ViewMode::Grid); });
    connect(m_listButton, &QToolButton::clicked, this, [this] { setViewMode(ViewMode::List); });
    connect(m_stripButton, &QToolButton::clicked, this, [this] { setViewMode(ViewMode::Strip); });

    // Sort menu (Resolve's media pool sorts the thumbnail view too, not only the list).
    auto* sortButton = new QToolButton(host);
    sortButton->setObjectName(QStringLiteral("MediaPoolSort"));
    sortButton->setIcon(QIcon::fromTheme(QStringLiteral("view-sort")));
    sortButton->setToolTip(tr("Sort media"));
    sortButton->setAutoRaise(true);
    sortButton->setPopupMode(QToolButton::InstantPopup);
    auto* sortMenu = new QMenu(sortButton);
    auto* columns = new QActionGroup(sortMenu);
    const std::pair<int, QString> keys[] = {{-1, tr("Import Order")},
        {MediaPoolModel::NameColumn, tr("Clip Name")}, {MediaPoolModel::TypeColumn, tr("Type")},
        {MediaPoolModel::DurationColumn, tr("Duration")},
        {MediaPoolModel::ResolutionColumn, tr("Resolution")},
        {MediaPoolModel::FpsColumn, tr("Frame Rate")}, {MediaPoolModel::CodecColumn, tr("Codec")},
        {MediaPoolModel::ClipColorColumn, tr("Clip Color")},
        {MediaPoolModel::UsageColumn, tr("Usage")},
        {MediaPoolModel::DateAddedColumn, tr("Date Added")},
        {MediaPoolModel::DateModifiedColumn, tr("Date Modified")}};
    for (const auto& [column, label] : keys) {
        QAction* action = sortMenu->addAction(label);
        action->setCheckable(true);
        action->setData(column);
        columns->addAction(action);
    }
    sortMenu->addSeparator();
    QAction* descending = sortMenu->addAction(tr("Descending"));
    descending->setCheckable(true);
    auto applySort = [this, columns, descending] {
        const int column = columns->checkedAction() ? columns->checkedAction()->data().toInt() : -1;
        setSort(column, descending->isChecked() ? Qt::DescendingOrder : Qt::AscendingOrder);
    };
    connect(columns, &QActionGroup::triggered, this, applySort);
    connect(descending, &QAction::triggered, this, applySort);
    // The menu shows the current sort (it may come from the list header or the bin).
    connect(sortMenu, &QMenu::aboutToShow, this, [this, columns, descending] {
        const int column = m_list->header()->sortIndicatorSection();
        for (QAction* a : columns->actions())
            a->setChecked(a->data().toInt() == (column < 0 ? -1 : column));
        descending->setChecked(m_list->header()->sortIndicatorOrder() == Qt::DescendingOrder);
    });
    sortButton->setMenu(sortMenu);
    layout->addWidget(sortButton);
}

void MediaPoolPanel::connectActions()
{
    ActionRegistry* reg = m_ctx.actions;
    if (!reg)
        return;
    // Both pages own a media pool panel; commands act in the visible one only (the bin list
    // toggle is a shared setting, so both follow it).
    auto on = [this, reg](const char* id, auto&& slot) {
        if (!reg->contains(QLatin1String(id)))
            return;
        connect(reg->action(QLatin1String(id)), &QAction::triggered, this, [this, slot] {
            if (isVisible())
                slot();
        });
    };
    if (reg->contains(QStringLiteral("view.show_bin_list"))) {
        QAction* toggle = reg->action(QStringLiteral("view.show_bin_list"));
        connect(toggle, &QAction::triggered, this, [this](bool on) { setBinsVisible(on); });
    }
    on("mediapool.new_bin", [this] { createBin(tr("New Bin"), currentBinKey()); });
    on("mediapool.new_smart_bin", [this] { newSmartBinWithDialog(); });
    on("mediapool.remove_unused", [this] { removeUnusedClips(true); });
    on("mediapool.select_in_timeline", [this] { selectClipsInTimeline(selectedMediaIds()); });
}

// ---- State ----

void MediaPoolPanel::publishSelection()
{
    if (!m_ctx.actions)
        return;
    const QStringList ids = selectedMediaIds();
    const MediaLinks* links = m_ctx.project->mediaLinks();
    const ProxyQueue* queue = m_ctx.media ? m_ctx.media->proxyQueue() : nullptr;
    bool anyProxy = false;
    bool anyOptimized = false;
    bool anyPending = false;
    bool anyFile = false;
    for (const QString& id : ids) {
        const MediaItem* item = m_ctx.project->mediaPool()->item(id);
        if (!item || item->isTimeline())
            continue;
        anyFile = true;
        anyProxy |= !links->link(id, LinkKind::Proxy).isEmpty();
        anyOptimized |= !links->link(id, LinkKind::Optimized).isEmpty();
        if (queue)
            anyPending |= queue->isPending(id, LinkKind::Proxy)
                || queue->isPending(id, LinkKind::Optimized);
    }
    const std::pair<const char*, bool> enabled[] = {{"mediapool.generate_optimized", anyFile},
        {"mediapool.generate_proxy", anyFile}, {"mediapool.stop_generation", anyPending},
        {"mediapool.link_proxy", anyFile && ids.size() == 1}, {"mediapool.unlink_proxy", anyProxy},
        {"mediapool.delete_proxy", anyProxy}, {"mediapool.delete_optimized", anyOptimized}};
    if (m_ctx.actions->contains(QStringLiteral("clip.attributes"))) // Clip > Clip Attributes
        m_ctx.actions->action(QStringLiteral("clip.attributes"))->setData(ids);
    for (const auto& [id, on] : enabled) {
        if (!m_ctx.actions->contains(QLatin1String(id)))
            continue;
        QAction* a = m_ctx.actions->action(QLatin1String(id));
        a->setData(ids);
        a->setEnabled(on);
    }
}

QStringList MediaPoolPanel::selectedMediaIds() const
{
    QStringList ids;
    for (const QModelIndex& index : selectedRowIndexes(m_selection, MediaPoolModel::NameColumn)) {
        const QString id = index.data(MediaPoolModel::IdRole).toString();
        if (!id.isEmpty()) // folders have no media id
            ids.append(id);
    }
    return ids;
}

QStringList MediaPoolPanel::selectedBinIds() const
{
    QStringList ids;
    for (const QModelIndex& index : selectedRowIndexes(m_selection, MediaPoolModel::NameColumn)) {
        if (index.data(MediaPoolModel::FolderRole).toBool())
            ids.append(index.data(MediaPoolModel::FolderBinRole).toString());
    }
    return ids;
}

void MediaPoolPanel::setSelectedMediaIds(const QStringList& ids)
{
    QItemSelection selection;
    QModelIndex first;
    for (const QString& id : ids) {
        const QModelIndex index = m_proxy->mapFromSource(m_model->index(m_model->rowOf(id), 0));
        if (!index.isValid())
            continue;
        if (!first.isValid())
            first = index;
        selection.select(index, index);
    }
    m_selection->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    if (first.isValid())
        m_selection->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
}

QString MediaPoolPanel::currentMediaId() const
{
    return m_selection->currentIndex().data(MediaPoolModel::IdRole).toString();
}

QStringList MediaPoolPanel::visibleMediaIds() const
{
    QStringList ids;
    for (int r = 0; r < m_proxy->rowCount(); ++r) {
        const QString id = m_proxy->index(r, 0).data(MediaPoolModel::IdRole).toString();
        if (!id.isEmpty()) // folders have no media id
            ids.append(id);
    }
    return ids;
}

QString MediaPoolPanel::currentBin() const
{
    const QString key = m_proxy->bin();
    return MediaPoolFilterModel::isSmartKey(key) || MediaPoolFilterModel::isPowerKey(key)
        ? QString()
        : key;
}

void MediaPoolPanel::setCurrentBin(const QString& binId)
{
    setCurrentBinKey(binId);
}

void MediaPoolPanel::showImportedItems(const QStringList& ids, const QString& targetBin)
{
    const MediaPool* pool = m_ctx.project ? m_ctx.project->mediaPool() : nullptr;
    const MediaItem* first = pool && !ids.isEmpty() ? pool->item(ids.front()) : nullptr;
    if (!first || first->binId == targetBin)
        return;
    // Walk up from the item's bin to the child of the target bin.
    QString shown = first->binId;
    for (const Bin* b = pool->bin(shown); b && b->parentId != targetBin; b = pool->bin(shown))
        shown = b->parentId;
    if (pool->bin(shown))
        setCurrentBin(shown);
}

QString MediaPoolPanel::currentBinKey() const
{
    return m_proxy->bin();
}

QString MediaPoolPanel::importTargetKey() const
{
    if (prefs::importIntoMasterBin() || MediaPoolFilterModel::isSmartKey(currentBinKey()))
        return {};
    return currentBinKey();
}

void MediaPoolPanel::becomeImportTarget()
{
    // The shown media pool answers "which bin is current" for imports from elsewhere
    // (File > Import, the media browser, timeline drops: importTargetBin()).
    m_ctx.currentMediaPoolBin
        = [pool = QPointer<MediaPoolPanel>(this)] { return pool ? pool->currentBin() : QString(); };
}

void MediaPoolPanel::showEvent(QShowEvent* event)
{
    PanelFrame::showEvent(event);
    becomeImportTarget();
    // The other page's media pool may have changed the shared thumbnail size.
    applyThumbnailStep(nearestThumbnailStep(storedThumbnailWidth(thumbnailWidth())), false);
}

void MediaPoolPanel::setCurrentBinKey(const QString& key)
{
    const QString shown = m_bins->itemForKey(key) ? key : QString();
    m_bins->setCurrentKey(shown);
    showBinKey(shown);
}

void MediaPoolPanel::showBinKey(const QString& key)
{
    if (key == m_proxy->bin())
        return;
    m_proxy->setBin(key);
    applyBinView();
    if (m_recordHistory)
        recordHistory(key);
}

// ---- Bin history ----

void MediaPoolPanel::recordHistory(const QString& key)
{
    m_history.erase(
        m_history.begin() + std::min(m_historyIndex + 1, int(m_history.size())), m_history.end());
    if (m_history.isEmpty() || m_history.back() != key)
        m_history.append(key);
    if (m_history.size() > kMaxHistory)
        m_history.remove(0, m_history.size() - kMaxHistory);
    m_historyIndex = int(m_history.size()) - 1;
}

int MediaPoolPanel::historyTarget(int direction) const
{
    // Skips bins deleted since (no row in the bin list) and the bin shown (after a deleted
    // bin fell back to Master, Master may be both the entry before and the bin shown).
    for (int i = m_historyIndex + direction; i >= 0 && i < m_history.size(); i += direction) {
        const QString& key = m_history[i];
        if (key != currentBinKey() && m_bins->itemForKey(key))
            return i;
    }
    return -1;
}

bool MediaPoolPanel::canGoBack() const
{
    return historyTarget(-1) >= 0;
}

bool MediaPoolPanel::canGoForward() const
{
    return historyTarget(+1) >= 0;
}

bool MediaPoolPanel::goBack()
{
    return stepHistory(-1);
}

bool MediaPoolPanel::goForward()
{
    return stepHistory(+1);
}

bool MediaPoolPanel::stepHistory(int direction)
{
    const int target = historyTarget(direction);
    if (target < 0)
        return false;
    m_historyIndex = target;
    const QScopedValueRollback paused(m_recordHistory, false);
    setCurrentBinKey(m_history[target]);
    return true;
}

bool MediaPoolPanel::navigateByButton(const QMouseEvent* event)
{
    if (event->button() == Qt::BackButton)
        goBack();
    else if (event->button() == Qt::ForwardButton)
        goForward();
    else
        return false;
    return true;
}

void MediaPoolPanel::mousePressEvent(QMouseEvent* event)
{
    // The toolbar, the empty placeholder and the panel's margins pass presses up to here.
    if (navigateByButton(event)) {
        event->accept();
        return;
    }
    PanelFrame::mousePressEvent(event);
}

// ---- Thumbnail size and wheel ----

void MediaPoolPanel::setThumbnailWidth(int width)
{
    applyThumbnailStep(nearestThumbnailStep(width), true);
}

void MediaPoolPanel::zoomThumbnails(int steps)
{
    applyThumbnailStep(m_thumbnailStep + steps, true);
}

void MediaPoolPanel::applyThumbnailStep(int step, bool remember)
{
    step = std::clamp(step, 0, int(kThumbnailWidths.size()) - 1);
    const int width = kThumbnailWidths[step];
    const QSize poster(width, (width * 9 + 8) / 16); // 16:9, rounded
    auto* delegate = static_cast<MediaPoolDelegate*>(m_grid->itemDelegate());
    const bool changed = step != m_thumbnailStep || delegate->posterSize() != poster;
    m_thumbnailStep = step;
    if (remember)
        QSettings().setValue(QLatin1String(kThumbnailWidthKey), width);
    {
        const QSignalBlocker block(m_zoomSlider);
        m_zoomSlider->setValue(step);
    }
    if (!changed && m_grid->gridSize().isValid())
        return;
    delegate->setPosterSize(poster);
    m_model->setPosterSize(poster);
    m_grid->setGridSize(MediaPoolDelegate::tileSize(poster) + QSize(kGridSpacing, kGridSpacing));
    // Keep the current item in view across the relayout.
    if (const QModelIndex current = m_selection->currentIndex(); current.isValid())
        QTimer::singleShot(0, m_grid, [grid = m_grid, current = QPersistentModelIndex(current)] {
            if (current.isValid())
                grid->scrollTo(current);
        });
}

void MediaPoolPanel::zoomByWheel(const QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if ((delta > 0) != (m_zoomWheel > 0))
        m_zoomWheel = 0; // direction changed
    m_zoomWheel += delta;
    const int steps = m_zoomWheel / kWheelNotch;
    if (steps == 0)
        return;
    m_zoomWheel -= steps * kWheelNotch;
    zoomThumbnails(steps);
}

bool MediaPoolPanel::scrollByWheel(QAbstractItemView* view, const QWheelEvent* event)
{
    // Ctrl / Shift (page steps) and Alt (horizontal) keep Qt's handling.
    if (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier))
        return false;
    const QPoint pixels = event->pixelDelta();
    const QPoint angle = event->angleDelta();
    if (pixels.y() == 0 && angle.y() == 0)
        return false; // horizontal
    const int row
        = view == m_grid ? m_grid->gridSize().height() : MediaPoolStripDelegate::rowHeight();
    // Touchpads report pixels: follow the fingers. Wheels: a third of a row per notch.
    double delta = pixels.y() != 0 ? double(pixels.y())
                                   : angle.y() * (double(row) / kWheelRowFraction) / kWheelNotch;
    delta += m_wheelRemainder;
    const int scroll = int(delta);
    m_wheelRemainder = delta - scroll;
    QScrollBar* bar = view->verticalScrollBar();
    bar->setValue(bar->value() - scroll);
    return true;
}

void MediaPoolPanel::setSearchText(const QString& text)
{
    m_search->setText(text); // textChanged updates the proxy
}

void MediaPoolPanel::setColorFilter(const QString& colorId)
{
    m_proxy->setColorFilter(colorId);
    m_colorButton->setIcon(colorId.isEmpty() || colorId == QLatin1String("none")
            ? icon(QStringLiteral("flag"))
            : icon(QStringLiteral("flag"), clipColorValue(colorId)));
}

int MediaPoolPanel::stackIndexFor(ViewMode mode) const
{
    switch (mode) {
    case ViewMode::List:
        return kStackList;
    case ViewMode::Strip:
        return m_stripAvailable ? kStackStrip : kStackGrid;
    case ViewMode::Grid:
        break;
    }
    return kStackGrid;
}

void MediaPoolPanel::setStripViewAvailable(bool available)
{
    m_stripAvailable = available;
    m_stripButton->setVisible(available);
    setViewMode(m_viewMode); // a stored strip mode shows now (or falls back to the grid)
}

void MediaPoolPanel::setViewMode(ViewMode mode)
{
    m_viewMode = mode;
    const ViewMode shown = mode == ViewMode::Strip && !m_stripAvailable ? ViewMode::Grid : mode;
    m_gridButton->setChecked(shown == ViewMode::Grid);
    m_listButton->setChecked(shown == ViewMode::List);
    m_stripButton->setChecked(shown == ViewMode::Strip);
    m_zoomWanted = shown == ViewMode::Grid;
    fitZoomSlider();
    if (m_model->rowCount() > 0)
        m_stack->setCurrentIndex(stackIndexFor(mode));
    else
        m_stack->setCurrentIndex(kStackEmpty);
    if (m_applyingView)
        return;
    QSettings().setValue(QLatin1String(kViewModeKey), viewModeKey(mode));
    storeBinView();
}

void MediaPoolPanel::setSort(int column, Qt::SortOrder order)
{
    // Sorting through the list view keeps its header indicator in sync (sortIndicatorChanged
    // stores the choice for the bin).
    m_list->sortByColumn(column, order);
    if (column < 0)
        m_proxy->sort(-1);
    storeBinView();
}

void MediaPoolPanel::applyBinView()
{
    // Per-bin view mode and sort order (Resolve remembers them per bin).
    const BinViewState state = m_ctx.project->binViewState(m_proxy->bin());
    m_applyingView = true;
    if (!state.mode.isEmpty())
        setViewMode(viewModeFromKey(state.mode));
    const int column = MediaPoolModel::columnForKey(state.sortKey);
    m_list->sortByColumn(column, state.descending ? Qt::DescendingOrder : Qt::AscendingOrder);
    if (column < 0)
        m_proxy->sort(-1);
    m_applyingView = false;
}

void MediaPoolPanel::storeBinView()
{
    if (m_applyingView)
        return;
    BinViewState state;
    state.mode = viewModeKey(m_viewMode);
    const int column = m_list->header()->sortIndicatorSection();
    state.sortKey = column >= 0 && column < MediaPoolModel::ColumnCount
        ? MediaPoolModel::columnKey(column)
        : QString();
    state.descending
        = !state.sortKey.isEmpty() && m_list->header()->sortIndicatorOrder() == Qt::DescendingOrder;
    m_ctx.project->setBinViewState(m_proxy->bin(), state);
}

void MediaPoolPanel::setColumnVisible(int column, bool visible)
{
    if (column <= MediaPoolModel::NameColumn || column >= MediaPoolModel::ColumnCount)
        return; // the name always shows
    m_list->header()->setSectionHidden(column, !visible);
    QStringList keys;
    for (int c = 0; c < MediaPoolModel::ColumnCount; ++c) {
        if (!m_list->header()->isSectionHidden(c))
            keys.append(MediaPoolModel::columnKey(c));
    }
    QSettings().setValue(QLatin1String(kColumnsKey), keys);
}

bool MediaPoolPanel::isColumnVisible(int column) const
{
    return !m_list->header()->isSectionHidden(column);
}

void MediaPoolPanel::applyColumnVisibility()
{
    const QVariant stored = QSettings().value(QLatin1String(kColumnsKey));
    const QStringList keys = stored.toStringList();
    for (int c = 0; c < MediaPoolModel::ColumnCount; ++c) {
        const bool visible = c == MediaPoolModel::NameColumn
            || (stored.isValid() ? keys.contains(MediaPoolModel::columnKey(c))
                                 : MediaPoolModel::columnVisibleByDefault(c));
        m_list->header()->setSectionHidden(c, !visible);
    }
}

void MediaPoolPanel::applyPreferences()
{
    const bool show = prefs::showDefaultSmartBins();
    if (show == m_showDefaultSmartBins)
        return;
    m_showDefaultSmartBins = show;
    rebuildBins();
}

void MediaPoolPanel::setBinsVisible(bool visible)
{
    m_binsAuto = false;
    m_bins->setVisible(visible);
    m_binsButton->setChecked(visible);
    if (m_ctx.actions && m_ctx.actions->contains(QStringLiteral("view.show_bin_list")))
        m_ctx.actions->action(QStringLiteral("view.show_bin_list"))->setChecked(visible);
    QSettings().setValue(QLatin1String(kBinsVisibleKey), visible);
}

bool MediaPoolPanel::binsVisible() const
{
    return !m_bins->isHidden();
}

void MediaPoolPanel::applyAutoBins()
{
    if (!m_binsAuto)
        return;
    const bool visible = width() >= kAutoBinsMinWidth;
    m_bins->setVisible(visible);
    m_binsButton->setChecked(visible);
}

void MediaPoolPanel::fitZoomSlider()
{
    if (!m_toolbar || !m_zoomSlider)
        return;
    QLayout* box = m_toolbar->layout();
    const QMargins m = box->contentsMargins();
    int others = m.left() + m.right();
    int items = 0;
    for (int i = 0; i < box->count(); ++i) {
        QWidget* w = box->itemAt(i)->widget();
        if (w == m_zoomSlider || (w && w->isHidden()))
            continue;
        ++items;
        if (w && w != m_search)
            others += w->sizeHint().width();
    }
    const int spacing = box->spacing();
    const int searchNeed
        = m_search->fontMetrics().horizontalAdvance(m_search->placeholderText()) + kSearchChrome;
    // Items plus the slider, each separated by the spacing.
    const int room = m_toolbar->width() - others - items * spacing - searchNeed - spacing;
    const bool fits = room >= kZoomSliderMinWidth;
    if (fits)
        m_zoomSlider->setFixedWidth(std::min(kZoomSliderWidth, room));
    m_zoomSlider->setVisible(m_zoomWanted && fits);
}

void MediaPoolPanel::resizeEvent(QResizeEvent* event)
{
    PanelFrame::resizeEvent(event);
    fitZoomSlider();
    applyAutoBins();
}

bool MediaPoolPanel::selectMedia(const QString& id)
{
    const MediaItem* item = m_model->item(id);
    if (!item)
        return false;
    m_search->clear();
    setColorFilter({});
    const bool power = m_model->powerBins() && m_model->poolOf(id) == m_model->powerBins();
    setCurrentBinKey(
        power ? QLatin1String(MediaPoolFilterModel::kPowerPrefix) + item->binId : item->binId);
    const QModelIndex index = m_proxy->mapFromSource(m_model->index(m_model->rowOf(id), 0));
    if (!index.isValid())
        return false;
    m_selection->setCurrentIndex(
        index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    currentView()->scrollTo(index);
    return true;
}

void MediaPoolPanel::setThumbnailProvider(PosterProvider provider)
{
    m_model->setThumbnailProvider(std::move(provider));
}

bool MediaPoolPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_toolbar) {
        if (event->type() == QEvent::Resize)
            fitZoomSlider();
        return false;
    }
    // With the media pool focused, the editing keys act on it, as in Resolve: Delete /
    // Backspace delete the selected items (asking first when timeline clips use them) and
    // Ctrl+A selects every item of the bin. As window shortcuts they would ripple-delete or
    // select the timeline's clips instead (spec 33.31). A rename editor gets its own keys.
    const bool view = watched == m_grid || watched == m_list || watched == m_strip;
    const bool keyEvent
        = event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress;
    if (watched == m_bins && keyEvent) {
        // In the bin list they delete the current bin (Master and smart bins stay).
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace)
            && (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
            if (event->type() == QEvent::KeyPress)
                deleteBin(m_bins->currentKey(), true);
            event->accept();
            return true;
        }
    }
    const bool viewport = watched == m_grid->viewport() || watched == m_list->viewport()
        || watched == m_strip->viewport() || watched == m_bins->viewport();
    if (viewport
        && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonDblClick
            || event->type() == QEvent::MouseButtonRelease)) {
        // Back / Forward (quick repeated presses arrive as double-clicks); the views would
        // select the item under the pointer.
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::BackButton || mouse->button() == Qt::ForwardButton) {
            if (event->type() != QEvent::MouseButtonRelease)
                navigateByButton(mouse);
            return true;
        }
    }
    if (event->type() == QEvent::Wheel
        && (watched == m_grid->viewport() || watched == m_strip->viewport())) {
        const auto* wheel = static_cast<QWheelEvent*>(event);
        if (watched == m_grid->viewport() && (wheel->modifiers() & Qt::ControlModifier)) {
            zoomByWheel(wheel);
            return true;
        }
        if (scrollByWheel(
                watched == m_grid->viewport() ? static_cast<QAbstractItemView*>(m_grid) : m_strip,
                wheel))
            return true;
    }
    if (watched == m_grid->viewport() && event->type() == QEvent::MouseButtonDblClick) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const QModelIndex index = m_grid->indexAt(mouse->position().toPoint());
        if (index.data(MediaPoolModel::FolderRole).toBool()
            && MediaPoolDelegate::nameRect(m_grid->visualRect(index))
                   .contains(mouse->position().toPoint())) {
            m_selection->setCurrentIndex(
                index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            m_grid->edit(index);
            return true;
        }
    }
    if (view && keyEvent) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers mods = key->modifiers() & ~Qt::KeypadModifier;
        const bool del = (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace)
            && (mods == Qt::NoModifier || mods == Qt::ShiftModifier);
        const bool all = key->key() == Qt::Key_A && mods == Qt::ControlModifier;
        if (del || all) {
            if (event->type() == QEvent::KeyPress) {
                if (del) {
                    // Folders delete as bins (same question and undo as in the bin list).
                    const QStringList bins = selectedBinIds();
                    confirmAndDelete(selectedMediaIds());
                    for (const QString& bin : bins)
                        deleteBin(bin, true);
                } else
                    static_cast<QAbstractItemView*>(watched)->selectAll();
            }
            event->accept(); // ShortcutOverride: the key goes to the view, not to a shortcut
            return true;
        }
    }
    return PanelFrame::eventFilter(watched, event);
}

QAbstractItemView* MediaPoolPanel::currentView() const
{
    switch (stackIndexFor(m_viewMode)) {
    case kStackList:
        return m_list;
    case kStackStrip:
        return m_strip;
    default:
        return m_grid;
    }
}

// ---- Bins ----

QVector<SmartBin> MediaPoolPanel::shownSmartBins() const
{
    QVector<SmartBin> bins;
    if (m_showDefaultSmartBins)
        bins = smartbins::defaultSmartBins();
    bins += m_ctx.project->mediaPool()->smartBins();
    return bins;
}

MediaPool* MediaPoolPanel::poolForKey(const QString& key) const
{
    if (MediaPoolFilterModel::isPowerKey(key))
        return m_ctx.powerBins ? m_ctx.powerBins->pool() : nullptr;
    if (MediaPoolFilterModel::isSmartKey(key))
        return nullptr;
    return m_ctx.project->mediaPool();
}

void MediaPoolPanel::rebuildBins()
{
    // Falling back to Master for a deleted bin is not a navigation of the user's.
    const QScopedValueRollback paused(m_recordHistory, false);
    m_proxy->binsChanged();
    m_proxy->smartBinsChanged();
    // A default smart bin that was switched off: back to Master.
    if (MediaPoolFilterModel::isSmartKey(m_proxy->bin()) && !m_showDefaultSmartBins
        && MediaPoolFilterModel::idOfKey(m_proxy->bin())
               .startsWith(QLatin1String(smartbins::kDefaultIdPrefix)))
        m_proxy->setBin({});
    m_bins->setCurrentKey(m_proxy->bin());
    m_bins->rebuild(*m_ctx.project->mediaPool(), shownSmartBins(),
        m_ctx.powerBins ? m_ctx.powerBins->pool() : nullptr);
    if (!m_bins->setCurrentKey(m_proxy->bin()))
        showBinKey({});
}

void MediaPoolPanel::openTimeline(quint64 uid)
{
    if (m_ctx.project->openTab(uid))
        emit timelineOpened(uid);
}

// ---- OS drops onto the item area ----

namespace {
/// True for drags of media pool items or of a bin (they can be dropped on a folder).
bool isPoolDrag(const QMimeData* data)
{
    return data->hasFormat(QLatin1String(mime::MediaItems))
        || data->hasFormat(QLatin1String(BinList::kBinMime));
}
} // namespace

QString MediaPoolPanel::folderAt(const QPoint& pos) const
{
    const QAbstractItemView* view = currentView();
    if (!view || !view->isVisible())
        return {};
    const QPoint local = view->viewport()->mapFrom(this, pos);
    if (!view->viewport()->rect().contains(local))
        return {};
    const QModelIndex index = view->indexAt(local);
    return index.data(MediaPoolModel::FolderRole).toBool()
        ? index.data(MediaPoolModel::FolderBinRole).toString()
        : QString();
}

void MediaPoolPanel::dragEnterEvent(QDragEnterEvent* event)
{
    // Files from the OS anywhere; clips and bins from the pool itself only onto a folder
    // (dragMoveEvent() decides per position).
    if (platform::hasDroppedFiles(event->mimeData()) || isPoolDrag(event->mimeData()))
        event->acceptProposedAction();
    else
        event->ignore();
    logDropEvent("media pool", event);
}

void MediaPoolPanel::dragMoveEvent(QDragMoveEvent* event)
{
    // Explicit, so every move answers the compositor (Wayland: wl_data_offer.accept +
    // set_actions per motion); an unanswered offer is treated as "cannot drop here".
    const QMimeData* data = event->mimeData();
    bool ok = platform::hasDroppedFiles(data);
    if (!ok && isPoolDrag(data)) {
        const QString folder = folderAt(event->position().toPoint());
        ok = !folder.isEmpty()
            && QString::fromUtf8(data->data(QLatin1String(BinList::kBinMime))) != folder;
    }
    if (ok)
        event->acceptProposedAction();
    else
        event->ignore();
}

void MediaPoolPanel::dropEvent(QDropEvent* event)
{
    logDropEvent("media pool", event);
    const QMimeData* data = event->mimeData();
    const QString folder = folderAt(event->position().toPoint());
    if (!folder.isEmpty() && isPoolDrag(data)) {
        // Onto a folder: clips move into that bin (as onto the bin list), a bin moves inside.
        event->acceptProposedAction();
        if (data->hasFormat(QLatin1String(BinList::kBinMime))) {
            const QString bin = QString::fromUtf8(data->data(QLatin1String(BinList::kBinMime)));
            if (bin != folder)
                QTimer::singleShot(0, this,
                    [this, bin, folder] { moveBin(bin, folder, int(BinList::DropPlace::Into)); });
            return;
        }
        // Power bin items carry their own ids (reading mime::MediaItems would copy them into
        // the project first), as in BinList::dropEvent().
        QStringList ids = QString::fromUtf8(data->data(QLatin1String(kPowerItemsMime)))
                              .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (ids.isEmpty())
            ids = QString::fromUtf8(data->data(QLatin1String(mime::MediaItems)))
                      .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        // From the event loop: the source view is still inside its drag.
        QTimer::singleShot(0, this, [this, ids, folder] { moveItemsToBin(ids, folder); });
        return;
    }
    const QStringList paths = platform::droppedLocalFiles(data);
    if (paths.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    // From the event loop: importing may show a modal report, which must not run inside the
    // drop (the source would wait for the drop to finish). Onto a folder: into that bin;
    // elsewhere the import target (the shown bin or Master, Preferences > Editing).
    const QString key = !folder.isEmpty() ? folder : importTargetKey();
    QTimer::singleShot(0, this, [this, paths, key] { importFiles(paths, key); });
}

} // namespace kk
