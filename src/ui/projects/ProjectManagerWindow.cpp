// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectManagerWindow implementation: layout, navigation, actions, keys, drag and drop.
#include "ui/projects/ProjectManagerWindow.h"

#include "pack/EmbeddedMedia.h"
#include "pack/ProjectPackage.h"
#include "platform/FileDialogs.h"
#include "platform/FileManager.h"
#include "projects/ProjectLibrary.h"
#include "ui/common/Icons.h"
#include "ui/common/Metrics.h"
#include "ui/common/ProgressTask.h"
#include "ui/common/SelectedRows.h"
#include "ui/projects/NewProjectDialog.h"
#include "ui/projects/ProjectCardDelegate.h"
#include "ui/projects/ProjectLibraryModel.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace kk {

namespace {

constexpr char kSettingsGroup[] = "projectManager";
constexpr char kLastDirKey[] = "paths/projectManagerImportDir";
constexpr int kGridSpacing = 10; ///< px between cards
const QSize kDefaultSize(1100, 700);

QString projectFilter()
{
    QStringList patterns = projectkinds::nameFilters();
    return QObject::tr("Projects (%1);;All files (*)").arg(patterns.join(QLatin1Char(' ')));
}

/// A part of the folder path: clicking shows that folder; library items dropped on it move
/// there (like dropping on a folder card).
class CrumbButton final : public QToolButton
{
public:
    CrumbButton(QString folder, ProjectLibraryModel* model, QWidget* parent)
        : QToolButton(parent)
        , m_folder(std::move(folder))
        , m_model(model)
    {
        setAcceptDrops(true);
        setAutoRaise(true);
    }

protected:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event->mimeData()->hasFormat(QLatin1String(ProjectLibraryModel::kMimeType)))
            event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override
    {
        const QStringList paths = QString::fromUtf8(
            event->mimeData()->data(QLatin1String(ProjectLibraryModel::kMimeType)))
                                      .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        m_model->dropPaths(paths, m_folder);
        event->acceptProposedAction();
    }

private:
    QString m_folder;
    ProjectLibraryModel* m_model;
};

/// The list view's delegate: the default look, with the card delegate's rename-editor guard.
class ListDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    {
        if (!ProjectCardDelegate::editorAlreadyFilled(editor))
            QStyledItemDelegate::setEditorData(editor, index);
    }
};

/// kk::icon(@p name) when the icon theme or our resources have it, else the style's
/// @p fallback (the fallback build without the Breeze icon theme).
QIcon iconOr(const QString& name, QStyle::StandardPixmap fallback, const QStyle* style)
{
    if (hasIcon(name) || QIcon::hasThemeIcon(themeIconName(name)))
        return icon(name);
    return style->standardIcon(fallback);
}

} // namespace

ProjectManagerWindow::ProjectManagerWindow(ProjectLibrary* library, QWidget* parent)
    : QDialog(parent, Qt::Window)
    , m_library(library)
{
    setObjectName(QStringLiteral("ProjectManager"));
    setWindowTitle(tr("Project Manager"));
    setSizeGripEnabled(true);
    m_model = new ProjectLibraryModel(library, this);
    m_proxy = new ProjectSortProxy(this);
    m_proxy->setSourceModel(m_model);
    m_delegate = new ProjectCardDelegate(this);
    buildUi();
    restoreSettings();
    connect(
        m_model, &ProjectLibraryModel::operationFailed, this, &ProjectManagerWindow::reportError);
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] {
        rebuildCrumbs();
        updateActions();
    });
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, &ProjectManagerWindow::updateActions);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, &ProjectManagerWindow::updateActions);
    showFolder();
}

ProjectManagerWindow::~ProjectManagerWindow()
{
    saveSettings();
}

