// SPDX-License-Identifier: GPL-3.0-or-later
// DesktopNotifications implementation.
#include "platform/DesktopNotifications.h"

#include "app/AppInfo.h"

#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QFile>
#include <QGuiApplication>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcNotify, "kk.notifications", QtInfoMsg)

namespace kk::platform {

namespace {

constexpr int kProbeTimeoutMs = 3000; ///< GetCapabilities (may start the server)

bool serviceAvailable(QDBusConnection& bus, const QString& service)
{
    QDBusConnectionInterface* iface = bus.isConnected() ? bus.interface() : nullptr;
    if (!iface)
        return false;
    // Activatable services count: dunst / mako may only start on the first notification.
    return iface->isServiceRegistered(service)
        || iface->activatableServiceNames().value().contains(service);
}

void logUnavailableOnce(const QString& service)
{
    static bool logged = false;
    if (!logged) {
        logged = true;
        qCInfo(lcNotify) << "no notification service" << service
                         << "on the session bus; render notifications are off";
    }
}

} // namespace

namespace notifications {

QVariantList notifyArguments(const DesktopNotification& n)
{
    QStringList actions;
    for (const DesktopNotification::Action& a : n.actions)
        actions << a.key << a.label;
    QVariantMap hints;
    hints.insert(QStringLiteral("desktop-entry"), QLatin1String(appinfo::desktopId));
    hints.insert(QStringLiteral("category"), QStringLiteral("transfer.complete"));
    return {QLatin1String(appinfo::name), uint(0), QLatin1String(appinfo::desktopId), n.summary,
        n.body, actions, hints, int(-1)};
}

QVariantMap portalNotification(const DesktopNotification& n)
{
    QVariantMap map;
    map.insert(QStringLiteral("title"), n.summary);
    map.insert(QStringLiteral("body"), n.body);
    map.insert(QStringLiteral("priority"), QStringLiteral("normal"));
    if (!n.actions.isEmpty()) {
        QList<QVariantMap> buttons;
        for (const DesktopNotification::Action& a : n.actions) {
            // An action name without "app." comes back as the ActionInvoked signal.
            buttons.append({{QStringLiteral("label"), a.label}, {QStringLiteral("action"), a.key}});
        }
        map.insert(QStringLiteral("buttons"), QVariant::fromValue(buttons));
    }
    return map;
}

bool runningInSandbox()
{
    return QFile::exists(QStringLiteral("/.flatpak-info"));
}

Backend chooseBackend(const QString& override, bool sandboxed)
{
    const QString o = override.trimmed().toLower();
    if (o == QLatin1String("off") || o == QLatin1String("none") || o == QLatin1String("0"))
        return Backend::None;
    if (o == QLatin1String("portal"))
        return Backend::Portal;
    if (o == QLatin1String("freedesktop"))
        return Backend::Freedesktop;
    return sandboxed ? Backend::Portal : Backend::Freedesktop;
}

} // namespace notifications

// ---- org.freedesktop.Notifications ----------------------------------------------------

FreedesktopNotifier::FreedesktopNotifier(
    const QDBusConnection& bus, QString service, QObject* parent)
    : Notifier(parent)
    , m_bus(bus)
    , m_service(std::move(service))
{ }

bool FreedesktopNotifier::available()
{
    if (m_available < 0) {
        m_available = serviceAvailable(m_bus, m_service) ? 1 : 0;
        if (!m_available)
            logUnavailableOnce(m_service);
    }
    return m_available == 1;
}

bool FreedesktopNotifier::supportsActions()
{
    if (m_actions < 0) {
        m_actions = 0;
        if (available()) {
            QDBusMessage call = QDBusMessage::createMethodCall(m_service,
                QLatin1String(notifications::kObjectPath), QLatin1String(notifications::kInterface),
                QStringLiteral("GetCapabilities"));
            const QDBusReply<QStringList> reply = m_bus.call(call, QDBus::Block, kProbeTimeoutMs);
            m_actions = reply.isValid() && reply.value().contains(QLatin1String("actions"));
        }
    }
    return m_actions == 1;
}

bool FreedesktopNotifier::show(const DesktopNotification& notification, ActionHandler onAction)
{
    if (!available())
        return false;
    if (!m_connected) {
        m_connected = true;
        const QString path = QLatin1String(notifications::kObjectPath);
        const QString iface = QLatin1String(notifications::kInterface);
        m_bus.connect(m_service, path, iface, QStringLiteral("ActionInvoked"), this,
            SLOT(onActionInvoked(uint, QString)));
        m_bus.connect(m_service, path, iface, QStringLiteral("NotificationClosed"), this,
            SLOT(onClosed(uint, uint)));
    }
    DesktopNotification n = notification;
    if (!supportsActions())
        n.actions.clear();
    QDBusMessage call
        = QDBusMessage::createMethodCall(m_service, QLatin1String(notifications::kObjectPath),
            QLatin1String(notifications::kInterface), QStringLiteral("Notify"));
    call.setArguments(notifications::notifyArguments(n));
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this, handler = std::move(onAction), hasActions = !n.actions.isEmpty()](
            QDBusPendingCallWatcher* w) {
            const QDBusPendingReply<uint> reply = *w;
            if (reply.isError())
                qCInfo(lcNotify) << "Notify failed:" << reply.error().message();
            else if (hasActions && handler)
                m_handlers.insert(reply.value(), handler);
            w->deleteLater();
        });
    return true;
}

