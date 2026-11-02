// SPDX-License-Identifier: GPL-3.0-or-later
// MainWindow implementation: window chrome, page switching, panel toggles, persistence.
#include "app/MainWindow.h"

#include "app/AppInfo.h"
#include "app/AutosaveController.h"
#include "app/InterfaceBar.h"
#include "app/PageBar.h"
#include "app/ProjectController.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "app/pages/MediaPage.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/StandardActions.h"
#include "ui/deliver/DeliverPage.h"
#include "ui/dialogs/DialogActions.h"
#include "ui/viewer/ViewerTransport.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStackedWidget>
#include <QUndoStack>
#include <QVBoxLayout>

namespace kk {

namespace {

constexpr char kGeometryKey[] = "mainWindow/geometry";
constexpr char kStateKey[] = "mainWindow/state";
constexpr char kPageKey[] = "mainWindow/page";
const QSize kMinimumWindowSize(1024, 640);

/// Interface-toolbar toggles: action id → panel.
struct PanelToggle
{
    const char* actionId;
    PanelId panel;
    bool leftSide;
};
constexpr PanelToggle kPanelToggles[] = {
    {"view.toggle_media_pool", PanelId::MediaPool, true},
    {"view.toggle_effects", PanelId::Effects, true},
    {"view.toggle_edit_index", PanelId::EditIndex, true},
    {"view.toggle_mixer", PanelId::Mixer, false},
    {"view.toggle_metadata", PanelId::Metadata, false},
    {"view.toggle_inspector", PanelId::Inspector, false},
};

QString pageSettingsGroup(const QString& pageId)
{
    return QStringLiteral("pages/") + pageId;
}

} // namespace

MainWindow::MainWindow(AppContext& ctx, const MainWindowOptions& options, QWidget* parent)
    : QMainWindow(parent)
    , m_ctx(ctx)
    , m_options(options)
{
    setMinimumSize(kMinimumWindowSize);
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));
    // Every registered action works as an application-wide shortcut, menu or not.
    addActions(ctx.actions->actions());

    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("CentralWidget"));
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_interfaceBar = new InterfaceBar(central);
    m_stack = new QStackedWidget(central);
    m_pageBar = new PageBar(central);
    layout->addWidget(m_interfaceBar);
    layout->addWidget(m_stack, 1);
    layout->addWidget(m_pageBar);
    setCentralWidget(central);

    m_projectController = new ProjectController(ctx, this);
    m_pageActions = new QActionGroup(this);
    m_pageActions->setExclusive(true);

    // ---- Pages (extension point: new pages are added here) ----
    m_mediaPage = new MediaPage(ctx);
    m_editPage = new EditPage(ctx);
    m_deliverPage = new DeliverPage(ctx);
    addPage(m_mediaPage, QStringLiteral("view.page_media"));
    addPage(m_editPage, QStringLiteral("view.page_edit"));
    // Color page is reserved: a disabled button between Edit and Deliver.
    QAction* colorAction = ctx.actions->action(QStringLiteral("view.page_color"));
    colorAction->setToolTip(tr("Color — coming later"));
    m_pageBar->addPageAction(colorAction);
    addPage(m_deliverPage, QStringLiteral("view.page_deliver"));

    buildMenus();
    buildInterfaceBar();
    connectAppActions();
    connectPanelToggles();
    // Playback-owned actions (transport.*, view.fullscreen_viewer) -> the active viewer.
    m_transport = new ViewerTransport(ctx, this);
    m_transport->connectActions();

    connect(ctx.project, &Project::nameChanged, this, &MainWindow::updateTitle);
    connect(ctx.project, &Project::dirtyChanged, this, &MainWindow::updateTitle);
    updateTitle();

    // The stack shows the first added page (Media) until told otherwise, so without a restored
    // layout the default page has to be selected explicitly.
    if (m_options.restoreLayout)
        restoreSettings();
    else
        setCurrentPage(QStringLiteral("edit"));
    if (currentPageId().isEmpty() || m_stack->currentWidget() == nullptr)
        setCurrentPage(QStringLiteral("edit"));
    if (m_options.autosave)
        m_autosave = new AutosaveController(ctx.project, this);
}

MainWindow::~MainWindow() = default;

