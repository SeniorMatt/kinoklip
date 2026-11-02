// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPanel implementation.
#include "ui/mediapool/MediaPoolPanel.h"

#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "export/ProxyQueue.h"
#include "media/MediaCache.h"
#include "platform/ChildProcessEnv.h"
#include "platform/DroppedFiles.h"
#include "platform/FileDialogs.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragAndDrop.h"
#include "ui/common/DragMime.h"
#include "ui/common/Icons.h"
#include "ui/common/MediaImport.h"
#include "ui/common/MediaManagement.h"
#include "ui/common/PanelSplitter.h"
#include "ui/mediapool/MediaPoolDelegate.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {
constexpr int kSearchMaxWidth = 280; ///< px; Resolve keeps the media pool search compact

constexpr int kBinIdRole = Qt::UserRole;
constexpr char kViewModeKey[] = "mediaPool/viewMode";
constexpr char kBinsVisibleKey[] = "mediaPool/binsVisible";
/// Below this panel width the bins sidebar hides itself (unless the user chose explicitly), so
/// the grid keeps at least two tile columns in the narrow Edit page column.
constexpr int kAutoBinsMinWidth = 460;
constexpr int kBinsWidth = 130;
constexpr int kItemsWidth = 400;
constexpr int kGridSpacing = 4;
constexpr int kStackGrid = 0;
constexpr int kStackList = 1;
constexpr int kStackEmpty = 2;

QStringList idsFromMime(const QMimeData* data)
{
    const QString text = QString::fromUtf8(data->data(QLatin1String(mime::MediaItems)));
    return text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

/// Files from a file manager / the desktop (uri-list or document-portal transfer).
QStringList localFiles(const QMimeData* data)
{
    return platform::droppedLocalFiles(data);
}

/// Bins tree accepting media item drops (move into bin) and OS file drops (import into bin).
class BinTree final : public QTreeWidget
{
public:
    std::function<void(const QStringList& ids, const QString& binId)> onItemsDropped;
    std::function<void(const QStringList& paths, const QString& binId)> onFilesDropped;

    using QTreeWidget::QTreeWidget;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event->mimeData()->hasFormat(QLatin1String(mime::MediaItems))
            || platform::hasDroppedFiles(event->mimeData()))
            event->acceptProposedAction();
        else
            event->ignore();
        logDropEvent("bins", event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        QTreeWidgetItem* target = itemAt(event->position().toPoint());
        if (target) {
            setCurrentItem(target);
            event->acceptProposedAction();
        } else {
            event->ignore();
        }
        logDropEvent("bins", event);
    }

    void dropEvent(QDropEvent* event) override
    {
        QTreeWidgetItem* target = itemAt(event->position().toPoint());
        logDropEvent("bins", event);
        if (!target) {
            event->ignore();
            return;
        }
        const QString binId = target->data(0, kBinIdRole).toString();
        const QMimeData* data = event->mimeData();
        if (data->hasFormat(QLatin1String(mime::MediaItems)) && onItemsDropped)
            onItemsDropped(idsFromMime(data), binId);
        else if (platform::hasDroppedFiles(data) && onFilesDropped) {
            // Deferred like MediaPoolPanel::dropEvent(): no modal dialog inside the drop.
            const QStringList paths = localFiles(data);
            QTimer::singleShot(0, this, [this, paths, binId] { onFilesDropped(paths, binId); });
        }
        event->acceptProposedAction();
    }
};

} // namespace

