// SPDX-License-Identifier: GPL-3.0-or-later
// FileManager implementation: FileManager1.ShowItems with an open-the-folder fallback.
#include "platform/FileManager.h"

#include "platform/ChildProcessEnv.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFileInfo>
#include <QUrl>

namespace kk::platform {

namespace {
constexpr char kService[] = "org.freedesktop.FileManager1";
constexpr char kObject[] = "/org/freedesktop/FileManager1";

void openFolderOf(const QString& path)
{
    openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}
} // namespace

void showInFileManager(const QString& path)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusConnectionInterface* iface = bus.isConnected() ? bus.interface() : nullptr;
    // Activatable services (started on demand) count too: Dolphin / Nautilus are usually not
    // running when the user asks.
    const bool available = iface
        && (iface->isServiceRegistered(QLatin1String(kService))
            || iface->activatableServiceNames().value().contains(QLatin1String(kService)));
    if (!available) {
        openFolderOf(path);
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(QLatin1String(kService),
        QLatin1String(kObject), QLatin1String(kService), QStringLiteral("ShowItems"));
    call << QStringList {QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString()}
         << QString();
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(call));
    QObject::connect(
        watcher, &QDBusPendingCallWatcher::finished, watcher, [path](QDBusPendingCallWatcher* w) {
            if (QDBusPendingReply<>(*w).isError())
                openFolderOf(path);
            w->deleteLater();
        });
}

} // namespace kk::platform
