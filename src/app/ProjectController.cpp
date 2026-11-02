// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectController implementation.
#include "app/ProjectController.h"

#include "app/AppInfo.h"
#include "app/AutosaveController.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "platform/FileDialogs.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaImport.h"

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
    return QObject::tr("%1 projects (*.%2);;All files (*)")
        .arg(QLatin1String(appinfo::name), QLatin1String(appinfo::projectExtension));
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
    connect(actions.action(QStringLiteral("file.import_subtitle")), &QAction::triggered, this,
        &ProjectController::importSubtitle);
}

bool ProjectController::maybeSave()
{
    if (!m_ctx.project->isDirty())
        return true;
    const auto answer = QMessageBox::warning(m_window, tr("Unsaved Changes"),
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

bool ProjectController::openFile(const QString& path, bool askToSave)
{
    if (askToSave && !maybeSave())
        return false;
    QString error;
    if (!m_ctx.project->load(path, &error)) {
        QMessageBox::critical(m_window, tr("Open Project"), error);
        return false;
    }
    m_ctx.selection->clear();
    rememberDir(kLastProjectDirKey, path);
    addRecentProject(path);
    return true;
}

bool ProjectController::save()
{
    if (m_ctx.project->filePath().isEmpty())
        return saveAs();
    QString error;
    if (!m_ctx.project->save(m_ctx.project->filePath(), &error)) {
        QMessageBox::critical(m_window, tr("Save Project"), error);
        return false;
    }
    return true;
}

bool ProjectController::saveAs()
{
    QString path = platform::getSaveFileName(m_window, tr("Save Project As"), projectFilter(),
        QLatin1String(kLastProjectDirKey),
        m_ctx.project->name() + QLatin1Char('.') + QLatin1String(appinfo::projectExtension));
    if (path.isEmpty())
        return false;
    if (QFileInfo(path).suffix().isEmpty())
        path += QLatin1Char('.') + QLatin1String(appinfo::projectExtension);
    QString error;
    if (!m_ctx.project->save(path, &error)) {
        QMessageBox::critical(m_window, tr("Save Project"), error);
        return false;
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
    importMediaWithFeedback(m_ctx, paths, m_window);
}

void ProjectController::importSubtitle()
{
    // Subtitle files land in the media pool like Resolve's File > Import > Subtitle; dragging
    // the item onto the timeline creates the captions.
    const QStringList paths = platform::getOpenFileNames(m_window, tr("Import Subtitle"),
        subtitleFileDialogFilter(), QLatin1String(kLastMediaDirKey));
    if (paths.isEmpty())
        return;
    importMediaWithFeedback(m_ctx, paths, m_window);
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
        const QString label = number < 10
            ? QStringLiteral("&%1 %2").arg(number).arg(file.completeBaseName())
            : file.completeBaseName();
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