MediaPoolPanel::MediaPoolPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Media Pool"), parent)
    , m_ctx(ctx)
{
    setAcceptDrops(true);
    Project* project = ctx.project;

    m_model = new MediaPoolModel(project, this);
    m_proxy = new MediaPoolFilterModel(project, this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_selection = new QItemSelectionModel(m_proxy, this);

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
    connect(newBin, &QAction::triggered, this, [this] { createBin(tr("New Bin"), currentBin()); });
    addTitleBarAction(newBin);

    // ---- Body ----
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* toolbar = new QWidget;
    buildToolbar(toolbar);
    layout->addWidget(toolbar);

    auto* bins = new BinTree;
    m_bins = bins;
    m_bins->setHeaderHidden(true);
    m_bins->setFrameShape(QFrame::NoFrame);
    m_bins->setIndentation(12);
    m_bins->setAcceptDrops(true);
    m_bins->setDropIndicatorShown(true);
    m_bins->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_bins->setContextMenuPolicy(Qt::CustomContextMenu);
    bins->onItemsDropped
        = [this](const QStringList& ids, const QString& binId) { moveItemsToBin(ids, binId); };
    bins->onFilesDropped
        = [this](const QStringList& paths, const QString& binId) { importFiles(paths, binId); };

    m_grid = new QListView;
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setMovement(QListView::Static);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setUniformItemSizes(true); // O(1) layout per item, required for 2,000+ items
    m_grid->setLayoutMode(QListView::Batched);
    m_grid->setBatchSize(200);
    m_grid->setSpacing(kGridSpacing);
    m_grid->setWrapping(true);
    m_grid->setItemDelegate(new MediaPoolDelegate(m_grid));
    m_grid->setGridSize(MediaPoolDelegate::tileSize() + QSize(kGridSpacing, kGridSpacing));

    m_list = new QTreeView;
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(-1, Qt::AscendingOrder); // import order until the user sorts
    m_list->setAllColumnsShowFocus(true);

    for (QAbstractItemView* view :
        {static_cast<QAbstractItemView*>(m_grid), static_cast<QAbstractItemView*>(m_list)}) {
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
        connect(view, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
            const QString id = index.data(MediaPoolModel::IdRole).toString();
            if (id.isEmpty())
                return;
            // Timelines and compound clips open in a timeline tab (Resolve), media in the
            // source viewer.
            const MediaItem* item = m_ctx.project->mediaPool()->item(id);
            if (item && item->isTimeline() && item->kind != MediaItemKind::MulticamClip)
                openTimeline(item->timelineUid); // multicam clips: angles in the source viewer
            else
                emit openInSourceViewer(id);
        });
        connect(view, &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint& pos) { showItemMenu(view->viewport()->mapToGlobal(pos)); });
    }
    QHeaderView* header = m_list->header();
    header->setStretchLastSection(true);
    header->resizeSection(MediaPoolModel::NameColumn, 200);
    header->resizeSection(MediaPoolModel::DurationColumn, 90);
    header->resizeSection(MediaPoolModel::ResolutionColumn, 95);
    header->resizeSection(MediaPoolModel::FpsColumn, 55);
    header->resizeSection(MediaPoolModel::CodecColumn, 70);

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
    connect(m_selection, &QItemSelectionModel::selectionChanged, this,
        &MediaPoolPanel::publishSelection);
    connect(m_bins, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (!m_rebuildingBins && item)
            m_proxy->setBin(item->data(0, kBinIdRole).toString());
    });
    connect(m_bins, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item) {
        if (m_rebuildingBins)
            return;
        const QString id = item->data(0, kBinIdRole).toString();
        if (!id.isEmpty())
            renameBin(id, item->text(0));
    });
    connect(m_bins, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        showBinMenu(m_bins->itemAt(pos), m_bins->viewport()->mapToGlobal(pos));
    });
    MediaPool* pool = project->mediaPool();
    connect(pool, &MediaPool::binsChanged, this, &MediaPoolPanel::rebuildBins);
    connect(pool, &MediaPool::reset, this, &MediaPoolPanel::rebuildBins);
    auto updateEmpty = [this] {
        const bool empty = m_model->rowCount() == 0;
        m_stack->setCurrentIndex(
            empty ? kStackEmpty : (m_viewMode == ViewMode::Grid ? kStackGrid : kStackList));
    };
    connect(m_model, &QAbstractItemModel::rowsInserted, this, updateEmpty);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, updateEmpty);
    connect(m_model, &QAbstractItemModel::modelReset, this, updateEmpty);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) {
        if (s == Qt::ApplicationActive)
            m_model->recheckOffline();
    });

    rebuildBins();
    connectMulticamActions();
    const QVariant binsChoice = QSettings().value(QLatin1String(kBinsVisibleKey));
    if (binsChoice.isValid())
        setBinsVisible(binsChoice.toBool());
    else
        applyAutoBins();
    const QString stored = QSettings().value(QLatin1String(kViewModeKey)).toString();
    setViewMode(stored == QLatin1String("list") ? ViewMode::List : ViewMode::Grid);
}

