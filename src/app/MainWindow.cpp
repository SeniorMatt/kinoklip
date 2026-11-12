// SPDX-License-Identifier: GPL-3.0-or-later
// MainWindow implementation: window chrome, page switching, panel toggles, persistence.
#include "app/MainWindow.h"

#include "app/AppInfo.h"
#include "app/AutosaveController.h"
#include "app/BugReportDialog.h"
#include "app/InterfaceBar.h"
#include "app/LogExportDialog.h"
#include "app/PageBar.h"
#include "app/ProjectController.h"
#include "app/SettingsTransfer.h"
#include "app/TimelineExport.h"
#include "app/TimelineImport.h"
#include "app/VimController.h"
#include "app/commands/Command.h"
#include "app/live/LiveServer.h"
#include "app/pages/AudioPage.h"
#include "app/pages/ColorPage.h"
#include "app/pages/CompositingPage.h"
#include "app/pages/CutPage.h"
#include "app/pages/EditPage.h"
#include "app/pages/MediaPage.h"
#include "core/Logging.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "ui/audio/VoiceOverController.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/ListKeyGuard.h"
#include "ui/common/MediaManagement.h"
#include "ui/common/MenuLayouts.h"
#include "ui/common/PanelSearchFocus.h"
#include "ui/common/ResizeProfiler.h"
#include "ui/common/ShortcutFallback.h"
#include "ui/common/StandardActions.h"
#include "ui/deliver/DeliverPage.h"
#include "render/FontReload.h"
#include "ui/dialogs/DialogActions.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/help/Guide.h"
#include "ui/help/HelpController.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/scopes/ScopesWindow.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/viewer/ViewerOverlaySettings.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerTransport.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QHash>
#include <QCursor>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolTip>
#include <QUndoStack>
#include <QVBoxLayout>

#include <functional>

namespace kk {

namespace {

constexpr char kGeometryKey[] = "mainWindow/geometry";
constexpr char kStateKey[] = "mainWindow/state";
/// A dragged splitter is stored this long after the last move (not only on quitting).
constexpr int kLayoutSaveDelayMs = 1000;
constexpr char kPageKey[] = "mainWindow/page";
const QSize kMinimumWindowSize(1024, 640);

/// Interface-toolbar toggles: action id → panel.
struct PanelToggle
{
    const char* actionId;
    PanelId panel;
    bool leftSide;
    bool takesDrops; ///< the panel accepts dropped files / clips: a drag over its button opens it
};
constexpr PanelToggle kPanelToggles[] = {
    {"view.toggle_media_pool", PanelId::MediaPool, true, true},
    {"view.toggle_effects", PanelId::Effects, true, false},
    {"view.toggle_edit_index", PanelId::EditIndex, true, false},
    {"view.toggle_keyframes", PanelId::Keyframes, true, false}, // Resolve 20: top left
    {"view.toggle_gallery", PanelId::Gallery, true, false}, // Color page
    {"view.toggle_clips", PanelId::Clips, true, false}, // Color page
    {"view.toggle_color_timeline", PanelId::ColorTimeline, true, false}, // Color page
    {"view.toggle_nodes", PanelId::Nodes, false, true}, // Compositing and Color page node editors
    {"view.toggle_scopes", PanelId::Scopes, false, false}, // Color page
    {"view.toggle_mixer", PanelId::Mixer, false, false},
    {"view.toggle_metadata", PanelId::Metadata, false, false},
    {"view.toggle_inspector", PanelId::Inspector, false, true},
};

/// Page bar slots in Resolve's order: page id, or nullptr for a page that is not built yet.
struct PageSlot
{
    const char* actionId;
    const char* pageId;
};
constexpr PageSlot kPageSlots[] = {
    {"view.page_media", "media"},
    {"view.page_cut", "cut"},
    {"view.page_edit", "edit"},
    {"view.page_compositing", "compositing"},
    {"view.page_color", "color"},
    {"view.page_audio", "audio"},
    {"view.page_deliver", "deliver"},
};

QString pageSettingsGroup(const QString& pageId)
{
    return QStringLiteral("pages/") + pageId;
}

/// Closes an open menu when its title in the menu bar is clicked again, before QMenuBar does.
/// QMenuBar::mousePressEvent() calls QWidget::grabMouse() in that case (to receive the
/// release); Qt Wayland refuses explicit grabs outside popups and prints "This plugin supports
/// grabbing the mouse only for popup windows" for the grab and again for the release. The
/// implicit grab of the press already delivers the release, so nothing is lost.
class MenuBarGrabGuard final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() != QEvent::MouseButtonPress)
            return false;
        auto* bar = qobject_cast<QMenuBar*>(watched);
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (!bar || mouse->button() != Qt::LeftButton)
            return false;
        QAction* action = bar->actionAt(mouse->position().toPoint());
        QMenu* menu = action ? action->menu() : nullptr;
        if (!menu || action != bar->activeAction() || !menu->isVisible())
            return false;
        menu->hide();
        return true;
    }
};

} // namespace