void MainWindow::addPage(Page* page, const QString& actionId)
{
    QAction* action = m_ctx.actions->action(actionId);
    if (!action)
        return;
    action->setCheckable(true);
    m_pageActions->addAction(action);
    m_stack->addWidget(page);
    m_pages.append(PageEntry {page, action});
    m_pageBar->addPageAction(action);
    const QString id = page->pageId();
    connect(action, &QAction::triggered, this, [this, id] { setCurrentPage(id); });
    connect(page, &Page::panelVisibilityChanged, this, [this, page] {
        if (m_stack->currentWidget() == page)
            syncPanelToggles();
    });
}

bool MainWindow::setCurrentPage(const QString& pageId)
{
    for (const PageEntry& entry : std::as_const(m_pages)) {
        if (entry.page->pageId() != pageId)
            continue;
        m_stack->setCurrentWidget(entry.page);
        entry.action->setChecked(true);
        syncPanelToggles();
        return true;
    }
    return false;
}

QString MainWindow::currentPageId() const
{
    const auto* page = qobject_cast<const Page*>(m_stack->currentWidget());
    return page ? page->pageId() : QString();
}

Page* MainWindow::page(const QString& pageId) const
{
    for (const PageEntry& entry : m_pages) {
        if (entry.page->pageId() == pageId)
            return entry.page;
    }
    return nullptr;
}

void MainWindow::buildMenus()
{
    const ActionRegistry& registry = *m_ctx.actions;
    auto fillMenu = [&registry](QMenu* menu, const QString& category) {
        for (const QString& id : registry.menuEntries(category)) {
            if (id == QLatin1String(ActionRegistry::kSeparator))
                menu->addSeparator();
            else
                menu->addAction(registry.action(id));
        }
    };
    QStringList known;
    QMenu* helpMenu = nullptr;
    for (const MenuCategory& category : standardMenus()) {
        known.append(category.id);
        QMenu* menu = menuBar()->addMenu(category.title);
        fillMenu(menu, category.id);
        if (category.id == QLatin1String("help"))
            helpMenu = menu;
    }
    // Categories registered by other modules get their own menu before Help.
    for (const QString& category : registry.categories()) {
        if (known.contains(category))
            continue;
        QString title = category;
        title[0] = title[0].toUpper();
        QMenu* menu = new QMenu(title, menuBar());
        menuBar()->insertMenu(helpMenu ? helpMenu->menuAction() : nullptr, menu);
        fillMenu(menu, category);
    }
}

void MainWindow::buildInterfaceBar()
{
    for (const PanelToggle& toggle : kPanelToggles) {
        QAction* action = m_ctx.actions->action(QLatin1String(toggle.actionId));
        if (toggle.leftSide)
            m_interfaceBar->addLeftAction(action);
        else
            m_interfaceBar->addRightAction(action);
    }
}

void MainWindow::connectAppActions()
{
    ActionRegistry& actions = *m_ctx.actions;
    Project* project = m_ctx.project;
    QUndoStack* undoStack = project->undoStack();

    // ---- Undo / redo (through Project so they are refused mid-drag) ----
    QAction* undo = actions.action(QStringLiteral("edit.undo"));
    QAction* redo = actions.action(QStringLiteral("edit.redo"));
    connect(undo, &QAction::triggered, project, &Project::undo);
    connect(redo, &QAction::triggered, project, &Project::redo);
    auto updateUndo = [undo, redo, undoStack] {
        undo->setEnabled(undoStack->canUndo());
        redo->setEnabled(undoStack->canRedo());
        undo->setText(
            undoStack->canUndo() ? tr("&Undo %1").arg(undoStack->undoText()) : tr("&Undo"));
        redo->setText(
            undoStack->canRedo() ? tr("&Redo %1").arg(undoStack->redoText()) : tr("&Redo"));
    };
    connect(undoStack, &QUndoStack::indexChanged, this, updateUndo);
    connect(undoStack, &QUndoStack::cleanChanged, this, updateUndo);
    updateUndo();

    // ---- Window ----
    connect(
        actions.action(QStringLiteral("file.quit")), &QAction::triggered, this, &QWidget::close);
    QAction* fullScreen = actions.action(QStringLiteral("view.fullscreen"));
    connect(fullScreen, &QAction::triggered, this, [this](bool on) {
        if (on)
            showFullScreen();
        else
            showNormal();
    });
    QAction* singleViewer = actions.action(QStringLiteral("view.single_viewer"));
    connect(singleViewer, &QAction::triggered, this,
        [this](bool on) { m_editPage->setSingleViewer(on); });

    QAction* systemColors = actions.action(QStringLiteral("view.use_system_colors"));
    systemColors->setChecked(theme::useSystemColors());
    connect(
        systemColors, &QAction::triggered, this, [](bool on) { theme::setUseSystemColors(on); });

    // ---- Help ----
    connect(actions.action(QStringLiteral("help.about")), &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("About %1").arg(QLatin1String(appinfo::name)),
            tr("<h3>%1 %2</h3><p>A non-linear video editor built with Qt and FFmpeg.</p>"
               "<p>Licensed under the GNU GPL v3 or later.</p>")
                .arg(QLatin1String(appinfo::name), QLatin1String(appinfo::version)));
    });
    connect(actions.action(QStringLiteral("help.about_qt")), &QAction::triggered, qApp,
        &QApplication::aboutQt);

    // ---- Dialogs (src/ui/dialogs) ----
    connectDialogActions(m_ctx, this, [this] {
        if (m_autosave)
            m_autosave->setIntervalMinutes(prefs::autosaveMinutes());
    });
}