void FreedesktopNotifier::onActionInvoked(uint id, const QString& key)
{
    const ActionHandler handler = m_handlers.take(id);
    if (handler)
        handler(key);
}

void FreedesktopNotifier::onClosed(uint id, uint /*reason*/)
{
    m_handlers.remove(id);
}

// ---- org.freedesktop.portal.Notification ----------------------------------------------

PortalNotifier::PortalNotifier(const QDBusConnection& bus, QString service, QObject* parent)
    : Notifier(parent)
    , m_bus(bus)
    , m_service(std::move(service))
{
    qDBusRegisterMetaType<QList<QVariantMap>>();
}

bool PortalNotifier::available()
{
    if (m_available < 0) {
        m_available = serviceAvailable(m_bus, m_service) ? 1 : 0;
        if (!m_available)
            logUnavailableOnce(m_service);
    }
    return m_available == 1;
}

bool PortalNotifier::show(const DesktopNotification& notification, ActionHandler onAction)
{
    if (!available())
        return false;
    if (!m_connected) {
        m_connected = true;
        m_bus.connect(m_service, QLatin1String(notifications::kPortalObjectPath),
            QLatin1String(notifications::kPortalInterface), QStringLiteral("ActionInvoked"), this,
            SLOT(onActionInvoked(QString, QString, QVariantList)));
    }
    // Ids are per application; a fresh one per render keeps earlier notifications visible.
    const QString id = QStringLiteral("render-%1").arg(m_next++);
    QDBusMessage call
        = QDBusMessage::createMethodCall(m_service, QLatin1String(notifications::kPortalObjectPath),
            QLatin1String(notifications::kPortalInterface), QStringLiteral("AddNotification"));
    call << id << notifications::portalNotification(notification);
    if (onAction && !notification.actions.isEmpty())
        m_handlers.insert(id, std::move(onAction));
    auto* watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(
        watcher, &QDBusPendingCallWatcher::finished, this, [this, id](QDBusPendingCallWatcher* w) {
            const QDBusPendingReply<> reply = *w;
            if (reply.isError()) {
                qCInfo(lcNotify) << "AddNotification failed:" << reply.error().message();
                m_handlers.remove(id);
            }
            w->deleteLater();
        });
    return true;
}

void PortalNotifier::onActionInvoked(
    const QString& id, const QString& action, const QVariantList& /*parameter*/)
{
    const ActionHandler handler = m_handlers.take(id);
    if (handler)
        handler(action);
}

// ---- Factory ----------------------------------------------------------------------------

std::unique_ptr<Notifier> createDesktopNotifier(QObject* parent)
{
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal"))
        return nullptr; // tests and screenshot runs never reach the user's desktop
    switch (notifications::chooseBackend(
        qEnvironmentVariable(notifications::kOverrideVar), notifications::runningInSandbox())) {
    case notifications::Backend::None:
        return nullptr;
    case notifications::Backend::Portal:
        return std::make_unique<PortalNotifier>(
            QDBusConnection::sessionBus(), QLatin1String(notifications::kPortalService), parent);
    case notifications::Backend::Freedesktop:
        break;
    }
    return std::make_unique<FreedesktopNotifier>(
        QDBusConnection::sessionBus(), QLatin1String(notifications::kService), parent);
}

} // namespace kk::platform
