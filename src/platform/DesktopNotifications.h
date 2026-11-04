// SPDX-License-Identifier: GPL-3.0-or-later
// DesktopNotifications — system notifications ("Render complete") through the freedesktop
// notification service, or the XDG portal's Notification interface inside a sandbox.
#pragma once

#include <QDBusConnection>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <functional>
#include <memory>

namespace kk::platform {

/// One notification: a title, a body and optional buttons ("Open Folder", "Play").
struct DesktopNotification
{
    struct Action
    {
        QString key; ///< sent back when the button is pressed ("open-folder")
        QString label; ///< button text ("Open Folder")
    };
    QString summary;
    QString body;
    QList<Action> actions;
};

/// Shows desktop notifications. GUI thread only.
class Notifier : public QObject
{
    Q_OBJECT
public:
    /// Called with the action key when the user presses one of the notification's buttons.
    using ActionHandler = std::function<void(const QString& key)>;

    using QObject::QObject;

    /// Sends @p notification (asynchronously). @p onAction runs at most once, for a pressed
    /// button. @return false if there is no notification service (nothing was sent).
    virtual bool show(const DesktopNotification& notification, ActionHandler onAction) = 0;
    /// True if buttons can be shown (the server announces the "actions" capability).
    [[nodiscard]] virtual bool supportsActions() = 0;
};

namespace notifications {

inline constexpr char kService[] = "org.freedesktop.Notifications";
inline constexpr char kObjectPath[] = "/org/freedesktop/Notifications";
inline constexpr char kInterface[] = "org.freedesktop.Notifications";
inline constexpr char kPortalService[] = "org.freedesktop.portal.Desktop";
inline constexpr char kPortalObjectPath[] = "/org/freedesktop/portal/desktop";
inline constexpr char kPortalInterface[] = "org.freedesktop.portal.Notification";
/// Override of the backend choice: "freedesktop", "portal" or "off" (default: automatic).
inline constexpr char kOverrideVar[] = "KINOKLIP_NOTIFICATIONS";

/// Arguments of org.freedesktop.Notifications.Notify (signature susssasa{sv}i): app name
/// "Kinoklip", no replaced id, the desktop file's icon id, summary, body, the actions as
/// key / label pairs, hints (desktop-entry, category transfer.complete) and the server's
/// default timeout.
[[nodiscard]] QVariantList notifyArguments(const DesktopNotification& notification);

/// The a{sv} notification of org.freedesktop.portal.Notification.AddNotification: title,
/// body, priority and the buttons (aa{sv} of label / action).
[[nodiscard]] QVariantMap portalNotification(const DesktopNotification& notification);

/// True inside a Flatpak sandbox (/.flatpak-info exists), where the session bus only offers
/// the portal.
[[nodiscard]] bool runningInSandbox();

enum class Backend {
    None,
    Freedesktop,
    Portal
};
/// Backend for @p override (kOverrideVar) and @p sandboxed: the portal inside a sandbox, the
/// freedesktop service otherwise; "off" disables notifications.
[[nodiscard]] Backend chooseBackend(const QString& override, bool sandboxed);

} // namespace notifications

/// org.freedesktop.Notifications (Plasma, GNOME Shell, dunst, mako, ...). Tests point it at a
/// fake service on a private bus.
class FreedesktopNotifier final : public Notifier
{
    Q_OBJECT
public:
    explicit FreedesktopNotifier(const QDBusConnection& bus = QDBusConnection::sessionBus(),
        QString service = QLatin1String(notifications::kService), QObject* parent = nullptr);

    bool show(const DesktopNotification& notification, ActionHandler onAction) override;
    [[nodiscard]] bool supportsActions() override;

private Q_SLOTS:
    void onActionInvoked(uint id, const QString& key);
    void onClosed(uint id, uint reason);

private:
    bool available();

    QDBusConnection m_bus;
    QString m_service;
    int m_available = -1; ///< -1 not probed yet
    int m_actions = -1;
    bool m_connected = false;
    QHash<uint, ActionHandler> m_handlers;
};

/// org.freedesktop.portal.Notification (inside Flatpak). Its notifications carry the app's
/// icon from the sandbox' desktop file.
class PortalNotifier final : public Notifier
{
    Q_OBJECT
public:
    explicit PortalNotifier(const QDBusConnection& bus = QDBusConnection::sessionBus(),
        QString service = QLatin1String(notifications::kPortalService), QObject* parent = nullptr);

    bool show(const DesktopNotification& notification, ActionHandler onAction) override;
    [[nodiscard]] bool supportsActions() override { return true; }

private Q_SLOTS:
    void onActionInvoked(const QString& id, const QString& action, const QVariantList& parameter);

private:
    bool available();

    QDBusConnection m_bus;
    QString m_service;
    int m_available = -1;
    bool m_connected = false;
    quint64 m_next = 1;
    QHash<QString, ActionHandler> m_handlers;
};

/// The notifier for this desktop (chooseBackend() with the environment), or nullptr when
/// notifications are off. Offscreen / minimal platform runs (tests, screenshots) get nullptr
/// so they never reach the user's real desktop.
[[nodiscard]] std::unique_ptr<Notifier> createDesktopNotifier(QObject* parent = nullptr);

} // namespace kk::platform
