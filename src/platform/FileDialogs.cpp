// SPDX-License-Identifier: GPL-3.0-or-later
// FileDialogs implementation: portal first, QFileDialog as the fallback.
#include "platform/FileDialogs.h"

#include "platform/PortalFileChooser.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSettings>
#include <QWidget>
#include <QWindow>

#include <optional>

namespace kk::platform {

namespace {

Q_LOGGING_CATEGORY(lcFileDialogs, "kinoklip.filedialogs", QtInfoMsg)

constexpr char kOverrideVar[] = "KINOKLIP_FILE_DIALOG";
/// Test hook: talk to this D-Bus name instead of org.freedesktop.portal.Desktop.
constexpr char kServiceVar[] = "KINOKLIP_PORTAL_SERVICE";

QString portalService()
{
    const QString s = qEnvironmentVariable(kServiceVar);
    return s.isEmpty() ? QString::fromLatin1(portal::kService) : s;
}

/// Portal FileChooser version, probed once per service name.
uint portalVersion()
{
    static QString probedService;
    static std::optional<uint> cached;
    const QString service = portalService();
    if (!cached || probedService != service) {
        probedService = service;
        cached = portal::FileChooser(QDBusConnection::sessionBus(), service).version();
        qCDebug(lcFileDialogs) << "portal file chooser" << service << "version" << *cached;
    }
    return *cached;
}

QStringList runQtDialog(QWidget* parent, const FileDialogRequest& r, const QString& startDir)
{
    using Mode = FileDialogRequest::Mode;
    switch (r.mode) {
    case Mode::OpenFile: {
        const QString p = QFileDialog::getOpenFileName(parent, r.title, startDir, r.filter);
        return p.isEmpty() ? QStringList() : QStringList {p};
    }
    case Mode::OpenFiles:
        return QFileDialog::getOpenFileNames(parent, r.title, startDir, r.filter);
    case Mode::SaveFile: {
        const QString suggested
            = r.fileName.isEmpty() ? startDir : QDir(startDir).filePath(r.fileName);
        const QString p = QFileDialog::getSaveFileName(parent, r.title, suggested, r.filter);
        return p.isEmpty() ? QStringList() : QStringList {p};
    }
    case Mode::Directory: {
        const QString p = QFileDialog::getExistingDirectory(parent, r.title, startDir);
        return p.isEmpty() ? QStringList() : QStringList {p};
    }
    }
    return {};
}

void rememberFolder(const FileDialogRequest& r, const QStringList& paths)
{
    if (r.settingsKey.isEmpty() || paths.isEmpty())
        return;
    const QString first = paths.constFirst();
    const QString folder = r.mode == FileDialogRequest::Mode::Directory
        ? QFileInfo(first).absoluteFilePath()
        : QFileInfo(first).absolutePath();
    if (portal::isDocumentPortalPath(folder))
        return; // a per-grant FUSE mirror, not a folder the user browses
    QSettings().setValue(r.settingsKey, folder);
}

} // namespace

FileDialogBackend chooseFileDialogBackend(
    const QString& override, uint portalVersion, FileDialogRequest::Mode mode)
{
    const QString o = override.trimmed().toLower();
    if (o == QLatin1String("qt") || o == QLatin1String("widget"))
        return FileDialogBackend::Qt;
    if (portalVersion == 0)
        return FileDialogBackend::Qt;
    if (mode == FileDialogRequest::Mode::Directory && portalVersion < portal::kDirectoryVersion)
        return FileDialogBackend::Qt;
    return FileDialogBackend::Portal;
}

FileDialogBackend currentFileDialogBackend(FileDialogRequest::Mode mode)
{
    const QString override = qEnvironmentVariable(kOverrideVar);
    if (override.trimmed().compare(QLatin1String("qt"), Qt::CaseInsensitive) == 0)
        return FileDialogBackend::Qt; // no need to touch the bus
    return chooseFileDialogBackend(override, portalVersion(), mode);
}

QString startDirectory(const FileDialogRequest& request)
{
    if (!request.directory.isEmpty())
        return request.directory;
    if (!request.settingsKey.isEmpty()) {
        const QString remembered = QSettings().value(request.settingsKey).toString();
        if (!remembered.isEmpty() && QFileInfo(remembered).isDir())
            return remembered;
    }
    return QDir::homePath();
}

namespace {
std::function<QStringList(const FileDialogRequest&)>& testHandler()
{
    static std::function<QStringList(const FileDialogRequest&)> handler;
    return handler;
}
} // namespace

void setFileDialogHandlerForTests(std::function<QStringList(const FileDialogRequest&)> handler)
{
    testHandler() = std::move(handler);
}

QStringList runFileDialog(QWidget* parent, const FileDialogRequest& request)
{
    if (testHandler())
        return testHandler()(request);
    const QString startDir = startDirectory(request);
    QStringList paths;
    bool done = false;
    if (currentFileDialogBackend(request.mode) == FileDialogBackend::Portal) {
        QWindow* window = parent && parent->window() ? parent->window()->windowHandle() : nullptr;
        const portal::FileChooser chooser(QDBusConnection::sessionBus(), portalService());
        const portal::FileChooser::Result result
            = chooser.run(request, startDir, portal::parentWindowIdentifier(window));
        if (result.ok) {
            paths = result.paths;
            done = true;
        } else {
            qCWarning(lcFileDialogs)
                << "portal file dialog failed, using Qt's dialog:" << result.error;
        }
    }
    // "portal" = portal only: a failed portal call does not open Qt's dialog instead.
    const bool portalOnly = qEnvironmentVariable(kOverrideVar)
                                .trimmed()
                                .compare(QLatin1String("portal"), Qt::CaseInsensitive)
        == 0;
    if (!done && !portalOnly)
        paths = runQtDialog(parent, request, startDir);
    if (request.mode != FileDialogRequest::Mode::OpenFiles && paths.size() > 1)
        paths = paths.mid(0, 1);
    rememberFolder(request, paths);
    return paths;
}

QString getOpenFileName(QWidget* parent, const QString& title, const QString& filter,
    const QString& settingsKey, const QString& directory)
{
    FileDialogRequest r;
    r.mode = FileDialogRequest::Mode::OpenFile;
    r.title = title;
    r.filter = filter;
    r.settingsKey = settingsKey;
    r.directory = directory;
    return runFileDialog(parent, r).value(0);
}

QStringList getOpenFileNames(QWidget* parent, const QString& title, const QString& filter,
    const QString& settingsKey, const QString& directory)
{
    FileDialogRequest r;
    r.mode = FileDialogRequest::Mode::OpenFiles;
    r.title = title;
    r.filter = filter;
    r.settingsKey = settingsKey;
    r.directory = directory;
    return runFileDialog(parent, r);
}

QString getSaveFileName(QWidget* parent, const QString& title, const QString& filter,
    const QString& settingsKey, const QString& suggestedName, const QString& directory)
{
    FileDialogRequest r;
    r.mode = FileDialogRequest::Mode::SaveFile;
    r.title = title;
    r.filter = filter;
    r.settingsKey = settingsKey;
    r.fileName = suggestedName;
    r.directory = directory;
    return runFileDialog(parent, r).value(0);
}

QString getExistingDirectory(
    QWidget* parent, const QString& title, const QString& settingsKey, const QString& directory)
{
    FileDialogRequest r;
    r.mode = FileDialogRequest::Mode::Directory;
    r.title = title;
    r.settingsKey = settingsKey;
    r.directory = directory;
    return runFileDialog(parent, r).value(0);
}

} // namespace kk::platform
