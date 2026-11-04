// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectController implementation.
#include "app/ProjectController.h"

#include "app/AppInfo.h"
#include "app/AutosaveController.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "pack/EmbeddedMedia.h"
#include "pack/ProjectPackage.h"
#include "platform/FileDialogs.h"
#include "projects/ProjectLibrary.h"
#include "projects/ProjectLock.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/FolderImport.h"
#include "ui/common/MediaImport.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/projects/KeymapChoiceBanner.h"
#include "ui/projects/NewProjectDialog.h"
#include "ui/projects/ProjectManagerWindow.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>

namespace kk {

namespace {
constexpr char kLastProjectDirKey[] = "paths/lastProjectDir";
constexpr char kLastMediaDirKey[] = "paths/lastMediaDir";
constexpr char kRecentProjectsKey[] = "paths/recentProjects";

QString projectFilter()
{
    return QObject::tr("%1 projects (*.%2 *.%3);;All files (*)")
        .arg(QLatin1String(appinfo::name), QLatin1String(pack::kContainerSuffix),
            QLatin1String(appinfo::projectExtension));
}

/// Save As: both kinds, the open project's kind first (the dialog's default filter).
QString saveFilter(bool singleFileFirst)
{
    const QString single
        = QObject::tr("Single-file project (*.%1)").arg(QLatin1String(pack::kContainerSuffix));
    const QString linked
        = QObject::tr("Linked project (*.%1)").arg(QLatin1String(appinfo::projectExtension));
    return singleFileFirst ? single + QStringLiteral(";;") + linked
                           : linked + QStringLiteral(";;") + single;
}

void rememberDir(const char* key, const QString& filePath)
{
    QSettings().setValue(QLatin1String(key), QFileInfo(filePath).absolutePath());
}
} // namespace

ProjectController::ProjectController(AppContext& ctx, QWidget* window)
    : QObject(window)
    , m_ctx(ctx)
    , m_window(window)
{
    ActionRegistry& actions = *ctx.actions;
    connect(actions.action(QStringLiteral("file.project_manager")), &QAction::triggered, this,
        &ProjectController::showProjectManager);
    connect(actions.action(QStringLiteral("file.new")), &QAction::triggered, this,
        &ProjectController::newProject);
    connect(actions.action(QStringLiteral("file.open")), &QAction::triggered, this,
        &ProjectController::open);
    connect(actions.action(QStringLiteral("file.save")), &QAction::triggered, this,
        &ProjectController::save);
    connect(actions.action(QStringLiteral("file.save_as")), &QAction::triggered, this,
        &ProjectController::saveAs);
    connect(actions.action(QStringLiteral("file.import_media")), &QAction::triggered, this,
        &ProjectController::importMedia);
    connect(actions.action(QStringLiteral("file.import_media_folder")), &QAction::triggered, this,
        &ProjectController::importMediaFolder);
    connect(actions.action(QStringLiteral("file.import_subtitle")), &QAction::triggered, this,
        &ProjectController::importSubtitle);
    connect(actions.action(QStringLiteral("file.pack_project")), &QAction::triggered, this,
        &ProjectController::packProject);
    connect(actions.action(QStringLiteral("file.unpack_project")), &QAction::triggered, this,
        &ProjectController::unpackProject);
    connect(actions.action(QStringLiteral("file.compact_project")), &QAction::triggered, this,
        &ProjectController::compactProject);
    connect(actions.action(QStringLiteral("file.verify_project")), &QAction::triggered, this,
        &ProjectController::verifyProject);
    ctx.mediaStore = this;
    updatePackageActions();
}

ProjectController::~ProjectController()
{
    delete m_manager.data();
    if (m_ctx.mediaStore == this)
        m_ctx.mediaStore = nullptr;
    // No releaseContainer(): the window is being destroyed, nobody may be told any more.
    m_package.reset();
    m_lock.reset();
}

bool ProjectController::maybeSave(QWidget* dialogParent)
{
    if (!m_ctx.project->isDirty())
        return true;
    const auto answer
        = QMessageBox::warning(dialogParent ? dialogParent : m_window, tr("Unsaved Changes"),
            tr("Save the changes to “%1” before closing?").arg(m_ctx.project->name()),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return save();
    return answer == QMessageBox::Discard;
}

void ProjectController::newProject()
{
    if (!maybeSave())
        return;
    m_ctx.selection->clear();
    m_ctx.project->newProject();
    releaseContainer();
}

void ProjectController::open()
{
    if (!maybeSave())
        return;
    const QString path = platform::getOpenFileName(
        m_window, tr("Open Project"), projectFilter(), QLatin1String(kLastProjectDirKey));
    if (!path.isEmpty())
        openFile(path, false);
}

bool ProjectController::openFile(const QString& path, bool askToSave, QWidget* dialogParent)
{
    if (askToSave && !maybeSave(dialogParent))
        return false;
    // An autosave of this project left by a crash (the start-up prompt was answered "Not
    // Now") is offered again: opening the file without asking would soon autosave over it.
    if (offerRecovery(path)) {
        rememberDir(kLastProjectDirKey, path);
        addRecentProject(path);
        return true;
    }
    QString error;
    QWidget* parent = dialogParent ? dialogParent : m_window;
    if (pack::isContainerFile(path)) {
        if (!openContainer(path, &error, parent)) {
            QMessageBox::critical(parent, tr("Open Project"), error);
            return false;
        }
    } else {
        if (!m_ctx.project->load(path, &error)) {
            QMessageBox::critical(parent, tr("Open Project"), error);
            return false;
        }
        releaseContainer();
    }
    m_ctx.selection->clear();
    rememberDir(kLastProjectDirKey, path);
    addRecentProject(path);
    return true;
}

bool ProjectController::save()
{
    Q_EMIT m_ctx.project->aboutToSave();
    if (m_ctx.project->filePath().isEmpty())
        return saveAs();
    QString error;
    if (pack::isContainerFile(m_ctx.project->filePath())) {
        if (!m_package || m_readOnly) {
            QMessageBox::information(m_window, tr("Save Project"),
                tr("“%1” is open read-only. Save it under another name.")
                    .arg(QFileInfo(m_ctx.project->filePath()).fileName()));
            return saveAs();
        }
        if (!saveContainer(&error)) {
            QMessageBox::critical(m_window, tr("Save Project"), error);
            return false;
        }
        return true;
    }
    if (!m_ctx.project->save(m_ctx.project->filePath(), &error)) {
        QMessageBox::critical(m_window, tr("Save Project"), error);
        return false;
    }
    return true;
}

bool ProjectController::saveAs()
{
    Q_EMIT m_ctx.project->aboutToSave();
    // A never-saved project offers the kind New Project would create; otherwise its own kind.
    const QString current = m_ctx.project->filePath();
    const bool single = current.isEmpty()
        ? NewProjectDialog::preferredKindId() == QLatin1String(pack::kContainerSuffix)
        : pack::isContainerFile(current);
    const QString suffix
        = single ? QLatin1String(pack::kContainerSuffix) : QLatin1String(appinfo::projectExtension);
    QString path = platform::getSaveFileName(m_window, tr("Save Project As"), saveFilter(single),
        QLatin1String(kLastProjectDirKey), m_ctx.project->name() + QLatin1Char('.') + suffix);
    if (path.isEmpty())
        return false;
    if (QFileInfo(path).suffix().isEmpty())
        path += QLatin1Char('.') + suffix;
    path = QFileInfo(path).absoluteFilePath();
    // The project takes the name it is saved under, as with Resolve's Save Project As (the
    // title bar showed "Untitled Project" after the first save), when it is new or its name
    // was its file's; a name set in Project Settings stays.
    const QString oldName = m_ctx.project->name();
    if (current.isEmpty() || oldName == QFileInfo(current).completeBaseName())
        m_ctx.project->setName(QFileInfo(path).completeBaseName());
    const auto failed = [&] {
        m_ctx.project->setName(oldName);
        return false;
    };
    if (pack::isContainerFile(path)) {
        // The open single-file project itself: a plain save.
        if (m_package && !m_readOnly && path == m_package->path())
            return save() || failed();
        if (!saveAsSingleFile(path))
            return failed();
    } else if (isSingleFile()) {
        if (!saveAsLinked(path))
            return failed();
    } else {
        QString error;
        if (!m_ctx.project->save(path, &error)) {
            QMessageBox::critical(m_window, tr("Save Project"), error);
            return failed();
        }
        releaseContainer(); // a never-saved or recovered project: nothing held
    }
    rememberDir(kLastProjectDirKey, path);
    addRecentProject(path);
    return true;
}

void ProjectController::importMedia()
{
    const QStringList paths = platform::getOpenFileNames(
        m_window, tr("Import Media"), mediaFileDialogFilter(), QLatin1String(kLastMediaDirKey));
    if (paths.isEmpty())
        return;
    importMediaWithFeedback(m_ctx, paths, m_window, importTargetBin(m_ctx));
}

void ProjectController::importMediaFolder()
{
    MediaPoolPanel* pool = shownMediaPool();
    const QString target = importTargetBin(m_ctx);
    const QStringList ids = importMediaFolderWithDialogs(m_ctx, m_window, target);
    if (pool)
        pool->showImportedItems(ids, target);
}

MediaPoolPanel* ProjectController::shownMediaPool() const
{
    // File > Import goes into the bin the user is looking at, as in Resolve (the media pool
    // of the page shown; pages keep the same bin, MainWindow::setCurrentPage()).
    const QList<MediaPoolPanel*> pools = m_window->findChildren<MediaPoolPanel*>();
    for (MediaPoolPanel* pool : pools) {
        if (pool->isVisible())
            return pool;
    }
    return pools.isEmpty() ? nullptr : pools.front();
}

void ProjectController::importSubtitle()
{
    // Subtitle files land in the media pool like Resolve's File > Import > Subtitle; dragging
    // the item onto the timeline creates the captions.
    const QStringList paths = platform::getOpenFileNames(m_window, tr("Import Subtitle"),
        subtitleFileDialogFilter(), QLatin1String(kLastMediaDirKey));
    if (paths.isEmpty())
        return;
    importMediaWithFeedback(m_ctx, paths, m_window, importTargetBin(m_ctx));
}

bool ProjectController::openLastProject()
{
    for (const QString& path : recentProjects()) {
        if (QFileInfo::exists(path))
            return openFile(path, false);
    }
    return false;
}

// ---- Project Manager ----

ProjectLibrary* ProjectController::projectLibrary()
{
    const QString root
        = QDir::cleanPath(QFileInfo(prefs::projectLibraryDirectory()).absoluteFilePath());
    if (m_library && m_library->root() != root) {
        // Preferences moved the library: a new library (and window) for the new folder.
        delete m_manager.data();
        delete m_library;
        m_library = nullptr;
    }
    if (!m_library) {
        m_library = new ProjectLibrary(root, {}, this);
        connect(m_library, &ProjectLibrary::moved, this, &ProjectController::followMovedProject);
    }
    return m_library;
}

ProjectManagerWindow* ProjectController::projectManager() const
{
    return m_manager.data();
}

void ProjectController::showProjectManager()
{
    ProjectLibrary* library = projectLibrary();
    if (!m_manager) {
        m_manager = new ProjectManagerWindow(library, m_window);
        // Over the editor like a dialog, but shown with show(): start-up and tests go on.
        m_manager->setWindowModality(Qt::WindowModal);
        m_manager->setSwitchGuard([this](QWidget* parent) { return maybeSave(parent); });
        connect(m_manager, &ProjectManagerWindow::openRequested, this, [this](const QString& path) {
            const bool alreadyOpen = QFileInfo(m_ctx.project->filePath()).absoluteFilePath()
                == QFileInfo(path).absoluteFilePath();
            if (alreadyOpen || openFile(path, false, m_manager))
                m_manager->hide();
        });
        connect(m_manager, &ProjectManagerWindow::projectSettingsRequested, this, [this] {
            m_manager->hide();
            m_ctx.actions->action(QStringLiteral("file.project_settings"))->trigger();
        });
        // First start: "Coming from another editor?" (spec 38.32).
        if (KeymapChoiceBanner::shouldOffer())
            m_manager->setTopBanner(new KeymapChoiceBanner(*m_ctx.actions));
    }
    m_manager->setRecentProjects(recentProjects());
    m_manager->setOpenProjectPath(m_ctx.project->filePath());
    m_manager->show();
    m_manager->raise();
    m_manager->activateWindow();
}

// ---- Crash recovery ----

bool ProjectController::offerRecovery(const QString& onlyDocument)
{
    const QString only
        = onlyDocument.isEmpty() ? QString() : QFileInfo(onlyDocument).absoluteFilePath();
    for (const RecoveryCandidate& c : AutosaveController::findRecoverable()) {
        if (!only.isEmpty() && QFileInfo(c.documentPath).absoluteFilePath() != only)
            continue;
        QMessageBox box(QMessageBox::Warning, tr("Recover Unsaved Changes"),
            tr("%1 did not shut down properly. An autosave of “%2” from %3 has changes "
               "that were not saved.")
                .arg(QLatin1String(appinfo::name), c.name,
                    QLocale().toString(c.savedAt, QLocale::ShortFormat)),
            QMessageBox::NoButton, m_window);
        box.setInformativeText(c.documentPath.isEmpty()
                ? tr("The project had never been saved.")
                : tr("Project file: %1\nRestoring opens the autosave; save it to replace the "
                     "project file.")
                      .arg(QDir::toNativeSeparators(c.documentPath)));
        QPushButton* restore = box.addButton(tr("&Restore"), QMessageBox::AcceptRole);
        QPushButton* discard = box.addButton(tr("&Discard"), QMessageBox::DestructiveRole);
        box.addButton(tr("&Not Now"), QMessageBox::RejectRole);
        box.setDefaultButton(restore);
        box.exec();
        if (box.clickedButton() == discard) {
            AutosaveController::discardCandidate(c);
            continue;
        }
        if (box.clickedButton() != restore)
            continue; // kept for the next start
        QString error;
        if (!m_ctx.project->loadRecovered(c.autosavePath, c.documentPath, &error)) {
            QMessageBox::critical(m_window, tr("Recover Unsaved Changes"), error);
            continue;
        }
        // A single-file project's media stay in its file: hold it again for saving.
        if (pack::isContainerFile(c.documentPath)) {
            if (!attachContainer(c.documentPath, &error))
                QMessageBox::warning(m_window, tr("Recover Unsaved Changes"), error);
        } else {
            releaseContainer();
        }
        m_ctx.selection->clear();
        if (m_autosave)
            m_autosave->adopt(c.autosavePath);
        return true; // one project per window; older candidates wait for the next start
    }
    return false;
}

// ---- Recent projects ----

QStringList ProjectController::recentProjects()
{
    return QSettings().value(QLatin1String(kRecentProjectsKey)).toStringList();
}

void ProjectController::addRecentProject(const QString& path)
{
    const QString absolute = QFileInfo(path).absoluteFilePath();
    QStringList list = recentProjects();
    list.removeAll(absolute);
    list.prepend(absolute);
    while (list.size() > kMaxRecentProjects)
        list.removeLast();
    QSettings().setValue(QLatin1String(kRecentProjectsKey), list);
}

void ProjectController::followMovedProject(const QString& from, const QString& to)
{
    // A renamed or moved project (or folder of projects) stays in File > Open Recent.
    const QString prefix = from + QLatin1Char('/');
    QStringList list = recentProjects();
    bool changed = false;
    for (QString& path : list) {
        if (path == from) {
            path = to;
            changed = true;
        } else if (path.startsWith(prefix)) {
            path = to + QLatin1Char('/') + path.mid(prefix.size());
            changed = true;
        }
    }
    if (!changed)
        return;
    QSettings().setValue(QLatin1String(kRecentProjectsKey), list);
    if (m_manager)
        m_manager->setRecentProjects(list);
}

void ProjectController::clearRecentProjects()
{
    QSettings().remove(QLatin1String(kRecentProjectsKey));
}

void ProjectController::attachRecentMenu(QMenu* menu)
{
    // Rebuilt on every opening: cheap (ten entries) and always reflects other windows'
    // changes and files that went missing meanwhile.
    connect(menu, &QMenu::aboutToShow, this, [this, menu] { fillRecentMenu(menu); });
    fillRecentMenu(menu);
}

void ProjectController::fillRecentMenu(QMenu* menu)
{
    menu->clear();
    const QStringList list = recentProjects();
    int number = 0;
    for (const QString& path : list) {
        const QFileInfo file(path);
        // "&1 name" accelerators for the first nine, like KDE's recent files menus.
        ++number;
        // A "&" in the name is text, not a mnemonic.
        QString name = file.completeBaseName();
        name.replace(QLatin1Char('&'), QStringLiteral("&&"));
        const QString label = number < 10 ? QStringLiteral("&%1 %2").arg(number).arg(name) : name;
        QAction* action = menu->addAction(label);
        action->setToolTip(QDir::toNativeSeparators(path));
        action->setStatusTip(QDir::toNativeSeparators(path));
        if (!file.exists()) {
            action->setEnabled(false);
            action->setText(tr("%1 (missing)").arg(label));
            continue;
        }
        connect(action, &QAction::triggered, this, [this, path] { openFile(path, true); });
    }
    if (list.isEmpty()) {
        menu->addAction(tr("No Recent Projects"))->setEnabled(false);
        return;
    }
    menu->addSeparator();
    connect(menu->addAction(tr("&Clear Menu")), &QAction::triggered, this,
        [] { clearRecentProjects(); });
}

} // namespace kk