MainWindow::MainWindow(AppContext& ctx, const MainWindowOptions& options, QWidget* parent)
    : QMainWindow(parent)
    , m_ctx(ctx)
    , m_options(options)
{
    setMinimumSize(kMinimumWindowSize);
    // The owner's pixel-art icon (resources/icons/app), one file per size so none is rescaled.
    QIcon appIcon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256, 512}) {
        appIcon.addFile(QStringLiteral(":/icons/app/kinoklip-%1.png").arg(size), QSize(size, size));
    }
    setWindowIcon(appIcon);
    // Every registered action works as an application-wide shortcut, menu or not.
    addActions(ctx.actions->actions());
    // Ctrl+Shift+[ etc. also when the layout reports Ctrl+Shift+{ (alpha.2 feedback).
    new ShortcutFallback(*ctx.actions, this);
    // A focused list or tree panel keeps Delete, Ctrl+A and the arrow keys (spec 10.11).
    new ListKeyGuard(this);
    // Per-step live-resize timings with QT_LOGGING_RULES=kk.ui.resize.debug=true.
    ResizeProfiler::attach(this);

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
    if (ctx.voiceOver) {
        // Voice-over (spec 75): what the user must be told (a refused arm or Record, a failed
        // take). The result of a take is the clip on the track.
        connect(
            ctx.voiceOver, &VoiceOverController::message, this, [this](const QString& text, bool) {
                QMessageBox::warning(this, tr("Voice-Over"), text);
            });
    }
    connectTimelineExportActions(ctx, this); // File > Export (EDL, OTIO, marker lists)
    connectTimelineImportActions(ctx, this); // File > Import > Timeline (OTIO)
    m_pageActions = new QActionGroup(this);
    m_pageActions->setExclusive(true);

    // Panel sizes are stored shortly after a splitter drag, so a crash does not lose them.
    m_layoutSaveTimer = new QTimer(this);
    m_layoutSaveTimer->setSingleShot(true);
    m_layoutSaveTimer->setInterval(kLayoutSaveDelayMs);
    connect(m_layoutSaveTimer, &QTimer::timeout, this, [this] {
        if (m_options.saveLayout)
            saveSettings();
    });

    // ---- Pages (extension point: new pages are added here) ----
    m_mediaPage = new MediaPage(ctx);
    m_editPage = new EditPage(ctx);
    // The Cut page edits with the Edit page's timeline controller (one tool / target state).
    m_cutPage = new CutPage(ctx, m_editPage->timeline()->controller());
    // The Audio page too: one tool / target state for every timeline view.
    m_audioPage = new AudioPage(ctx, m_editPage->timeline()->controller());
    m_compositingPage = new CompositingPage(ctx);
    m_colorPage = new ColorPage(ctx);
    m_deliverPage = new DeliverPage(ctx);
    // The page bar in Resolve's order; pages not built yet are disabled placeholders.
    const QHash<QString, Page*> built {{QStringLiteral("media"), m_mediaPage},
        {QStringLiteral("cut"), m_cutPage}, {QStringLiteral("edit"), m_editPage},
        {QStringLiteral("compositing"), m_compositingPage}, {QStringLiteral("color"), m_colorPage},
        {QStringLiteral("audio"), m_audioPage}, {QStringLiteral("deliver"), m_deliverPage}};
    for (const PageSlot& slot : kPageSlots) {
        const QString actionId = QLatin1String(slot.actionId);
        Page* page = slot.pageId ? built.value(QLatin1String(slot.pageId)) : nullptr;
        if (page) {
            addPage(page, actionId);
            continue;
        }
        QAction* placeholder = ctx.actions->action(actionId);
        placeholder->setToolTip(
            tr("%1 page — coming in a later version").arg(placeholder->iconText()));
        m_pageBar->addPageAction(placeholder);
    }

    describeStandardActions(*ctx.actions); // status tips; the menu reference reads them
    m_menuLayout = menulayout::layout();
    buildMenus();
    // The page menu of the Streamlined layout follows the page shown.
    connect(m_stack, &QStackedWidget::currentChanged, this, &MainWindow::updatePageMenu);
    // Preferences > UI Settings > Menu layout: the menu bar is rebuilt at once.
    connect(&menulayout::notifier(), &menulayout::Notifier::changed, this,
        [this] { setMenuLayout(menulayout::layout()); });
    // A rebound key may take or free a menu title's Alt+letter.
    connect(ctx.actions, &ActionRegistry::shortcutsChanged, this, &MainWindow::updateMenuMnemonics);
    connect(
        ctx.actions, &ActionRegistry::activeProfileChanged, this, &MainWindow::updateMenuMnemonics);
    buildInterfaceBar();
    connectAppActions();
    connectPanelToggles();
    // Playback-owned actions (transport.*, view.fullscreen_viewer) -> the active viewer.
    m_transport = new ViewerTransport(ctx, this);
    m_transport->connectActions();
    // In-app help (src/ui/help): F1, Help > Guide, the "?" buttons of the panels.
    m_help = new HelpController(*ctx.actions, this, [this] { return currentPageId(); }, this);

    connect(ctx.project, &Project::nameChanged, this, &MainWindow::updateTitle);
    connect(ctx.project, &Project::dirtyChanged, this, &MainWindow::updateTitle);
    connect(
        m_projectController, &ProjectController::storageChanged, this, &MainWindow::updateTitle);
    updateTitle();

    // The stack shows the first added page (Media) until told otherwise, so without a restored
    // layout the default page has to be selected explicitly.
    if (m_options.restoreLayout)
        restoreSettings();
    else
        setCurrentPage(QStringLiteral("edit"));
    if (currentPageId().isEmpty() || m_stack->currentWidget() == nullptr)
        setCurrentPage(QStringLiteral("edit"));
    if (m_options.autosave) {
        m_autosave = new AutosaveController(ctx.project, this);
        m_projectController->setAutosave(m_autosave);
    }

    // Local control (Preferences > Local Control): commands from `kinoklip mcp` run on this
    // window's project, one undo step per call. Listens only while the preference is on.
    commands::LiveTarget target;
    target.project = ctx.project;
    target.save = [this](QString* error) {
        if (m_projectController->save())
            return true;
        if (error)
            *error = tr("the window could not save the project (see the message it showed)");
        return false;
    };
    m_live = new live::LiveServer(commands::CommandRegistry::builtin(), target, this);
    connect(m_live, &live::LiveServer::clientCountChanged, m_interfaceBar,
        &InterfaceBar::setRemoteControlClients);
    applyLocalControlPreference();
    // Vim mode (Kinoklip menu > Vim Mode, off by default): the keys, the mode line, the `:` line.
    m_vim = new VimController(ctx, m_editPage->timeline(), this, m_pageBar, target, this);
    // A settings import may have switched Local Control off.
    connect(settingstransfer::notifier(), &settingstransfer::Notifier::applied, this,
        &MainWindow::applyLocalControlPreference, Qt::QueuedConnection);
}