void MediaPoolPanel::buildToolbar(QWidget* host)
{
    host->setObjectName(QStringLiteral("MediaPoolToolbar"));
    auto* layout = new QHBoxLayout(host);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(4);
    m_binsButton = new QToolButton(host);
    m_binsButton->setIcon(icon(QStringLiteral("folder")));
    m_binsButton->setToolTip(tr("Show bins"));
    m_binsButton->setCheckable(true);
    m_binsButton->setAutoRaise(true);
    connect(m_binsButton, &QToolButton::clicked, this, &MediaPoolPanel::setBinsVisible);
    layout->addWidget(m_binsButton);
    layout->addStretch(1);
    m_search = new QLineEdit(host);
    m_search->setPlaceholderText(tr("Search media"));
    m_search->setClearButtonEnabled(true);
    m_search->addAction(icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    connect(m_search, &QLineEdit::textChanged, this,
        [this](const QString& text) { m_proxy->setSearchText(text); });
    m_search->setMaximumWidth(kSearchMaxWidth);
    layout->addWidget(m_search, 1);

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
    connect(m_gridButton, &QToolButton::clicked, this, [this] { setViewMode(ViewMode::Grid); });
    connect(m_listButton, &QToolButton::clicked, this, [this] { setViewMode(ViewMode::List); });

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
        {MediaPoolModel::NameColumn, tr("Clip Name")},
        {MediaPoolModel::DurationColumn, tr("Duration")},
        {MediaPoolModel::ResolutionColumn, tr("Resolution")},
        {MediaPoolModel::FpsColumn, tr("Frame Rate")}, {MediaPoolModel::CodecColumn, tr("Codec")}};
    for (const auto& [column, label] : keys) {
        QAction* action = sortMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(column == -1);
        action->setData(column);
        columns->addAction(action);
    }
    sortMenu->addSeparator();
    QAction* descending = sortMenu->addAction(tr("Descending"));
    descending->setCheckable(true);
    auto applySort = [this, columns, descending] {
        const int column = columns->checkedAction() ? columns->checkedAction()->data().toInt() : -1;
        // Sorting through the list view keeps its header indicator in sync.
        m_list->sortByColumn(
            column, descending->isChecked() ? Qt::DescendingOrder : Qt::AscendingOrder);
    };
    connect(columns, &QActionGroup::triggered, this, applySort);
    connect(descending, &QAction::triggered, this, applySort);
    sortButton->setMenu(sortMenu);
    layout->addWidget(sortButton);
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
    for (const QModelIndex& index : m_selection->selectedRows(MediaPoolModel::NameColumn))
        ids.append(index.data(MediaPoolModel::IdRole).toString());
    return ids;
}

QString MediaPoolPanel::currentMediaId() const
{
    return m_selection->currentIndex().data(MediaPoolModel::IdRole).toString();
}

QStringList MediaPoolPanel::visibleMediaIds() const
{
    QStringList ids;
    for (int r = 0; r < m_proxy->rowCount(); ++r)
        ids.append(m_proxy->index(r, 0).data(MediaPoolModel::IdRole).toString());
    return ids;
}

QString MediaPoolPanel::currentBin() const
{
    return m_proxy->bin();
}

void MediaPoolPanel::setCurrentBin(const QString& binId)
{
    m_proxy->setBin(binId);
    QTreeWidgetItemIterator it(m_bins);
    for (; *it; ++it) {
        if ((*it)->data(0, kBinIdRole).toString() == binId) {
            const QSignalBlocker block(m_bins);
            m_bins->setCurrentItem(*it);
            break;
        }
    }
}

void MediaPoolPanel::setSearchText(const QString& text)
{
    m_search->setText(text); // textChanged updates the proxy
}

void MediaPoolPanel::setViewMode(ViewMode mode)
{
    m_viewMode = mode;
    m_gridButton->setChecked(mode == ViewMode::Grid);
    m_listButton->setChecked(mode == ViewMode::List);
    if (m_model->rowCount() > 0)
        m_stack->setCurrentIndex(mode == ViewMode::Grid ? kStackGrid : kStackList);
    else
        m_stack->setCurrentIndex(kStackEmpty);
    QSettings().setValue(QLatin1String(kViewModeKey),
        mode == ViewMode::Grid ? QStringLiteral("grid") : QStringLiteral("list"));
}

