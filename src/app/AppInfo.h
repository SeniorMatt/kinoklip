// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Product identity comes from CMake (KK_APP_NAME / KK_APP_ID / KK_APP_VERSION).
namespace kk::appinfo {
inline constexpr const char* name = KK_APP_NAME;
inline constexpr const char* id = KK_APP_ID;
inline constexpr const char* version = KK_APP_VERSION; // base version; see prerelease
/// Pre-release tag shown in the About box and window title (empty for a final release).
inline constexpr const char* prerelease = "alpha.1";
inline constexpr const char* projectExtension = "kkproj";
} // namespace kk::appinfo
