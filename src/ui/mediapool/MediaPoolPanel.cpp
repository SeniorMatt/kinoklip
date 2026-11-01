// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolPanel implementation.
#include "ui/mediapool/MediaPoolPanel.h"

#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "media/MediaCache.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
#include "ui/common/Icons.h"
#include "ui/common/MediaImport.h"
#include "ui/mediapool/MediaPoolDelegate.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
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
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace kk {

namespace {

constexpr int kBinIdRole = Qt::UserRole;
constexpr char kViewModeKey[] = "mediaPool/viewMode";
constexpr char kBinsVisibleKey[] = "mediaPool/binsVisible";
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

QStringList localFiles(const QMimeData* data)
{
    QStringList paths;
    for (const QUrl& url : data->urls()) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    return paths;
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
            || event->mimeData()->hasUrls())
            event->acceptProposedAction();
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
    }

    void dropEvent(QDropEvent* event) override
    {
        QTreeWidgetItem* target = itemAt(event->position().toPoint());
        if (!target)
            return;
        const QString binId = target->data(0, kBinIdRole).toString();
        const QMimeData* data = event->mimeData();
        if (data->hasFormat(QLatin1String(mime::MediaItems)) && onItemsDropped)
            onItemsDropped(idsFromMime(data), binId);
        else if (data->hasUrls() && onFilesDropped)
            onFilesDropped(localFiles(data), binId);
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
        view->setAcceptDrops(false); // OS file drops are handled by the panel
        view->setEditTriggers(QAbstractItemView::EditKeyPressed);
        view->setContextMenuPolicy(Qt::CustomContextMenu);
        view->setMouseTracking(true);
        connect(view, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
            const QString id = index.data(MediaPoolModel::IdRole).toString();
            if (!id.isEmpty())
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

    m_empty = createPlaceholderContent(
        tr("No media yet.\nImport files with Ctrl+I or drop them here."));
    m_stack = new QStackedWidget;
    m_stack->insertWidget(kStackGrid, m_grid);
    m_stack->insertWidget(kStackList, m_list);
    m_stack->insertWidget(kStackEmpty, m_empty);

    m_splitter = new QSplitter(Qt::Horizontal);
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
    setBinsVisible(QSettings().value(QLatin1String(kBinsVisibleKey), true).toBool());
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
    m_search = new QLineEdit(host);
    m_search->setPlaceholderText(tr("Search media"));
    m_search->setClearButtonEnabled(true);
    m_search->addAction(icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    connect(m_search, &QLineEdit::textChanged, this,
        [this](const QString& text) { m_proxy->setSearchText(text); });
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
}

// ---- State ----

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
    m_bins->setVisible(visible);
    m_binsButton->setChecked(visible);
    QSettings().setValue(QLatin1String(kBinsVisibleKey), visible);
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

void MediaPoolPanel::setThumbnailProvider(ThumbnailProvider provider)
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
    m_ctx.project->beginEdit(
        ids.size() == 1 ? tr("Delete Media") : tr("Delete %1 Media Items").arg(ids.size()));
    int removed = 0;
    for (const QString& id : ids)
        removed += m_ctx.project->mediaPool()->removeItem(id) ? 1 : 0;
    m_ctx.project->endEdit();
    return removed;
}

QStringList MediaPoolPanel::importFiles(const QStringList& paths, const QString& binId)
{
    return importMediaWithFeedback(m_ctx, paths, this, binId);
}

// ---- Context menus & commands ----

void MediaPoolPanel::importWithDialog()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Import Media"), QString(), mediaFileDialogFilter());
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
    const TimelineData& data = m_ctx.project->timeline()->data();
    for (const auto* tracks : {&data.videoTracks, &data.audioTracks}) {
        for (const TrackData& track : *tracks) {
            for (const ClipData& clip : track.clips)
                used += idSet.contains(clip.mediaId) ? 1 : 0;
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
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(item->path).absolutePath()));
}

void MediaPoolPanel::showItemMenu(const QPoint& globalPos)
{
    const QStringList ids = selectedMediaIds();
    const QString current = currentMediaId();
    QMenu menu(this);
    if (!current.isEmpty()) {
        menu.addAction(tr("Open in Source Viewer"), this,
            [this, current] { emit openInSourceViewer(current); });
        menu.addSeparator();
        menu.addAction(tr("Rename"), this, &MediaPoolPanel::renameCurrentItem);
        menu.addAction(
            tr("Reveal in File Manager"), this, [this, current] { revealInFileManager(current); });
        QMenu* move = menu.addMenu(tr("Move to Bin"));
        move->addAction(tr("Master"), this, [this, ids] { moveItemsToBin(ids, QString()); });
        for (const Bin& bin : m_ctx.project->mediaPool()->bins()) {
            const QString binId = bin.id;
            move->addAction(bin.name, this, [this, ids, binId] { moveItemsToBin(ids, binId); });
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
    menu.exec(globalPos);
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
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MediaPoolPanel::dropEvent(QDropEvent* event)
{
    const QStringList paths = localFiles(event->mimeData());
    event->acceptProposedAction();
    importFiles(paths, currentBin());
}

} // namespace kk