void ProjectManagerWindow::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM);
    root->setSpacing(metrics::kSpaceM);

    // ---- Top bar: path, search, sort, view, size ----
    auto* top = new QHBoxLayout;
    top->setSpacing(metrics::kSpaceS);
    m_upButton = new QToolButton;
    m_upButton->setIcon(iconOr(QStringLiteral("go_up"), QStyle::SP_FileDialogToParent, style()));
    m_upButton->setToolTip(tr("Parent folder (Backspace)"));
    m_upButton->setAutoRaise(true);
    connect(m_upButton, &QToolButton::clicked, this, &ProjectManagerWindow::goUp);
    top->addWidget(m_upButton);
    m_crumbs = new QWidget;
    m_crumbLayout = new QHBoxLayout(m_crumbs);
    m_crumbLayout->setContentsMargins(0, 0, 0, 0);
    m_crumbLayout->setSpacing(0);
    top->addWidget(m_crumbs, 1);

    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("ProjectSearch"));
    m_search->setPlaceholderText(tr("Search"));
    m_search->setClearButtonEnabled(true);
    m_search->addAction(icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    m_search->setMinimumWidth(180);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& t) {
        m_proxy->setSearchText(t.trimmed());
        updateActions();
    });
    top->addWidget(m_search);

    m_sortCombo = new QComboBox;
    m_sortCombo->setToolTip(tr("Sort by"));
    m_sortCombo->addItem(tr("Name"), int(ProjectLibraryModel::NameColumn));
    m_sortCombo->addItem(tr("Date Modified"), int(ProjectLibraryModel::ModifiedColumn));
    m_sortCombo->addItem(tr("Duration"), int(ProjectLibraryModel::DurationColumn));
    connect(m_sortCombo, &QComboBox::activated, this,
        [this] { sortBy(m_sortCombo->currentData().toInt(), m_proxy->sortOrder()); });
    top->addWidget(m_sortCombo);
    m_orderButton = new QToolButton;
    m_orderButton->setAutoRaise(true);
    m_orderButton->setToolTip(tr("Ascending / descending"));
    connect(m_orderButton, &QToolButton::clicked, this, [this] {
        sortBy(m_proxy->sortColumn(),
            m_proxy->sortOrder() == Qt::AscendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder);
    });
    top->addWidget(m_orderButton);

    m_cardsButton = new QToolButton;
    m_cardsButton->setIcon(icon(QStringLiteral("grid_view")));
    m_cardsButton->setToolTip(tr("Thumbnail view"));
    m_listButton = new QToolButton;
    m_listButton->setIcon(icon(QStringLiteral("list_view")));
    m_listButton->setToolTip(tr("List view"));
    for (QToolButton* b : {m_cardsButton, m_listButton}) {
        b->setCheckable(true);
        b->setAutoRaise(true);
        top->addWidget(b);
    }
    connect(m_cardsButton, &QToolButton::clicked, this, [this] { setCardView(true); });
    connect(m_listButton, &QToolButton::clicked, this, [this] { setCardView(false); });

    m_sizeSlider = new QSlider(Qt::Horizontal);
    m_sizeSlider->setObjectName(QStringLiteral("CardSize"));
    m_sizeSlider->setToolTip(tr("Thumbnail size"));
    m_sizeSlider->setRange(ProjectCardDelegate::kMinWidth, ProjectCardDelegate::kMaxWidth);
    m_sizeSlider->setValue(ProjectCardDelegate::kDefaultWidth);
    m_sizeSlider->setFixedWidth(110);
    connect(m_sizeSlider, &QSlider::valueChanged, this, &ProjectManagerWindow::applyCardWidth);
    top->addWidget(m_sizeSlider);
    root->addLayout(top);

    // ---- Places | views ----
    auto* split = new QSplitter(Qt::Horizontal);
    m_places = new QListWidget;
    m_places->setObjectName(QStringLiteral("ProjectPlaces"));
    auto* libraryItem = new QListWidgetItem(icon(QStringLiteral("folder")), tr("Project Library"));
    libraryItem->setToolTip(QDir::toNativeSeparators(m_library->root()));
    m_places->addItem(libraryItem);
    m_places->addItem(new QListWidgetItem(icon(QStringLiteral("film")), tr("Recent Projects")));
    m_places->setCurrentRow(0);
    connect(m_places, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (m_places->row(item) == 0)
            showFolder();
        else
            showRecent();
    });
    split->addWidget(m_places);

    m_views = new QStackedWidget;
    m_cardView = new QListView;
    m_cardView->setObjectName(QStringLiteral("ProjectCards"));
    m_cardView->setViewMode(QListView::IconMode);
    m_cardView->setMovement(QListView::Static); // drops go to the model (folders), no free layout
    m_cardView->setResizeMode(QListView::Adjust);
    m_cardView->setUniformItemSizes(true); // one sizeHint() for all: no per-row layout cost
    m_cardView->setSpacing(kGridSpacing / 2);
    m_cardView->setWrapping(true);
    m_cardView->setItemDelegate(m_delegate);
    m_cardView->setMouseTracking(true);
    m_listView = new QTreeView;
    m_listView->setObjectName(QStringLiteral("ProjectList"));
    m_listView->setRootIsDecorated(false);
    m_listView->setUniformRowHeights(true);
    m_listView->setAllColumnsShowFocus(true);
    m_listView->setSortingEnabled(true);
    m_listView->setItemDelegate(new ListDelegate(m_listView));
    for (QAbstractItemView* v : {static_cast<QAbstractItemView*>(m_cardView),
             static_cast<QAbstractItemView*>(m_listView)}) {
        v->setModel(m_proxy);
        v->setSelectionMode(QAbstractItemView::ExtendedSelection);
        v->setDragEnabled(true);
        v->setAcceptDrops(true);
        v->setDropIndicatorShown(true);
        v->setDragDropMode(QAbstractItemView::DragDrop);
        v->setDefaultDropAction(Qt::MoveAction);
        v->setEditTriggers(QAbstractItemView::EditKeyPressed);
        v->setContextMenuPolicy(Qt::CustomContextMenu);
        v->installEventFilter(this);
        connect(v, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& i) {
            openPath(i.data(ProjectLibraryModel::PathRole).toString());
        });
        connect(v, &QWidget::customContextMenuRequested, this,
            [this, v](const QPoint& pos) { showContextMenu(v, pos); });
        connect(v->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &ProjectManagerWindow::updateActions);
    }
    m_listView->header()->setSectionResizeMode(
        ProjectLibraryModel::NameColumn, QHeaderView::Stretch);
    m_listView->header()->setStretchLastSection(false);
    m_listView->header()->setSortIndicatorShown(true);
    connect(m_listView->header(), &QHeaderView::sortIndicatorChanged, this,
        [this](int column, Qt::SortOrder order) { sortBy(column, order); });
    m_views->addWidget(m_cardView);
    m_views->addWidget(m_listView);
    split->addWidget(m_views);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({180, 900});
    root->addWidget(split, 1);

    // ---- Bottom bar ----
    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(metrics::kSpaceS);
    m_importButton = new QPushButton(icon(QStringLiteral("import")), tr("Import..."));
    m_importButton->setToolTip(tr("Copy a project file into the library"));
    m_exportButton = new QPushButton(
        iconOr(QStringLiteral("export"), QStyle::SP_DialogSaveButton, style()), tr("Export..."));
    m_exportButton->setToolTip(tr("Save a copy of the selected project"));
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("ProjectStatus"));
    m_newFolderButton = new QPushButton(icon(QStringLiteral("folder")), tr("New Folder"));
    m_newProjectButton = new QPushButton(icon(QStringLiteral("add")), tr("New Project"));
    m_openButton = new QPushButton(tr("Open"));
    m_openButton->setDefault(true);
    connect(m_importButton, &QPushButton::clicked, this, &ProjectManagerWindow::importProject);
    connect(m_exportButton, &QPushButton::clicked, this, &ProjectManagerWindow::exportSelected);
    connect(m_newFolderButton, &QPushButton::clicked, this, &ProjectManagerWindow::newFolder);
    connect(m_newProjectButton, &QPushButton::clicked, this, &ProjectManagerWindow::newProject);
    connect(m_openButton, &QPushButton::clicked, this, &ProjectManagerWindow::openSelected);
    bottom->addWidget(m_importButton);
    bottom->addWidget(m_exportButton);
    bottom->addStretch(1);
    bottom->addWidget(m_status);
    bottom->addStretch(1);
    bottom->addWidget(m_newFolderButton);
    bottom->addWidget(m_newProjectButton);
    bottom->addWidget(m_openButton);
    root->addLayout(bottom);
}