void MainWindow::applyLocalControlPreference()
{
    if (!prefs::localControlEnabled()) {
        m_live->close();
        return;
    }
    QString error;
    if (!m_live->listen(&error))
        qCWarning(lcCore).noquote() << "local control is on but not available:" << error;
}

MainWindow::~MainWindow() = default;

BugReportDialog* MainWindow::showBugReportDialog()
{
    if (!m_bugReport) {
        m_bugReport = new BugReportDialog(bugreport::collect(), this);
        m_bugReport->setAttribute(Qt::WA_DeleteOnClose);
    }
    m_bugReport->show();
    m_bugReport->raise();
    m_bugReport->activateWindow();
    return m_bugReport;
}

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
    // The layout as built is what Workspace > Reset UI Layout returns to.
    page->captureDefaultLayout();
    for (QSplitter* splitter : page->findChildren<QSplitter*>())
        connect(
            splitter, &QSplitter::splitterMoved, m_layoutSaveTimer, qOverload<>(&QTimer::start));
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
        // One media pool, as in Resolve: the page shown next shows the same bin.
        auto* from = m_stack->currentWidget()
            ? m_stack->currentWidget()->findChild<MediaPoolPanel*>()
            : nullptr;
        if (auto* to = entry.page->findChild<MediaPoolPanel*>(); from && to && from != to)
            to->setCurrentBinKey(from->currentBinKey());
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

