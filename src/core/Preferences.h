// SPDX-License-Identifier: GPL-3.0-or-later
// Preferences — catalogue of user preference keys (QSettings) with typed accessors.
// Lives in core so every module (app, playback, media, export) can read the same keys.
#pragma once

#include <QString>
#include <QStringList>

namespace kk::prefs {

// ---- QSettings keys (the single place where preference key strings are spelled) ----
inline constexpr char kUseSystemColors[] = "ui/useSystemColors"; ///< bool, default false
inline constexpr char kHwAccel[] = "playback/hwaccel"; ///< HwAccel id string, default "auto"
inline constexpr char kAutosaveMinutes[] = "project/autosaveMinutes"; ///< int, 0 = off; default 2

/// Hardware video acceleration choice for decode/encode (consumed by src/media, src/playback
/// and src/export; the GPU compositor is always used when OpenGL 3.3 is available).
enum class HwAccel {
    Auto, ///< pick the best available API for the GPU (VA-API, NVDEC/NVENC, QSV), else CPU
    Off, ///< software decode/encode only
    Vaapi, ///< VA-API (Intel, AMD; Mesa)
    Cuda, ///< NVIDIA NVDEC/NVENC
    Qsv, ///< Intel Quick Sync Video
    Vulkan ///< Vulkan video (experimental drivers)
};

/// Stable id stored in the settings ("auto", "off", "vaapi", "cuda", "qsv", "vulkan").
[[nodiscard]] QString hwAccelToString(HwAccel mode);
/// Parses a stored id; unknown values map to HwAccel::Auto.
[[nodiscard]] HwAccel hwAccelFromString(const QString& id);
/// All ids in UI order (for a Preferences combo box).
[[nodiscard]] QStringList hwAccelIds();

/// Current hardware acceleration preference (default Auto).
[[nodiscard]] HwAccel hwAccel();
/// Stores the hardware acceleration preference.
void setHwAccel(HwAccel mode);

/// Autosave interval in minutes (0 = disabled, default 2).
[[nodiscard]] int autosaveMinutes();
/// Stores the autosave interval.
void setAutosaveMinutes(int minutes);

} // namespace kk::prefs
