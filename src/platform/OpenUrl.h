// SPDX-License-Identifier: GPL-3.0-or-later
// OpenUrl — opens links and local files with the desktop's handler, robustly.
#pragma once

#include <QString>
#include <QStringList>

class QUrl;

namespace kk::platform {

/// Who opened a URL (or None: nothing could).
enum class UrlOpener {
    None,
    Portal, ///< org.freedesktop.portal.OpenURI (OpenURI / OpenFile) over D-Bus
    Helper, ///< xdg-open / kde-open / kde-open5 / gio open, started with childEnvironment()
    QtServices, ///< QDesktopServices::openUrl inside ScopedOriginalEnvironment
};

/// "portal", "helper", "qt", "none".
[[nodiscard]] QString urlOpenerName(UrlOpener opener);

/// Opens @p url (a web link, mailto:, or a local file / folder) in the user's browser, mail
/// program or file manager. Tries, in order (see src/platform/readme.md, "Opening links"):
///   1. the XDG desktop portal's OpenURI interface (OpenURI for links, OpenFile with a file
///      descriptor for local paths): the desktop itself starts the handler, so the bundle's
///      environment never reaches it and it works on Wayland and immutable systems;
///   2. xdg-open, kde-open, kde-open5, `gio open` from the user's PATH, in that order, with
///      childEnvironment() (the launcher's variables undone); one that exits non-zero within
///      1.5 s (no browser configured, ...) counts as failed and the next is tried;
///   3. QDesktopServices::openUrl inside ScopedOriginalEnvironment, except on Wayland with a
///      focus window: there Qt starts the handler only after an activation-token round trip,
///      i.e. after we return, with the bundle's environment, and not at all if the calling
///      window is destroyed in between (a dialog that closes itself), yet returns true.
/// Synchronous; blocks at most a few seconds (portal activation, its answer, the helper's
/// first 1.5 s). GUI thread only.
/// Offscreen / minimal platforms (tests) open nothing unless KINOKLIP_URL_OPENERS is set.
/// Environment: KINOKLIP_URL_OPENERS=<comma list of portal,helper,qt> picks and orders the
/// steps ("" or "none": nothing); KINOKLIP_PORTAL_SERVICE=<bus name> talks to another portal
/// (tests). Returns who opened it.
UrlOpener openUrlWith(const QUrl& url);

/// openUrlWith(url) != UrlOpener::None.
bool openUrl(const QUrl& url);

/// The steps openUrlWith() takes, from KINOKLIP_URL_OPENERS or the default
/// {"portal", "helper", "qt"}; unknown names are ignored.
[[nodiscard]] QStringList urlOpenerOrder();

} // namespace kk::platform