void MainWindow::discardMenus()
{
    // The menus go later (this can run from an action of one of them, e.g. the Preferences
    // dialog); until then they are out of the bar and nameless, so nothing finds them.
    const auto old = menuBar()->findChildren<QMenu*>(Qt::FindDirectChildrenOnly);
    for (QMenu* menu : old) {
        menuBar()->removeAction(menu->menuAction());
        menu->hide();
        menu->setObjectName(QString());
        const auto inner = menu->findChildren<QObject*>();
        for (QObject* child : inner)
            child->setObjectName(QString());
        menu->deleteLater();
    }
    m_pageMenu = nullptr;
}

void MainWindow::setMenuLayout(menulayout::Layout layout)
{
    if (layout == m_menuLayout && !menuBar()->actions().isEmpty())
        return;
    m_menuLayout = layout;
    buildMenus();
}

void MainWindow::buildMenus()
{
    discardMenus();
    // Menus are data (docs/archive/plan.md 5.9): each category lists action ids, separators and
    // submenu markers; nothing here knows what a menu contains.
    const ActionRegistry& registry = *m_ctx.actions;
    if (m_menuLayout == menulayout::Layout::Streamlined) {
        // The second table (src/ui/common/MenuLayouts.cpp): the same actions, arranged by task.
        for (const menulayout::Menu& spec : menulayout::streamlinedMenus()) {
            const QString title = spec.id == QLatin1String("app")
                ? QCoreApplication::applicationName()
                : spec.title;
            QMenu* menu = menuBar()->addMenu(title);
            menu->setObjectName(QStringLiteral("menu:") + spec.id);
            menu->menuAction()->setProperty("kkTitle", title);
            menulayout::fill(menu, spec.entries, *m_ctx.actions, menu->objectName());
        }
        updatePageMenu();
    } else {
        std::function<void(QMenu*, const QString&)> fillMenu;
        fillMenu = [&registry, &fillMenu](QMenu* menu, const QString& category) {
            for (const QString& id : registry.menuEntries(category)) {
                if (id == QLatin1String(ActionRegistry::kSeparator)) {
                    menu->addSeparator();
                } else if (const QString sub = ActionRegistry::submenuOf(id); !sub.isEmpty()) {
                    QMenu* submenu = menu->addMenu(registry.submenuTitle(sub));
                    submenu->setObjectName(QStringLiteral("menu:") + sub);
                    fillMenu(submenu, sub);
                } else {
                    menu->addAction(registry.action(id));
                }
            }
        };
        QStringList known;
        QMenu* helpMenu = nullptr;
        for (const MenuCategory& category : standardMenus()) {
            known.append(category.id);
            QMenu* menu = menuBar()->addMenu(category.title);
            menu->setObjectName(QStringLiteral("menu:") + category.id);
            menu->menuAction()->setProperty("kkTitle", category.title);
            fillMenu(menu, category.id);
            if (category.id == QLatin1String("help"))
                helpMenu = menu;
        }
        // Categories registered by other modules get their own menu before Help (submenu
        // categories are already shown inside their parent menu).
        for (const QString& category : registry.categories()) {
            if (known.contains(category) || registry.isSubmenu(category))
                continue;
            QString title = category;
            title[0] = title[0].toUpper();
            QMenu* menu = new QMenu(title, menuBar());
            menu->setObjectName(QStringLiteral("menu:") + category);
            menuBar()->insertMenu(helpMenu ? helpMenu->menuAction() : nullptr, menu);
            fillMenu(menu, category);
        }
    }
    if (!menuBar()->findChild<QObject*>(QStringLiteral("MenuBarGrabGuard"))) {
        auto* guard = new MenuBarGrabGuard(menuBar());
        guard->setObjectName(QStringLiteral("MenuBarGrabGuard"));
        menuBar()->installEventFilter(guard);
    }
    updateMenuMnemonics();
    // Submenus whose entries depend on this computer or the user's files: the menu reference
    // (MenuReference.h) lists each as one row instead of its entries.
    const struct
    {
        const char* menu;
        const char* what;
    } dynamicMenus[] = {
        {"menu:file.recent",
            QT_TR_NOOP("Lists the projects opened lately, newest first; choosing one opens it.")},
        {"menu:view.guide_presets",
            QT_TR_NOOP("Lists the built-in and saved guide presets; choosing one applies it to "
                       "the viewer.")},
        {"menu:view.guide_presets_delete",
            QT_TR_NOOP("Lists the guide presets you saved; choosing one deletes it.")},
        {"menu:settings.color_scheme",
            QT_TR_NOOP("Lists the colour schemes found on this computer; choosing one changes the "
                       "colours of the whole window.")},
        {"menu:settings.style",
            QT_TR_NOOP("Lists the widget styles found on this computer; choosing one changes how "
                       "buttons and panels are drawn.")},
    };
    for (const auto& entry : dynamicMenus) {
        if (auto* menu = menuBar()->findChild<QMenu*>(QLatin1String(entry.menu))) {
            menu->setProperty("kkDynamicMenu", true);
            menu->menuAction()->setStatusTip(tr(entry.what));
        }
    }
    // File > Open Recent lists files, not registered commands: filled by ProjectController.
    if (auto* recent = menuBar()->findChild<QMenu*>(QStringLiteral("menu:file.recent")))
        m_projectController->attachRecentMenu(recent);
    // View > Guides > Load / Delete Guide Preset list preset files, not registered commands.
    ViewerOverlaySettings::instance().attachPresetMenus(
        menuBar()->findChild<QMenu*>(QStringLiteral("menu:view.guide_presets")),
        menuBar()->findChild<QMenu*>(QStringLiteral("menu:view.guide_presets_delete")));
}

