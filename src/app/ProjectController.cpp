// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectController implementation.
#include "app/ProjectController.h"

#include "app/AppInfo.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/MediaImport.h"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>

namespace cw {

namespace {
constexpr char kLastProjectDirKey[] = "paths/lastProjectDir";
constexpr char kLastMediaDirKey[] = "paths/lastMediaDir";

QString projectFilter()
{
    return QObject::tr("%1 projects (*.%2);;All files (*)")
        .arg(QLatin1String(appinfo::name), QLatin1String(appinfo::projectExtension));
}

QString lastDir(const char* key)
{
    return QSettings().value(QLatin1String(key), QDir::homePath()).toString();
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
    const QString path = QFileDialog::getOpenFileName(
        m_window, tr("Open Project"), lastDir(kLastProjectDirKey), projectFilter());
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
    const QString suggested = QDir(lastDir(kLastProjectDirKey))
                                  .filePath(m_ctx.project->name() + QLatin1Char('.')
                                      + QLatin1String(appinfo::projectExtension));
    QString path
        = QFileDialog::getSaveFileName(m_window, tr("Save Project As"), suggested, projectFilter());
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
    return true;
}

void ProjectController::importMedia()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        m_window, tr("Import Media"), lastDir(kLastMediaDirKey), mediaFileDialogFilter());
    if (paths.isEmpty())
        return;
    rememberDir(kLastMediaDirKey, paths.front());
    importMediaWithFeedback(m_ctx, paths, m_window);
}

} // namespace cw
