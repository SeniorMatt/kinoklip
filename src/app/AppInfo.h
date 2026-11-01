// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Product identity comes from CMake (KK_APP_NAME / KK_APP_ID / KK_APP_VERSION).
namespace kk::appinfo {
inline constexpr const char* name = KK_APP_NAME;
inline constexpr const char* id = KK_APP_ID;
inline constexpr const char* version = KK_APP_VERSION;
inline constexpr const char* projectExtension = "kkproj";
/// Reverse-DNS application id: desktop entry name, Wayland app_id, icon name, MIME package
/// (packaging/linux/org.kinoklip.Kinoklip.desktop).
inline constexpr const char* desktopId = "org.kinoklip.Kinoklip";
} // namespace kk::appinfo