void MainWindow::updatePageMenu()
{
    if (m_menuLayout != menulayout::Layout::Streamlined)
        return;
    if (m_pageMenu) {
        menuBar()->removeAction(m_pageMenu->menuAction());
        m_pageMenu->hide();
        m_pageMenu->setObjectName(QString());
        const auto inner = m_pageMenu->findChildren<QObject*>();
        for (QObject* child : inner)
            child->setObjectName(QString());
        m_pageMenu->deleteLater();
        m_pageMenu = nullptr;
    }
    const menulayout::Menu spec = menulayout::pageMenu(currentPageId());
    if (spec.id.isEmpty())
        return;
    // Before Help; the title follows the page (Cut, Nodes, Grade).
    QAction* before = nullptr;
    if (const QMenu* help = menuBar()->findChild<QMenu*>(QStringLiteral("menu:help")))
        before = help->menuAction();
    auto* menu = new QMenu(spec.title, menuBar());
    menu->setObjectName(QStringLiteral("menu:") + spec.id);
    menu->menuAction()->setProperty("kkTitle", spec.title);
    menulayout::fill(menu, spec.entries, *m_ctx.actions, menu->objectName());
    menuBar()->insertMenu(before, menu);
    m_pageMenu = menu;
    updateMenuMnemonics();
}

void MainWindow::updateMenuMnemonics()
{
    for (QAction* item : menuBar()->actions()) {
        const QString title = item->property("kkTitle").toString();
        const qsizetype amp = title.indexOf(QLatin1Char('&'));
        if (amp < 0 || amp + 1 >= title.size())
            continue;
        const QKeySequence alt(QKeyCombination(
            Qt::AltModifier, Qt::Key(QKeySequence(title.mid(amp + 1, 1).toUpper())[0].key())));
        const bool taken = !m_ctx.actions->actionForShortcut(alt).isEmpty();
        item->setText(taken ? QString(title).remove(amp, 1) : title);
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
        // A drag resting on the button of a panel that takes drops opens the panel (spec 74).
        if (toggle.takesDrops)
            m_interfaceBar->enableSpringLoad(action);
    }
}

