// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Product identity comes from CMake (CW_APP_NAME / CW_APP_ID / CW_APP_VERSION).
namespace cw::appinfo {
inline constexpr const char* name = CW_APP_NAME;
inline constexpr const char* id = CW_APP_ID;
inline constexpr const char* version = CW_APP_VERSION;
inline constexpr const char* projectExtension = "cwproj";
} // namespace cw::appinfo