// ---- Navigation ----

void ProjectManagerWindow::showFolder(const QString& folder)
{
    m_places->setCurrentRow(0);
    m_model->showFolder(folder.isEmpty() || !m_library->contains(folder) ? QString() : folder);
}

void ProjectManagerWindow::showRecent()
{
    m_places->setCurrentRow(1);
    m_model->showRecent(m_recent);
}

void ProjectManagerWindow::goUp()
{
    if (m_model->isRecent() || m_model->folder() == m_library->root())
        return;
    const QString from = m_model->folder();
    showFolder(QFileInfo(from).absolutePath());
    selectPath(from);
}

void ProjectManagerWindow::setOpenProjectPath(const QString& path)
{
    m_library->setOpenProjectPath(path);
    m_model->refresh();
}

void ProjectManagerWindow::setRecentProjects(const QStringList& paths)
{
    m_recent = paths;
    if (m_model->isRecent())
        m_model->showRecent(m_recent);
}

void ProjectManagerWindow::rebuildCrumbs()
{
    while (QLayoutItem* item = m_crumbLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    QStringList parts;
    QStringList folders;
    if (m_model->isRecent()) {
        parts << tr("Recent Projects");
        folders << QString();
    } else {
        // Library root, then each sub-folder down to the shown one.
        const QString rel = QDir(m_library->root()).relativeFilePath(m_model->folder());
        parts << tr("Project Library");
        folders << m_library->root();
        QString path = m_library->root();
        if (rel != QLatin1String(".")) {
            for (const QString& part : rel.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
                path += QLatin1Char('/') + part;
                parts << part;
                folders << path;
            }
        }
    }
    for (int i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            auto* sep = new QLabel(QStringLiteral(" / "));
            sep->setEnabled(false);
            m_crumbLayout->addWidget(sep);
        }
        auto* b = new CrumbButton(folders[i], m_model, m_crumbs);
        b->setText(parts[i]);
        b->setEnabled(!folders[i].isEmpty());
        if (i == parts.size() - 1) {
            QFont f = b->font();
            f.setBold(true);
            b->setFont(f);
        }
        const QString folder = folders[i];
        connect(b, &QToolButton::clicked, this, [this, folder] { showFolder(folder); });
        m_crumbLayout->addWidget(b);
    }
    m_crumbLayout->addStretch(1);
    m_upButton->setEnabled(!m_model->isRecent() && m_model->folder() != m_library->root());
}