void MainWindow::connectAppActions()
{
    ActionRegistry& actions = *m_ctx.actions;
    Project* project = m_ctx.project;
    QUndoStack* undoStack = project->undoStack();

    // ---- View > Video Scopes: the floating scopes window (any page) ----
    if (QAction* scopes = actions.action(QStringLiteral("view.video_scopes"))) {
        connect(scopes, &QAction::toggled, this, [this, scopes](bool on) {
            if (!m_scopesWindow) {
                if (!on)
                    return;
                m_scopesWindow = new ScopesWindow(m_ctx.playback, this);
                connect(m_scopesWindow, &ScopesWindow::closed, scopes,
                    [scopes] { scopes->setChecked(false); });
            }
            m_scopesWindow->setVisible(on);
        });
    }

    // ---- Workspace > Reset UI Layout: every page back to its default panels and sizes ----
    connect(actions.action(QStringLiteral("view.reset_ui_layout")), &QAction::triggered, this,
        &MainWindow::resetUiLayout);

    // ---- Undo / redo (through Project so they are refused mid-drag) ----
    QAction* undo = actions.action(QStringLiteral("edit.undo"));
    QAction* redo = actions.action(QStringLiteral("edit.redo"));
    // An edit left open by a gesture whose mouse release never arrived would refuse every
    // undo for the rest of the session (owner report on 0.4.0: "Ctrl+Z doesn't work"). With
    // no mouse button down no gesture can be running: close it first, and say which it was.
    auto closeStale = [project] {
        if (project->isEditing() && QGuiApplication::mouseButtons() == Qt::NoButton)
            qCWarning(lcCore).noquote() << "closed an edit left open:" << project->closeStaleEdit();
    };
    connect(undo, &QAction::triggered, project, [project, closeStale] {
        closeStale();
        project->undo();
    });
    connect(redo, &QAction::triggered, project, [project, closeStale] {
        closeStale();
        project->redo();
    });
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
    connect(actions.action(QStringLiteral("view.viewer_zoom_fit")), &QAction::triggered, this,
        [] { ViewerPanel::zoomTargetToFit(); });
    QAction* singleViewer = actions.action(QStringLiteral("view.single_viewer"));
    connect(singleViewer, &QAction::triggered, this,
        [this](bool on) { m_editPage->setSingleViewer(on); });
    // The page also leaves / enters the mode itself (viewer header button, restored layout).
    connect(m_editPage, &EditPage::singleViewerChanged, singleViewer, &QAction::setChecked);

    // ---- Compositing page: Clip > Open in Compositing Page (the selected clip) ----
    connect(actions.action(QStringLiteral("clip.open_in_compositing")), &QAction::triggered, this,
        [this] {
            if (currentPageId() == QLatin1String("compositing"))
                m_compositingPage->openClip();
            else
                setCurrentPage(QStringLiteral("compositing"));
        });

    // ---- Help ----
    connect(actions.action(QStringLiteral("help.about")), &QAction::triggered, this, [this] {
        const QString title = tr("About %1").arg(QLatin1String(appinfo::name));
        const QString text
            = tr("<h3>%1 %2</h3><p>A non-linear video editor built with Qt and FFmpeg.</p>"
                 "<p>Licensed under the GNU GPL v3 or later.</p>")
                  .arg(QLatin1String(appinfo::name), appinfo::displayVersion());
        if (m_menuLayout != menulayout::Layout::Streamlined) {
            QMessageBox::about(this, title, text);
            return;
        }
        // Streamlined: Help has no "About Qt" entry; the box offers it (spec 82).
        QMessageBox box(this);
        box.setWindowTitle(title);
        box.setText(text);
        box.setIconPixmap(windowIcon().pixmap(64, 64));
        QPushButton* aboutQt = box.addButton(tr("About Qt"), QMessageBox::ActionRole);
        box.addButton(QMessageBox::Ok);
        box.exec();
        if (box.clickedButton() == aboutQt)
            QApplication::aboutQt();
    });
    connect(actions.action(QStringLiteral("help.about_qt")), &QAction::triggered, qApp,
        &QApplication::aboutQt);
    // Help > Report a Bug or Inconsistency: the user sees what would be shared and decides.
    connect(actions.action(QStringLiteral("help.report_bug")), &QAction::triggered, this,
        [this] { showBugReportDialog(); });
    // Help > Export Log File...: says what the file holds, shows it, saves it where you choose.
    connect(actions.action(QStringLiteral("help.export_log")), &QAction::triggered, this,
        [this] { LogExportDialog::run(this); });
    // Help > Join the Community on Discord: the invite opens in the browser.
    connect(actions.action(QStringLiteral("help.community")), &QAction::triggered, this,
        [this] { guide::openUrl(QUrl(QLatin1String(appinfo::communityUrl)), this); });

    // ---- Fonts installed while the program runs: watched folders + Kinoklip > Reload Fonts ----
    {
        auto* fontWatcher = new render::fonts::FontWatcher(render::fonts::defaultFontDirs(), this);
        connect(fontWatcher, &render::fonts::FontWatcher::fontsAdded, this, [this] {
            if (m_ctx.playback)
                m_ctx.playback->redrawStill(); // titles in a font that was missing show it now
        });
        connect(actions.action(QStringLiteral("settings.reload_fonts")), &QAction::triggered, this,
            [this, fontWatcher] {
                const QStringList added = fontWatcher->reload();
                QToolTip::showText(QCursor::pos(),
                    added.isEmpty() ? tr("No new fonts found.")
                                    : tr("Added fonts: %1").arg(added.join(QStringLiteral(", "))),
                    this);
            });
    }

    // ---- Dialogs (src/ui/dialogs) ----
    connectDialogActions(m_ctx, this, [this] {
        if (m_autosave)
            m_autosave->setIntervalMinutes(prefs::autosaveMinutes());
        applyRuntimePreferences(m_ctx.playback);
        applyLocalControlPreference();
        if (m_ctx.media) // cache folder may have moved (Preferences > Media Storage)
            m_ctx.media->preferencesChanged();
        m_editPage->timeline()->applyPreferences(); // timeline tabs on / off
        m_editPage->mediaPool()->applyPreferences(); // default smart bins on / off
        if (m_mediaPage)
            m_mediaPage->mediaPool()->applyPreferences();
        if (m_cutPage)
            m_cutPage->mediaPool()->applyPreferences();
        if (m_compositingPage)
            m_compositingPage->mediaPool()->applyPreferences();
    });
    applyRuntimePreferences(m_ctx.playback); // stored choices from start-up on
}