void MainWindow::connectPanelToggles()
{
    for (const PanelToggle& toggle : kPanelToggles) {
        QAction* action = m_ctx.actions->action(QLatin1String(toggle.actionId));
        const PanelId panel = toggle.panel;
        // triggered (not toggled): only user actions drive the page; syncPanelToggles() sets
        // the checked state programmatically without feedback loops.
        connect(action, &QAction::triggered, this, [this, panel](bool on) {
            if (auto* page = qobject_cast<Page*>(m_stack->currentWidget()))
                page->setPanelVisible(panel, on);
        });
    }
}

void MainWindow::syncPanelToggles()
{
    const auto* page = qobject_cast<const Page*>(m_stack->currentWidget());
    for (const PanelToggle& toggle : kPanelToggles) {
        QAction* action = m_ctx.actions->action(QLatin1String(toggle.actionId));
        const bool available = page && page->hasPanel(toggle.panel);
        action->setEnabled(available);
        action->setChecked(available && page->isPanelVisible(toggle.panel));
    }
    QAction* singleViewer = m_ctx.actions->action(QStringLiteral("view.single_viewer"));
    singleViewer->setEnabled(page == m_editPage);
    singleViewer->setChecked(m_editPage->isSingleViewer());
}

void MainWindow::updateTitle()
{
    const QString name = m_ctx.project->name();
    setWindowTitle(QStringLiteral("%1[*] — %2").arg(name, QLatin1String(appinfo::name)));
    setWindowModified(m_ctx.project->isDirty());
    m_interfaceBar->setTitle(m_ctx.project->isDirty() ? name + QStringLiteral(" •") : name);
}

void MainWindow::showRestored()
{
    if (m_geometryRestored)
        show();
    else
        showMaximized();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (!m_projectController->maybeSave()) {
        event->ignore();
        return;
    }
    if (m_options.saveLayout)
        saveSettings();
    event->accept();
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    // Keep View ▸ Full Screen checked state right when the window manager changes it (Esc, ...).
    if (event->type() == QEvent::WindowStateChange)
        m_ctx.actions->action(QStringLiteral("view.fullscreen"))->setChecked(isFullScreen());
}

void MainWindow::saveSettings() const
{
    QSettings settings;
    settings.setValue(QLatin1String(kGeometryKey), saveGeometry());
    settings.setValue(QLatin1String(kStateKey), saveState());
    settings.setValue(QLatin1String(kPageKey), currentPageId());
    for (const PageEntry& entry : m_pages) {
        settings.beginGroup(pageSettingsGroup(entry.page->pageId()));
        entry.page->saveLayout(settings);
        settings.endGroup();
    }
}

void MainWindow::restoreSettings()
{
    QSettings settings;
    m_geometryRestored = restoreGeometry(settings.value(QLatin1String(kGeometryKey)).toByteArray());
    restoreState(settings.value(QLatin1String(kStateKey)).toByteArray());
    for (const PageEntry& entry : std::as_const(m_pages)) {
        settings.beginGroup(pageSettingsGroup(entry.page->pageId()));
        entry.page->restoreLayout(settings);
        settings.endGroup();
    }
    setCurrentPage(settings.value(QLatin1String(kPageKey), QStringLiteral("edit")).toString());
}

} // namespace kk
