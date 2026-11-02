// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
// Product identity comes from CMake (KK_APP_NAME / KK_APP_ID / KK_APP_VERSION).
namespace kk::appinfo {
inline constexpr const char* name = KK_APP_NAME;
inline constexpr const char* id = KK_APP_ID;
inline constexpr const char* version = KK_APP_VERSION; // base version; see prerelease
/// Pre-release tag shown in the About box and window title (empty for a final release).
inline constexpr const char* prerelease = "alpha.1";
/// Version as shown to the user: "0.1.0-alpha.1" (base version + "-" + prerelease if set).
inline QString displayVersion()
{
    const QString base = QLatin1String(version);
    return *prerelease ? base + QLatin1Char('-') + QLatin1String(prerelease) : base;
}
inline constexpr const char* projectExtension = "kkproj";
/// Reverse-DNS application id: desktop entry name, Wayland app_id, icon name, MIME package
/// (packaging/linux/org.kinoklip.Kinoklip.desktop).
inline constexpr const char* desktopId = "org.kinoklip.Kinoklip";
} // namespace kk::appinfo