void MainWindow::connectPanelToggles()
{
    // Opened from the keyboard, a panel with a search field can take the focus there
    // (Preferences > Keyboard, off by default; spec 49.5).
    auto* shortcutWatch = new ShortcutTriggerWatch(this);
    for (const PanelToggle& toggle : kPanelToggles) {
        QAction* action = m_ctx.actions->action(QLatin1String(toggle.actionId));
        const PanelId panel = toggle.panel;
        shortcutWatch->watch(action);
        // triggered (not toggled): only user actions drive the page; syncPanelToggles() sets
        // the checked state programmatically without feedback loops.
        connect(action, &QAction::triggered, this, [this, panel, action, shortcutWatch](bool on) {
            auto* page = qobject_cast<Page*>(m_stack->currentWidget());
            if (!page)
                return;
            page->setPanelVisible(panel, on);
            if (on && shortcutWatch->firedByShortcut(action) && prefs::focusSearchOnPanelKey())
                panelsearch::focusSearch(page, panel);
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
        m_interfaceBar->setActionShown(action, available);
    }
    QAction* singleViewer = m_ctx.actions->action(QStringLiteral("view.single_viewer"));
    singleViewer->setEnabled(page == m_editPage);
    singleViewer->setChecked(m_editPage->isSingleViewer());
}

void MainWindow::updateTitle()
{
    QString name = m_ctx.project->name();
    // A single-file project locked elsewhere or on a read-only medium (spec 39.24).
    if (m_projectController->isReadOnly())
        name = tr("%1 (Read-Only)").arg(name);
    setWindowTitle(QStringLiteral("%1[*] — %2 %3")
                       .arg(name, QLatin1String(appinfo::name), appinfo::displayVersion()));
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
    // `:q!` of the Vim line closes without asking (spec section 83).
    if (!property("kkDiscardOnClose").toBool() && !m_projectController->maybeSave()) {
        event->ignore();
        return;
    }
    // After a settings import the file's layout stays: this window's old one must not overwrite it.
    if (m_options.saveLayout && !settingstransfer::importPending())
        saveSettings();
    // Normal shutdown: the user saved or discarded the changes, so the autosave is obsolete
    // (one left behind means a crash, see AutosaveController).
    if (m_autosave)
        m_autosave->discard();
    event->accept();
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    // Keep View ▸ Full Screen checked state right when the window manager changes it (Esc, ...).
    if (event->type() == QEvent::WindowStateChange)
        m_ctx.actions->action(QStringLiteral("view.fullscreen"))->setChecked(isFullScreen());
}

void MainWindow::resetUiLayout()
{
    for (const PageEntry& entry : std::as_const(m_pages))
        entry.page->resetLayout();
    syncPanelToggles();
    m_layoutSaveTimer->start();
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
