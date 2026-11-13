// SPDX-License-Identifier: GPL-3.0-or-later
// MediaBrowserPanel implementation.
#include "ui/mediapool/MediaBrowserPanel.h"

#include "ui/common/AppContext.h"
#include "ui/common/FolderImport.h"
#include "ui/common/Icons.h"
#include "ui/common/MediaImport.h"
#include "ui/common/PanelInfo.h"
#include "ui/common/PanelSplitter.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardPaths>
#include <QTreeView>
#include <QVBoxLayout>

namespace kk {

namespace {
constexpr int kNameColumnWidth = 190;
constexpr int kSizeColumn = 1;
constexpr int kTypeColumn = 2;
constexpr int kDateColumn = 3;

/// Hides directories from the file list. QFileSystemModel keeps nodes for the ancestors of
/// every path it has seen (e.g. the working directory) even with a Files-only filter.
class FilesOnlyProxy final : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override
    {
        const auto* fs = static_cast<const QFileSystemModel*>(sourceModel());
        const QModelIndex index = fs->index(row, 0, parent);
        // Keep the ancestors of the listed folder (they hold the rows), hide other folders.
        return !fs->isDir(index) || m_folder.startsWith(fs->filePath(index));
    }

public:
    QString m_folder; ///< folder being listed
};
} // namespace

MediaBrowserPanel::MediaBrowserPanel(AppContext& ctx, QWidget* parent)
    : PanelFrame(tr("Media Storage"), parent)
    , m_ctx(ctx)
{
    setPanelInfo(this, tr("Media Storage"),
        tr("Browses the folders of this computer and adds media files to the media pool."));
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- Places ----
    auto* placesRow = new QWidget;
    auto* placesLayout = new QHBoxLayout(placesRow);
    placesLayout->setContentsMargins(6, 4, 6, 4);
    m_places = new QComboBox;
    struct Place
    {
        QStandardPaths::StandardLocation location;
        const char* label;
    };
    const Place places[] = {
        {QStandardPaths::HomeLocation, QT_TR_NOOP("Home")},
        {QStandardPaths::MoviesLocation, QT_TR_NOOP("Videos")},
        {QStandardPaths::PicturesLocation, QT_TR_NOOP("Pictures")},
        {QStandardPaths::MusicLocation, QT_TR_NOOP("Music")},
        {QStandardPaths::DesktopLocation, QT_TR_NOOP("Desktop")},
    };
    for (const Place& place : places) {
        const QString path = QStandardPaths::writableLocation(place.location);
        if (!path.isEmpty() && QDir(path).exists() && m_places->findData(path) < 0)
            m_places->addItem(icon(QStringLiteral("folder")), tr(place.label), path);
    }
    m_places->addItem(icon(QStringLiteral("folder")), tr("File System"), QDir::rootPath());
    placesLayout->addWidget(m_places, 1);
    layout->addWidget(placesRow);

    // ---- Folder tree ----
    m_dirs = new QFileSystemModel(this);
    m_dirs->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);
    m_dirs->setReadOnly(true);
    m_tree = new QTreeView;
    m_tree->setModel(m_dirs);
    m_tree->setHeaderHidden(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setUniformRowHeights(true);
    for (int column = 1; column < m_dirs->columnCount(); ++column)
        m_tree->hideColumn(column);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);

    // ---- Media files of the selected folder ----
    m_files = new QFileSystemModel(this);
    m_files->setFilter(QDir::Files | QDir::NoDotAndDotDot);
    m_files->setReadOnly(true);
    m_files->setNameFilters(mediaFileNamePatterns());
    m_files->setNameFilterDisables(false); // hide, rather than grey out, non-media files
    m_filesProxy = new FilesOnlyProxy(this);
    m_filesProxy->setSourceModel(m_files);
    m_filesProxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_list = new QTreeView;
    m_list->setModel(m_filesProxy);
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(0, Qt::AscendingOrder);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->hideColumn(kTypeColumn);
    m_list->header()->resizeSection(0, kNameColumnWidth);
    m_list->header()->setSectionResizeMode(kSizeColumn, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(kDateColumn, QHeaderView::ResizeToContents);

    auto* splitter = new PanelSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1);
    splitter->addWidget(m_tree);
    splitter->addWidget(m_list);
    splitter->setSizes({220, 300});
    layout->addWidget(splitter, 1);

    // ---- Add button ----
    auto* buttonRow = new QWidget;
    auto* buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(6, 4, 6, 6);
    m_add = new QPushButton(icon(QStringLiteral("import")), tr("Add to Media Pool"));
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(m_add);
    layout->addWidget(buttonRow);
    setContentWidget(content);

    connect(m_places, &QComboBox::activated, this,
        [this](int index) { setRootPath(m_places->itemData(index).toString()); });
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& index) {
            if (index.isValid())
                showFolder(m_dirs->filePath(index));
        });
    connect(m_list->selectionModel(), &QItemSelectionModel::selectionChanged, this,
        &MediaBrowserPanel::updateButton);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& index) {
            if (index.isValid())
                emit fileSelected(m_files->filePath(m_filesProxy->mapToSource(index)));
        });
    connect(m_list, &QTreeView::doubleClicked, this, [this] { addSelectedToMediaPool(); });
    connect(m_add, &QPushButton::clicked, this, [this] { addSelectedToMediaPool(); });
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (selectedFiles().isEmpty())
            return;
        QMenu menu(this);
        menu.addAction(icon(QStringLiteral("import")), tr("Add to Media Pool"), this,
            [this] { addSelectedToMediaPool(); });
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });

    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = m_tree->indexAt(pos);
        if (!index.isValid())
            return;
        const QString folder = m_dirs->filePath(index);
        QMenu menu(this);
        menu.addAction(icon(QStringLiteral("import")), tr("Add Folder into Media Pool"), this,
            [this, folder] { addFolderToMediaPool(folder, false); });
        menu.addAction(icon(QStringLiteral("folder")),
            tr("Add Folder and Subfolders into Media Pool (Create Bins)"), this,
            [this, folder] { addFolderToMediaPool(folder, true); });
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
    });

    qApp->installEventFilter(this); // mouse back / forward anywhere over the panel
    setRootPath(QDir::homePath());
    updateButton();
}