// ---- View options ----

void ProjectManagerWindow::setCardView(bool cards)
{
    QAbstractItemView* from = activeView();
    QAbstractItemView* to = cards ? static_cast<QAbstractItemView*>(m_cardView) : m_listView;
    if (from != to) {
        // Keep the selection and the current item across the switch (same proxy model).
        to->selectionModel()->select(from->selectionModel()->selection(),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        to->selectionModel()->setCurrentIndex(from->currentIndex(), QItemSelectionModel::NoUpdate);
    }
    m_views->setCurrentWidget(to);
    m_cardsButton->setChecked(cards);
    m_listButton->setChecked(!cards);
    m_sizeSlider->setEnabled(cards);
}

bool ProjectManagerWindow::isCardView() const
{
    return m_views->currentWidget() == m_cardView;
}

void ProjectManagerWindow::sortBy(int column, Qt::SortOrder order)
{
    m_proxy->sort(column, order);
    {
        const QSignalBlocker block(m_listView->header());
        m_listView->header()->setSortIndicator(column, order);
    }
    const int i = m_sortCombo->findData(column);
    if (i >= 0)
        m_sortCombo->setCurrentIndex(i);
    m_orderButton->setArrowType(order == Qt::AscendingOrder ? Qt::UpArrow : Qt::DownArrow);
}

void ProjectManagerWindow::setSearchText(const QString& text)
{
    m_search->setText(text);
}

void ProjectManagerWindow::applyCardWidth(int width)
{
    m_delegate->setCardWidth(width);
    const QSize card = m_delegate->cardSize();
    m_cardView->setGridSize(card + QSize(kGridSpacing, kGridSpacing));
    // A new sizeHint(): the uniform item size is cached by the view until relayout.
    m_cardView->doItemsLayout();
}

QAbstractItemView* ProjectManagerWindow::activeView() const
{
    return isCardView() ? static_cast<QAbstractItemView*>(m_cardView) : m_listView;
}

QString ProjectManagerWindow::currentPath() const
{
    QAbstractItemView* v = activeView();
    QModelIndex i = v->currentIndex();
    if (!i.isValid() || !v->selectionModel()->isSelected(i)) {
        const QModelIndexList rows = selectedRowIndexes(v->selectionModel());
        i = rows.isEmpty() ? QModelIndex() : rows.first();
    }
    return i.isValid() ? i.data(ProjectLibraryModel::PathRole).toString() : QString();
}

QStringList ProjectManagerWindow::selectedPaths() const
{
    QModelIndexList rows
        = selectedRowIndexes(activeView()->selectionModel(), ProjectLibraryModel::NameColumn);
    std::sort(rows.begin(), rows.end(),
        [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
    QStringList paths;
    for (const QModelIndex& i : rows)
        paths << i.data(ProjectLibraryModel::PathRole).toString();
    return paths;
}

bool ProjectManagerWindow::selectPath(const QString& path)
{
    const int row = m_model->rowOf(QDir::cleanPath(path));
    if (row < 0)
        return false;
    const QModelIndex i = m_proxy->mapFromSource(m_model->index(row, 0));
    if (!i.isValid())
        return false; // filtered out
    for (QAbstractItemView* v : {static_cast<QAbstractItemView*>(m_cardView),
             static_cast<QAbstractItemView*>(m_listView)}) {
        v->selectionModel()->setCurrentIndex(
            i, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        v->scrollTo(i);
    }
    return true;
}

void ProjectManagerWindow::updateActions()
{
    const QStringList selected = selectedPaths();
    const QString current = currentPath();
    const bool project = !current.isEmpty() && QFileInfo(current).isFile();
    m_openButton->setEnabled(!current.isEmpty());
    m_exportButton->setEnabled(project && selected.size() == 1);
    const bool inLibrary = !m_model->isRecent();
    m_importButton->setEnabled(inLibrary);
    m_newFolderButton->setEnabled(inLibrary);
    int projects = 0;
    int folders = 0;
    for (int r = 0; r < m_proxy->rowCount(); ++r) {
        if (m_proxy->index(r, 0).data(ProjectLibraryModel::IsFolderRole).toBool())
            ++folders;
        else
            ++projects;
    }
    QString text = tr("%n project(s)", nullptr, projects);
    if (folders > 0)
        text += tr(", %n folder(s)", nullptr, folders);
    m_status->setText(text);
}

// ---- Operations ----

void ProjectManagerWindow::openPath(const QString& path)
{
    if (path.isEmpty())
        return;
    if (QFileInfo(path).isDir()) {
        if (m_model->isRecent())
            return;
        showFolder(path);
        return;
    }
    if (!QFileInfo::exists(path)) {
        reportError(
            tr("The project file %1 no longer exists.").arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (QDir::cleanPath(path) != m_library->openProjectPath() && m_guard && !m_guard(this))
        return;
    emit openRequested(QDir::cleanPath(path));
}

void ProjectManagerWindow::openSelected()
{
    openPath(currentPath());
}

QString ProjectManagerWindow::createProject(const QString& name, const QString& kindId)
{
    QString error;
    const QString folder = m_model->isRecent() ? m_library->root() : m_model->folder();
    // Ask before creating: a cancelled "save changes?" must not leave a new project behind.
    if (m_guard && !m_guard(this))
        return {};
    const QString path = m_library->createProject(
        folder, name, kindId.isEmpty() ? NewProjectDialog::preferredKindId() : kindId, &error);
    if (path.isEmpty()) {
        reportError(error);
        return {};
    }
    selectPath(path);
    emit openRequested(path);
    return path;
}

QString ProjectManagerWindow::createFolder(const QString& name)
{
    QString error;
    const QString path = m_library->createFolder(m_model->folder(), name, &error);
    if (path.isEmpty())
        reportError(error);
    else
        selectPath(path);
    return path;
}

void ProjectManagerWindow::newProject()
{
    const QString suggested = QFileInfo(ProjectLibrary::uniquePath(m_model->folder(),
                                            tr("Untitled Project"), QStringLiteral("kkproj")))
                                  .completeBaseName();
    NewProjectDialog dialog(suggested, this);
    if (dialog.exec() == QDialog::Accepted)
        createProject(dialog.name(), dialog.kindId());
}

void ProjectManagerWindow::newFolder()
{
    if (m_model->isRecent())
        return;
    bool ok = false;
    const QString suggested
        = QFileInfo(ProjectLibrary::uniquePath(m_model->folder(), tr("New Folder"), {})).fileName();
    const QString name = QInputDialog::getText(
        this, tr("New Folder"), tr("Folder name:"), QLineEdit::Normal, suggested, &ok);
    if (ok)
        createFolder(name);
}

void ProjectManagerWindow::renameSelected()
{
    QAbstractItemView* v = activeView();
    const QModelIndex i = v->currentIndex().siblingAtColumn(ProjectLibraryModel::NameColumn);
    if (!i.isValid())
        return;
    if (!(i.flags() & Qt::ItemIsEditable)) {
        const QString path = i.data(ProjectLibraryModel::PathRole).toString();
        if (m_library->isOpenOrContainsOpen(path))
            reportError(tr("The open project cannot be renamed. Open another project first."));
        return;
    }
    v->edit(i);
}

void ProjectManagerWindow::duplicateSelected()
{
    QString last;
    for (const QString& p : selectedPaths()) {
        if (!QFileInfo(p).isFile())
            continue;
        // A single-file project holds its media: the copy runs on a worker, with Cancel.
        QString error;
        QString copy;
        bool cancelled = false;
        ProjectLibrary* library = m_library;
        runWithProgress(this, tr("Duplicate"), tr("Copying %1…").arg(QFileInfo(p).fileName()),
            [&](const TaskProgress& progress) {
                copy = library->duplicate(p, &error, progress, &cancelled);
            });
        if (cancelled)
            break;
        if (copy.isEmpty())
            reportError(error);
        else
            last = copy;
    }
    if (!last.isEmpty())
        selectPath(last);
}

bool ProjectManagerWindow::trash(const QStringList& paths, bool ask)
{
    if (paths.isEmpty())
        return false;
    for (const QString& p : paths) {
        if (m_library->isOpenOrContainsOpen(p)) {
            reportError(QFileInfo(p).isDir()
                    ? tr("“%1” contains the open project and cannot be deleted.")
                          .arg(QFileInfo(p).fileName())
                    : tr("“%1” is open and cannot be deleted. Open another project first.")
                          .arg(QFileInfo(p).completeBaseName()));
            return false;
        }
    }
    if (ask) {
        const QFileInfo first(paths.first());
        const QString what = paths.size() == 1
            ? (first.isDir() ? tr("the folder “%1” and everything in it").arg(first.fileName())
                             : tr("the project “%1”").arg(first.completeBaseName()))
            : tr("%n item(s)", nullptr, int(paths.size()));
        const auto answer
            = QMessageBox::question(this, tr("Delete"), tr("Move %1 to the trash?").arg(what),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes)
            return false;
    }
    bool all = true;
    for (const QString& p : paths) {
        QString error;
        if (!m_library->moveToTrash(p, &error)) {
            reportError(error);
            all = false;
        }
    }
    if (m_model->isRecent())
        m_model->refresh();
    return all;
}

void ProjectManagerWindow::deleteSelected()
{
    trash(selectedPaths(), true);
}

void ProjectManagerWindow::showSelectedInFileManager()
{
    const QString path = currentPath();
    platform::showInFileManager(path.isEmpty() ? m_model->folder() : path);
}

void ProjectManagerWindow::importProject()
{
    if (m_model->isRecent())
        showFolder();
    const QStringList files = platform::getOpenFileNames(
        this, tr("Import Project"), projectFilter(), QLatin1String(kLastDirKey));
    if (!files.isEmpty())
        m_model->dropPaths(files, m_model->folder()); // the same path as a drop from outside
}

void ProjectManagerWindow::exportSelected()
{
    exportProject(false);
}

void ProjectManagerWindow::exportAsSingleFile()
{
    exportProject(true);
}

void ProjectManagerWindow::exportProject(bool singleFile)
{
    const QString path = currentPath();
    if (path.isEmpty() || !QFileInfo(path).isFile())
        return;
    // Both kinds are offered: another kind than the project's converts it (Pack / Unpack).
    const bool fromSingle = pack::isContainerFile(path);
    const bool single = singleFile || fromSingle;
    const QString singleFilter = tr("Single-file project with its media (*.%1)")
                                     .arg(QLatin1String(pack::kContainerSuffix));
    const QString linkedFilter = tr("Linked project (*.kkproj)");
    const QString filter = single ? singleFilter + QStringLiteral(";;") + linkedFilter
                                  : linkedFilter + QStringLiteral(";;") + singleFilter;
    const QString suffix
        = single ? QLatin1String(pack::kContainerSuffix) : QStringLiteral("kkproj");
    QString target = platform::getSaveFileName(this,
        singleFile ? tr("Export as Single File") : tr("Export Project"), filter,
        QLatin1String(kLastDirKey), QFileInfo(path).completeBaseName() + QLatin1Char('.') + suffix);
    if (target.isEmpty())
        return;
    if (QFileInfo(target).suffix().isEmpty())
        target += QLatin1Char('.') + suffix;
    QString error;
    bool cancelled = false;
    QStringList linked;
    bool ok = false;
    if (fromSingle == pack::isContainerFile(target)) {
        ok = m_library->exportProject(path, target, &error);
    } else {
        // Pack / Unpack copy media: on a worker, with progress and Cancel.
        const ProjectLibrary* library = m_library;
        runWithProgress(this, singleFile ? tr("Export as Single File") : tr("Export Project"),
            tr("Exporting %1…").arg(QFileInfo(path).fileName()), [&](const TaskProgress& progress) {
                ok = library->exportProject(path, target, &error, progress, &cancelled, &linked);
            });
    }
    if (cancelled)
        return;
    if (!ok) {
        reportError(error);
        return;
    }
    if (!linked.isEmpty()) {
        QMessageBox::information(this, tr("Export Project"),
            tr("%n media file(s) stay linked (not stored in the exported file):", nullptr,
                int(linked.size()))
                + QLatin1Char('\n') + linked.mid(0, 12).join(QLatin1Char('\n')));
    }
}

void ProjectManagerWindow::reportError(const QString& message)
{
    if (!message.isEmpty())
        QMessageBox::warning(this, tr("Project Manager"), message);
}

// ---- Context menu and keys ----

void ProjectManagerWindow::showContextMenu(QAbstractItemView* view, const QPoint& pos)
{
    const QModelIndex at = view->indexAt(pos);
    if (at.isValid() && !view->selectionModel()->isSelected(at))
        view->selectionModel()->setCurrentIndex(
            at, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QMenu menu(this);
    if (at.isValid()) {
        const QString path = at.data(ProjectLibraryModel::PathRole).toString();
        const bool folder = at.data(ProjectLibraryModel::IsFolderRole).toBool();
        const bool exists = at.data(ProjectLibraryModel::ExistsRole).toBool();
        const bool open = path == m_library->openProjectPath();
        menu.addAction(tr("&Open"), this, &ProjectManagerWindow::openSelected)->setEnabled(exists);
        menu.addSeparator();
        QAction* rename
            = menu.addAction(tr("&Rename"), this, &ProjectManagerWindow::renameSelected);
        rename->setShortcut(QKeySequence(Qt::Key_F2));
        rename->setEnabled(exists && !m_library->isOpenOrContainsOpen(path));
        if (!folder)
            menu.addAction(tr("D&uplicate"), this, &ProjectManagerWindow::duplicateSelected)
                ->setEnabled(exists);
        QAction* del
            = menu.addAction(tr("&Delete..."), this, &ProjectManagerWindow::deleteSelected);
        del->setShortcut(QKeySequence(Qt::Key_Delete));
        del->setEnabled(exists && !m_library->isOpenOrContainsOpen(path));
        menu.addSeparator();
        if (!folder) {
            menu.addAction(tr("&Export..."), this, &ProjectManagerWindow::exportSelected)
                ->setEnabled(exists);
            if (!pack::isContainerFile(path)) {
                menu.addAction(tr("Export as Single &File..."), this,
                        &ProjectManagerWindow::exportAsSingleFile)
                    ->setEnabled(exists);
            }
            QAction* settings = menu.addAction(
                tr("Project &Settings..."), this, &ProjectManagerWindow::projectSettingsRequested);
            settings->setEnabled(open);
            if (!open)
                settings->setToolTip(tr("Open the project to change its settings"));
        }
        menu.addAction(
                tr("Show in &File Manager"), this, &ProjectManagerWindow::showSelectedInFileManager)
            ->setEnabled(exists);
    } else {
        view->clearSelection();
        menu.addAction(tr("New &Project..."), this, &ProjectManagerWindow::newProject);
        menu.addAction(tr("New F&older..."), this, &ProjectManagerWindow::newFolder)
            ->setEnabled(!m_model->isRecent());
        menu.addAction(tr("&Import..."), this, &ProjectManagerWindow::importProject);
        menu.addSeparator();
        menu.addAction(
            tr("Show in &File Manager"), this, &ProjectManagerWindow::showSelectedInFileManager);
    }
    menu.exec(view->viewport()->mapToGlobal(pos));
}

bool ProjectManagerWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress && (watched == m_cardView || watched == m_listView)) {
        auto* key = static_cast<QKeyEvent*>(event);
        auto* view = static_cast<QAbstractItemView*>(watched);
        if (view->focusWidget() && view->focusWidget() != view)
            return false; // the inline editor gets Enter / Delete
        if (key->modifiers() & ~(Qt::KeypadModifier))
            return false;
        switch (key->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            openSelected();
            return true;
        case Qt::Key_Delete:
            deleteSelected();
            return true;
        case Qt::Key_F2:
            renameSelected();
            return true;
        case Qt::Key_Backspace:
            goUp();
            return true;
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void ProjectManagerWindow::closeEvent(QCloseEvent* event)
{
    saveSettings();
    QDialog::closeEvent(event);
}

// ---- Settings ----

void ProjectManagerWindow::saveSettings() const
{
    QSettings s;
    s.beginGroup(QLatin1String(kSettingsGroup));
    s.setValue(QStringLiteral("geometry"), saveGeometry());
    s.setValue(
        QStringLiteral("view"), isCardView() ? QStringLiteral("cards") : QStringLiteral("list"));
    s.setValue(QStringLiteral("cardWidth"), m_delegate->cardWidth());
    s.setValue(QStringLiteral("sortColumn"), m_proxy->sortColumn());
    s.setValue(QStringLiteral("descending"), m_proxy->sortOrder() == Qt::DescendingOrder);
    s.setValue(QStringLiteral("listHeader"), m_listView->header()->saveState());
}

void ProjectManagerWindow::restoreSettings()
{
    QSettings s;
    s.beginGroup(QLatin1String(kSettingsGroup));
    if (!restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray()))
        resize(kDefaultSize);
    m_listView->header()->restoreState(s.value(QStringLiteral("listHeader")).toByteArray());
    const int width
        = s.value(QStringLiteral("cardWidth"), ProjectCardDelegate::kDefaultWidth).toInt();
    m_sizeSlider->setValue(width);
    applyCardWidth(width);
    setCardView(s.value(QStringLiteral("view")).toString() != QLatin1String("list"));
    // Default: newest first, the project you worked on last at the top left.
    const int column
        = s.value(QStringLiteral("sortColumn"), int(ProjectLibraryModel::ModifiedColumn)).toInt();
    const bool descending = s.value(QStringLiteral("descending"), true).toBool();
    sortBy(column >= 0 ? column : int(ProjectLibraryModel::ModifiedColumn),
        descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}

} // namespace kk