void MediaPoolPanel::setBinsVisible(bool visible)
{
    m_binsAuto = false;
    m_bins->setVisible(visible);
    m_binsButton->setChecked(visible);
    QSettings().setValue(QLatin1String(kBinsVisibleKey), visible);
}

void MediaPoolPanel::applyAutoBins()
{
    if (!m_binsAuto)
        return;
    const bool visible = width() >= kAutoBinsMinWidth;
    m_bins->setVisible(visible);
    m_binsButton->setChecked(visible);
}

void MediaPoolPanel::resizeEvent(QResizeEvent* event)
{
    PanelFrame::resizeEvent(event);
    applyAutoBins();
}

bool MediaPoolPanel::selectMedia(const QString& id)
{
    const MediaItem* item = m_ctx.project->mediaPool()->item(id);
    if (!item)
        return false;
    m_search->clear();
    setCurrentBin(item->binId);
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

QAbstractItemView* MediaPoolPanel::currentView() const
{
    return m_viewMode == ViewMode::Grid ? static_cast<QAbstractItemView*>(m_grid)
                                        : static_cast<QAbstractItemView*>(m_list);
}

// ---- Bins ----

void MediaPoolPanel::rebuildBins()
{
    // Bins are few (tens): a full rebuild is simpler than incremental updates and keeps the
    // current bin and the expanded state.
    m_rebuildingBins = true;
    QSet<QString> collapsed;
    QTreeWidgetItemIterator it(m_bins);
    for (; *it; ++it) {
        if (!(*it)->isExpanded())
            collapsed.insert((*it)->data(0, kBinIdRole).toString());
    }
    m_proxy->binsChanged();
    const QString current = m_proxy->bin();
    m_bins->clear();

    auto* root = new QTreeWidgetItem(m_bins, {tr("Master")});
    root->setIcon(0, icon(QStringLiteral("folder")));
    root->setData(0, kBinIdRole, QString());
    QHash<QString, QTreeWidgetItem*> items;
    items.insert(QString(), root);
    const QVector<Bin>& bins = m_ctx.project->mediaPool()->bins();
    // Parents may be listed after children: insert in passes until everything is placed.
    QVector<const Bin*> pending;
    for (const Bin& b : bins)
        pending.append(&b);
    while (!pending.isEmpty()) {
        const qsizetype before = pending.size();
        for (qsizetype i = 0; i < pending.size();) {
            const Bin* b = pending[i];
            QTreeWidgetItem* parent = items.value(b->parentId);
            if (!parent && !b->parentId.isEmpty() && !m_ctx.project->mediaPool()->bin(b->parentId))
                parent = root; // dangling parent id: show under Master
            if (!parent) {
                ++i;
                continue;
            }
            auto* item = new QTreeWidgetItem(parent, {b->name});
            item->setIcon(0, icon(QStringLiteral("folder")));
            item->setData(0, kBinIdRole, b->id);
            item->setFlags(item->flags() | Qt::ItemIsEditable);
            items.insert(b->id, item);
            pending.removeAt(i);
        }
        if (pending.size() == before)
            break; // cycle in parent ids: give up on the rest
    }
    for (auto i = items.cbegin(); i != items.cend(); ++i)
        i.value()->setExpanded(!collapsed.contains(i.key()));
    m_bins->setCurrentItem(items.value(current, root));
    m_rebuildingBins = false;
}

QString MediaPoolPanel::createBin(const QString& name, const QString& parentId)
{
    Project* project = m_ctx.project;
    project->beginEdit(tr("New Bin"));
    const QString id = project->mediaPool()->addBin(name, parentId);
    project->endEdit();
    // Start renaming the new bin right away (UI only).
    QTreeWidgetItemIterator it(m_bins);
    for (; *it; ++it) {
        if ((*it)->data(0, kBinIdRole).toString() == id) {
            m_bins->scrollToItem(*it);
            if (isVisible())
                m_bins->editItem(*it);
            break;
        }
    }
    return id;
}

bool MediaPoolPanel::renameBin(const QString& binId, const QString& name)
{
    const Bin* bin = m_ctx.project->mediaPool()->bin(binId);
    const QString trimmed = name.trimmed();
    if (!bin || trimmed.isEmpty() || bin->name == trimmed) {
        rebuildBins(); // restore the shown name
        return false;
    }
    m_ctx.project->beginEdit(tr("Rename Bin"));
    const bool ok = m_ctx.project->mediaPool()->renameBin(binId, trimmed);
    m_ctx.project->endEdit();
    return ok;
}

bool MediaPoolPanel::deleteBin(const QString& binId)
{
    if (binId.isEmpty())
        return false;
    m_ctx.project->beginEdit(tr("Delete Bin"));
    const bool ok = m_ctx.project->mediaPool()->removeBin(binId);
    m_ctx.project->endEdit();
    return ok;
}

int MediaPoolPanel::moveItemsToBin(const QStringList& ids, const QString& binId)
{
    MediaPool* pool = m_ctx.project->mediaPool();
    QStringList toMove;
    for (const QString& id : ids) {
        const MediaItem* item = pool->item(id);
        if (item && item->binId != binId)
            toMove.append(id);
    }
    if (toMove.isEmpty())
        return 0;
    m_ctx.project->beginEdit(tr("Move to Bin"));
    int moved = 0;
    for (const QString& id : std::as_const(toMove))
        moved += pool->moveItemToBin(id, binId) ? 1 : 0;
    m_ctx.project->endEdit();
    return moved;
}

int MediaPoolPanel::deleteItems(const QStringList& ids)
{
    if (ids.isEmpty())
        return 0;
    // Timeline / compound clip items delete their timeline (Project, one step each; the last
    // timeline of a project stays); media items go in one step.
    QStringList files;
    QVector<quint64> timelines;
    for (const QString& id : ids) {
        const MediaItem* item = m_ctx.project->mediaPool()->item(id);
        if (item && item->isTimeline())
            timelines.append(item->timelineUid);
        else
            files.append(id);
    }
    int removed = 0;
    if (!files.isEmpty()) {
        m_ctx.project->beginEdit(
            files.size() == 1 ? tr("Delete Media") : tr("Delete %1 Media Items").arg(files.size()));
        for (const QString& id : std::as_const(files))
            removed += m_ctx.project->mediaPool()->removeItem(id) ? 1 : 0;
        m_ctx.project->endEdit();
    }
    for (const quint64 uid : std::as_const(timelines))
        removed += m_ctx.project->deleteTimeline(uid) ? 1 : 0;
    return removed;
}

void MediaPoolPanel::openTimeline(quint64 uid)
{
    if (m_ctx.project->openTab(uid))
        emit timelineOpened(uid);
}

QStringList MediaPoolPanel::importFiles(const QStringList& paths, const QString& binId)
{
    return importMediaWithFeedback(m_ctx, paths, this, binId);
}

// ---- Context menus & commands ----

void MediaPoolPanel::importWithDialog()
{
    // Same remembered folder as File > Import Media (src/app/ProjectController.cpp).
    const QStringList paths = platform::getOpenFileNames(
        this, tr("Import Media"), mediaFileDialogFilter(), QStringLiteral("paths/lastMediaDir"));
    if (!paths.isEmpty())
        importFiles(paths, currentBin());
}

void MediaPoolPanel::renameCurrentItem()
{
    const QModelIndex current = m_selection->currentIndex();
    if (current.isValid())
        currentView()->edit(current.siblingAtColumn(MediaPoolModel::NameColumn));
}

void MediaPoolPanel::confirmAndDelete(const QStringList& ids)
{
    if (ids.isEmpty())
        return;
    // Count timeline clips that would go offline, so the user knows what happens.
    int used = 0;
    const QSet<QString> idSet(ids.cbegin(), ids.cend());
    QSet<quint64> timelines;
    for (const QString& id : ids) {
        if (const MediaItem* item = m_ctx.project->mediaPool()->item(id);
            item && item->isTimeline())
            timelines.insert(item->timelineUid);
    }
    for (const quint64 uid : m_ctx.project->timelineUids()) {
        const auto data = m_ctx.project->timelineData(uid);
        for (const auto* tracks : {&data->videoTracks, &data->audioTracks}) {
            for (const TrackData& track : *tracks) {
                for (const ClipData& clip : track.clips) {
                    const bool nested = clip.kind == ClipKind::Nested;
                    used += (!nested && idSet.contains(clip.mediaId))
                            || (nested && timelines.contains(clip.nestedUid))
                        ? 1
                        : 0;
                }
            }
        }
    }
    if (used > 0) {
        const auto answer = QMessageBox::question(this, tr("Delete Media"),
            tr("%n clip(s) on the timeline use this media and will show as offline. Delete anyway?",
                nullptr, used));
        if (answer != QMessageBox::Yes)
            return;
    }
    deleteItems(ids);
}

void MediaPoolPanel::revealInFileManager(const QString& id)
{
    const MediaItem* item = m_ctx.project->mediaPool()->item(id);
    if (!item)
        return;
    // Opening the containing folder works on every desktop; selecting the file itself would
    // need the platform-specific FileManager1 D-Bus interface (TODO(panels): src/platform).
    // platform::openUrl: the file manager must start with the user's environment, not the
    // bundle's library/plugin paths.
    platform::openUrl(QUrl::fromLocalFile(QFileInfo(item->path).absolutePath()));
}

void MediaPoolPanel::showItemMenu(const QPoint& globalPos)
{
    const QStringList ids = selectedMediaIds();
    const QString current = currentMediaId();
    QMenu menu(this);
    const MediaItem* currentItem = m_ctx.project->mediaPool()->item(current);
    if (currentItem && currentItem->isTimeline()) {
        const quint64 uid = currentItem->timelineUid;
        if (currentItem->kind == MediaItemKind::MulticamClip)
            menu.addAction(tr("Open in Source Viewer"), this,
                [this, current] { emit openInSourceViewer(current); });
        menu.addAction(tr("Open in Timeline"), this, [this, uid] { openTimeline(uid); });
        menu.addAction(currentItem->kind == MediaItemKind::MulticamClip
                ? tr("Duplicate Multicam Clip")
                : currentItem->kind == MediaItemKind::CompoundClip ? tr("Duplicate Compound Clip")
                                                                   : tr("Duplicate Timeline"),
            this, [this, uid] { m_ctx.project->duplicateTimeline(uid); });
        menu.addSeparator();
        menu.addAction(tr("Rename"), this, &MediaPoolPanel::renameCurrentItem);
    } else if (!current.isEmpty()) {
        menu.addAction(tr("Open in Source Viewer"), this,
            [this, current] { emit openInSourceViewer(current); });
        menu.addSeparator();
        menu.addAction(tr("Rename"), this, &MediaPoolPanel::renameCurrentItem);
        menu.addAction(
            tr("Reveal in File Manager"), this, [this, current] { revealInFileManager(current); });
        menu.addSeparator();
        addMulticamMenu(menu, ids);
    }
    if (!current.isEmpty()) {
        QMenu* move = menu.addMenu(tr("Move to Bin"));
        move->addAction(tr("Master"), this, [this, ids] { moveItemsToBin(ids, QString()); });
        for (const Bin& bin : m_ctx.project->mediaPool()->bins()) {
            const QString binId = bin.id;
            move->addAction(bin.name, this, [this, ids, binId] { moveItemsToBin(ids, binId); });
        }
        // Proxy / optimized media (Resolve's media pool context menu entries).
        if (m_ctx.actions && m_ctx.media && currentItem && !currentItem->isTimeline()) {
            publishSelection();
            menu.addSeparator();
            for (const char* id : {"mediapool.generate_optimized", "mediapool.generate_proxy",
                     "mediapool.stop_generation", "mediapool.link_proxy", "mediapool.unlink_proxy",
                     "mediapool.delete_proxy", "mediapool.delete_optimized"}) {
                if (m_ctx.actions->contains(QLatin1String(id)))
                    menu.addAction(m_ctx.actions->action(QLatin1String(id)));
            }
        }
        menu.addSeparator();
        menu.addAction(icon(QStringLiteral("remove")),
            ids.size() > 1 ? tr("Delete %1 Items").arg(ids.size()) : tr("Delete"), this,
            [this, ids] { confirmAndDelete(ids); });
        menu.addSeparator();
    }
    menu.addAction(icon(QStringLiteral("import")), tr("Import Media…"), this,
        &MediaPoolPanel::importWithDialog);
    menu.addAction(icon(QStringLiteral("folder")), tr("New Bin"), this,
        [this] { createBin(tr("New Bin"), currentBin()); });
    if (m_ctx.actions && m_ctx.actions->contains(QStringLiteral("timeline.new")))
        menu.addAction(m_ctx.actions->action(QStringLiteral("timeline.new")));
    // Only offered when something is offline (one stat per item when the menu opens).
    const auto& items = m_ctx.project->mediaPool()->state().items;
    if (std::any_of(items.cbegin(), items.cend(),
            [](const MediaItem& m) { return !m.isTimeline() && !QFileInfo::exists(m.path); })) {
        menu.addSeparator();
        menu.addAction(tr("Relink Offline Media…"), this, &MediaPoolPanel::relinkWithDialog);
    }
    menu.exec(globalPos);
}

int MediaPoolPanel::relinkFromFolder(const QString& folder)
{
    Project* project = m_ctx.project;
    project->beginEdit(tr("Relink Media"));
    const int relinked = project->mediaPool()->relinkFromFolder(folder);
    project->endEdit();
    m_model->recheckOffline();
    return relinked;
}

void MediaPoolPanel::relinkWithDialog()
{
    const QString folder = platform::getExistingDirectory(this,
        tr("Relink Offline Media: Choose the Folder with the Files"),
        QStringLiteral("paths/lastRelinkDir"));
    if (folder.isEmpty())
        return;
    const int relinked = relinkFromFolder(folder);
    const auto& items = m_ctx.project->mediaPool()->state().items;
    const auto stillOffline = std::count_if(items.cbegin(), items.cend(),
        [](const MediaItem& m) { return !m.isTimeline() && !QFileInfo::exists(m.path); });
    QMessageBox::information(this, tr("Relink Offline Media"),
        tr("%n item(s) relinked.", nullptr, relinked)
            + (stillOffline > 0 ? QLatin1Char(' ')
                        + tr("%n item(s) are still offline (no file with the same name in "
                             "that folder).",
                            nullptr, int(stillOffline))
                                : QString()));
}

void MediaPoolPanel::showBinMenu(QTreeWidgetItem* item, const QPoint& globalPos)
{
    const QString binId = item ? item->data(0, kBinIdRole).toString() : QString();
    QMenu menu(this);
    menu.addAction(icon(QStringLiteral("folder")), tr("New Bin"), this,
        [this, binId] { createBin(tr("New Bin"), binId); });
    if (item && !binId.isEmpty()) {
        menu.addAction(tr("Rename Bin"), this, [this, item] { m_bins->editItem(item); });
        menu.addAction(icon(QStringLiteral("remove")), tr("Delete Bin"), this,
            [this, binId] { deleteBin(binId); });
    }
    menu.exec(globalPos);
}

// ---- OS drops onto the item area ----

void MediaPoolPanel::dragEnterEvent(QDragEnterEvent* event)
{
    if (platform::hasDroppedFiles(event->mimeData()))
        event->acceptProposedAction();
    else
        event->ignore();
    logDropEvent("media pool", event);
}

void MediaPoolPanel::dragMoveEvent(QDragMoveEvent* event)
{
    // Explicit, so every move answers the compositor (Wayland: wl_data_offer.accept +
    // set_actions per motion); an unanswered offer is treated as "cannot drop here".
    if (platform::hasDroppedFiles(event->mimeData()))
        event->acceptProposedAction();
    else
        event->ignore();
}

void MediaPoolPanel::dropEvent(QDropEvent* event)
{
    logDropEvent("media pool", event);
    const QStringList paths = localFiles(event->mimeData());
    if (paths.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    // From the event loop: importing may show a modal report, which must not run inside the
    // drop (the source would wait for the drop to finish).
    const QString bin = currentBin();
    QTimer::singleShot(0, this, [this, paths, bin] { importFiles(paths, bin); });
}

} // namespace kk