void MediaBrowserPanel::setRootPath(const QString& path)
{
    m_tree->setRootIndex(m_dirs->setRootPath(path));
    const int place = m_places->findData(path);
    if (place >= 0)
        m_places->setCurrentIndex(place);
    showFolder(path);
}

void MediaBrowserPanel::showFolder(const QString& path)
{
    if (path == m_folder)
        return;
    m_folder = path;
    if (!m_navigating) {
        m_history.resize(m_historyPos + 1); // a new folder drops the forward entries
        m_history.append(path);
        m_historyPos = int(m_history.size()) - 1;
    }
    static_cast<FilesOnlyProxy*>(m_filesProxy)->m_folder = path;
    const QModelIndex root = m_files->setRootPath(path);
    m_filesProxy->invalidate();
    m_list->setRootIndex(m_filesProxy->mapFromSource(root));
    m_list->clearSelection();
    updateButton();
}

void MediaBrowserPanel::navigateHistory(int direction)
{
    const int pos = m_historyPos + direction;
    if (pos < 0 || pos >= m_history.size())
        return;
    m_navigating = true;
    m_historyPos = pos;
    const QString path = m_history.at(pos);
    const QModelIndex index = m_dirs->index(path);
    if (index.isValid())
        m_tree->setCurrentIndex(index); // showFolder() runs from the selection change
    showFolder(path);
    m_navigating = false;
}

bool MediaBrowserPanel::eventFilter(QObject* watched, QEvent* event)
{
    // The mouse back / forward buttons over the browser step through the folders shown.
    if ((event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick
            || event->type() == QEvent::MouseButtonRelease)
        && watched->isWidgetType() && isVisible()) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const auto* w = static_cast<const QWidget*>(watched);
        if ((mouse->button() == Qt::BackButton || mouse->button() == Qt::ForwardButton)
            && (w == this || isAncestorOf(w))) {
            if (event->type() != QEvent::MouseButtonRelease)
                navigateHistory(mouse->button() == Qt::BackButton ? -1 : 1);
            return true;
        }
    }
    return PanelFrame::eventFilter(watched, event);
}

QStringList MediaBrowserPanel::selectedFiles() const
{
    QStringList paths;
    for (const QModelIndex& proxyIndex : m_list->selectionModel()->selectedRows(0)) {
        const QModelIndex index = m_filesProxy->mapToSource(proxyIndex);
        if (!m_files->isDir(index))
            paths.append(m_files->filePath(index));
    }
    return paths;
}

QStringList MediaBrowserPanel::addSelectedToMediaPool()
{
    return importMediaWithFeedback(m_ctx, selectedFiles(), this);
}

QStringList MediaBrowserPanel::addFolderToMediaPool(const QString& path, bool withSubfolders)
{
    FolderImportOptions options;
    options.includeSubfolders = withSubfolders;
    options.createBins = withSubfolders; // the folder's own files go straight into Master
    return importMediaFoldersWithFeedback(m_ctx, {path}, options, this);
}

void MediaBrowserPanel::updateButton()
{
    const int count = int(m_list->selectionModel()->selectedRows(0).size());
    m_add->setEnabled(count > 0);
    m_add->setText(
        count > 1 ? tr("Add %1 Files to Media Pool").arg(count) : tr("Add to Media Pool"));
}

} // namespace kk
