// SPDX-License-Identifier: GPL-3.0-or-later
// PortalFileChooser — the desktop's file dialog through org.freedesktop.portal.FileChooser.
#pragma once

#include "platform/FileDialogs.h"

#include <QDBusConnection>
#include <QList>
#include <QMetaType>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class QDBusArgument;
class QWindow;

namespace kk::platform::portal {

// Talks to the XDG desktop portal directly over D-Bus (the protocol Qt's own
// "xdgdesktopportal" platform theme uses), so the app gets the native KDE/GNOME dialog without
// switching its platform theme: palette, fonts, icons and the widget style stay exactly as
// src/app/Theme sets them. Spec:
// https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.FileChooser.html

inline constexpr char kService[] = "org.freedesktop.portal.Desktop";
inline constexpr char kObjectPath[] = "/org/freedesktop/portal/desktop";
inline constexpr char kInterface[] = "org.freedesktop.portal.FileChooser";
inline constexpr char kRequestInterface[] = "org.freedesktop.portal.Request";
/// First interface version with the "directory" option (folder pickers).
inline constexpr uint kDirectoryVersion = 3;

/// One portal filter: a name and (type, pattern) pairs; type 0 = glob, 1 = MIME type.
/// D-Bus signature (sa(us)).
struct Filter
{
    QString name;
    QList<QPair<uint, QString>> patterns;
    bool operator==(const Filter& o) const { return name == o.name && patterns == o.patterns; }
};
using FilterList = QList<Filter>;

/// Registers the D-Bus types (idempotent; called by FileChooser).
void registerTypes();

/// "*.mp4" -> "*.[mM][pP]4": portal globs are case-sensitive, Qt's filters are not.
[[nodiscard]] QString caseInsensitiveGlob(const QString& glob);

/// Qt filter string ("Media (*.mp4 *.mov);;All files (*)") -> portal filters. A filter
/// without "(...)" is taken as one pattern list named after itself.
[[nodiscard]] FilterList parseQtFilters(const QString& qtFilter);

/// "OpenFile" or "SaveFile" for a dialog mode.
[[nodiscard]] QString methodFor(FileDialogRequest::Mode mode);

/// The a{sv} options of the OpenFile/SaveFile call for @p request starting in @p startDir.
[[nodiscard]] QVariantMap buildOptions(
    const FileDialogRequest& request, const QString& startDir, const QString& handleToken);

/// Request object path the portal will use: /org/freedesktop/portal/desktop/request/
/// <sender without ':' and '.'->'_'>/<token>.
[[nodiscard]] QString requestPath(const QString& uniqueName, const QString& token);

/// file:// URIs of a Response -> local paths (other schemes are dropped: the app reads local
/// files only). Document-portal paths (/run/user/<uid>/doc/...) are normal local files.
[[nodiscard]] QStringList pathsFromUris(const QStringList& uris);

/// True for a path inside the document portal's FUSE mount (not worth remembering as the
/// "last folder": it only holds the files the user granted).
[[nodiscard]] bool isDocumentPortalPath(const QString& path);

/// "x11:<id>" / "wayland:<exported handle>" for @p window, empty if unknown (the dialog is then
/// not attached to our window, but still works).
[[nodiscard]] QString parentWindowIdentifier(QWindow* window);

/// One portal endpoint (the session bus' org.freedesktop.portal.Desktop by default; tests
/// point it at a fake service).
class FileChooser
{
public:
    explicit FileChooser(
        const QDBusConnection& bus = QDBusConnection::sessionBus(), QString service = kService);

    /// FileChooser interface version, 0 if there is no portal or it has no file chooser
    /// backend. Activates the portal if needed (blocking, bounded by a timeout).
    [[nodiscard]] uint version() const;

    struct Result
    {
        bool ok = false; ///< false: the portal could not be used (caller falls back)
        QStringList paths; ///< chosen paths; empty with ok = cancelled
        QString error;
    };

    /// Shows the dialog and waits for the answer in a local event loop, with the
    /// application's windows blocked as for a modal dialog.
    [[nodiscard]] Result run(const FileDialogRequest& request, const QString& startDir,
        const QString& parentWindow) const;

private:
    QDBusConnection m_bus;
    QString m_service;
};

} // namespace kk::platform::portal

Q_DECLARE_METATYPE(kk::platform::portal::Filter)
Q_DECLARE_METATYPE(kk::platform::portal::FilterList)

QDBusArgument& operator<<(QDBusArgument& arg, const kk::platform::portal::Filter& filter);
const QDBusArgument& operator>>(const QDBusArgument& arg, kk::platform::portal::Filter& filter);
