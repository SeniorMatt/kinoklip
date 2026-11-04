// SPDX-License-Identifier: GPL-3.0-or-later
// Preferences — catalogue of user preference keys (QSettings) with typed accessors.
// Lives in core so every module (app, playback, media, export) can read the same keys.
#pragma once

#include <QString>
#include <QStringList>

namespace kk::prefs {

// ---- QSettings keys (the single place where preference key strings are spelled) ----
inline constexpr char kWidgetStyle[] = "ui/widgetStyle"; ///< QStyle key, default "Breeze"
inline constexpr char kColorScheme[]
    = "ui/colorScheme"; ///< KDE colour scheme id, default "BreezeDark"
/// Legacy bool (before the KDE colour schemes): true is migrated to the system scheme.
inline constexpr char kUseSystemColors[] = "ui/useSystemColors";
inline constexpr char kHwAccel[] = "playback/hwaccel"; ///< HwAccel id string, default "auto"
inline constexpr char kAutosaveMinutes[] = "project/autosaveMinutes"; ///< int, 0 = off; default 2
/// Folder of the project library (Project Manager); "" = projectLibraryDefaultDirectory().
inline constexpr char kProjectLibraryDirectory[] = "project/libraryDirectory";
/// What happens at start-up without a project argument: "manager" (default: show the Project
/// Manager) or "last" (reopen the most recent project).
inline constexpr char kStartupAction[] = "project/startupAction";
/// int MiB, memory budget of the decoded still image cache (media::ImageLoader), default 256.
inline constexpr char kStillCacheMiB[] = "system/stillCacheMiB";
/// "auto" (OpenGL compositor when available) or "cpu" (QPainter compositor); start-up only.
inline constexpr char kRenderBackend[] = "system/renderBackend";
/// Folder of the thumbnail / waveform cache ("" = the platform cache folder + "/media").
inline constexpr char kCacheDirectory[] = "system/cacheDirectory";
/// Folder of generated proxy media ("" = "<cache folder>/ProxyMedia", see proxyDirectory()).
inline constexpr char kProxyDirectory[] = "system/proxyDirectory";
/// int GiB, disk budget of the render cache (oldest chunks are deleted first), default 20.
inline constexpr char kRenderCacheLimitGiB[] = "system/renderCacheLimitGiB";
/// QAudioDevice id (UTF-8 bytes as a string) of the playback output; "" = system default.
inline constexpr char kAudioOutputDevice[] = "system/audioOutputDevice";
/// Playback resolution (Playback > Playback Resolution, the viewers' resolution combo,
/// Preferences > Playback): "full" (default), "half", "quarter" or "auto".
inline constexpr char kPreviewQuality[] = "playback/previewQuality";
/// Settings revision of kPreviewQuality (int). Below 2, a stored "auto" (the alpha.1/alpha.2
/// default, which lowered the resolution too eagerly) is migrated to "full" once.
inline constexpr char kPreviewQualityRevision[] = "playback/previewQualityRevision";
/// Mouse-wheel / slider zoom anchor of the timeline: "playhead" (Resolve, default) or "mouse".
inline constexpr char kZoomAnchor[] = "edit/zoomAnchor";
/// bool: show the default smart bins (Video, Audio, Stills, Timelines) in the bin list;
/// default false (Preferences > Editing).
inline constexpr char kDefaultSmartBins[] = "mediaPool/defaultSmartBins";
/// bool: small tweened animations of timeline edits, the playhead and the snap indicator
/// (Preferences > UI Settings > "Animate timeline edits"); default true (v2.2).
inline constexpr char kAnimations[] = "ui/animations";
/// double: speed factor of those animations (0.5 = twice as long .. 2 = half as long);
/// default 1.
inline constexpr char kAnimationSpeed[] = "ui/animationSpeed";
/// Timeline View Options: video clip pictures, "filmstrip" (default), "headtail" or "none".
inline constexpr char kTimelineThumbnails[] = "timeline/thumbnails";
/// Timeline View Options: bool, audio clips draw waveforms (default true).
inline constexpr char kTimelineWaveforms[] = "timeline/waveforms";
/// Colour of new markers: a markerColorPalette() id chosen in the marker button's menu
/// (default "blue"; spec 18.17).
inline constexpr char kNewMarkerColor[] = "timeline/markerColor";
/// Flag of the tool bar's flag button (Clip > Flag Clip): a flagPalette() id chosen in its
/// menu (default "blue"; spec 33.36).
inline constexpr char kToolBarFlag[] = "timeline/flagColor";
/// Timeline View Options > Show Source Timecode (bool, default false; spec 12.14).
inline constexpr char kTimelineSourceTimecode[] = "timeline/sourceTimecode";
/// Edit Index column chooser (spec 18.10): QStringList of the hidden column keys
/// (EditIndexModel::columnKey()), one value per mode: this prefix + "/edits" or "/markers".
inline constexpr char kEditIndexHiddenColumns[] = "editIndex/hiddenColumns";

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

/// Configured project library folder, or empty for the default.
[[nodiscard]] QString projectLibraryDirectoryOverride();
/// Default project library folder: "<Videos>/Kinoklip Projects" (QStandardPaths::MoviesLocation).
[[nodiscard]] QString projectLibraryDefaultDirectory();
/// Project library folder in use (the override, or the default).
[[nodiscard]] QString projectLibraryDirectory();
/// Stores the project library folder ("" = the default).
void setProjectLibraryDirectory(const QString& dir);
/// True (default) if start-up shows the Project Manager, false to reopen the last project.
[[nodiscard]] bool startupShowsProjectManager();
/// Stores the start-up choice (see kStartupAction).
void setStartupShowsProjectManager(bool show);

/// Still image cache budget in MiB (default 256, at least 16).
[[nodiscard]] int stillCacheMiB();
/// True if the user asked for the CPU compositor (takes effect at the next start).
[[nodiscard]] bool forceCpuRendering();
/// Configured cache folder, or empty for the default.
[[nodiscard]] QString cacheDirectory();
/// Cache folder in use: cacheDirectory(), or the default "<platform cache>/media".
[[nodiscard]] QString effectiveCacheDirectory();
/// Configured proxy folder, or empty for the default.
[[nodiscard]] QString proxyDirectoryOverride();
/// Proxy folder in use: kProxyDirectory, or "<effective cache folder>/ProxyMedia".
[[nodiscard]] QString proxyDirectory();
/// Optimized media folder (managed by Kinoklip): "<effective cache folder>/OptimizedMedia".
[[nodiscard]] QString optimizedMediaDirectory();
/// Render cache folder: "<effective cache folder>/RenderCache".
[[nodiscard]] QString renderCacheDirectory();
/// Render cache disk budget in GiB (default 20, at least 1).
[[nodiscard]] int renderCacheLimitGiB();
/// Configured audio output device id, or empty for the system default.
[[nodiscard]] QString audioOutputDevice();
/// Playback resolution id ("full", "half", "quarter", "auto"; default "full"). The first call
/// migrates a stored "auto" from before revision 2 to "full" (see kPreviewQualityRevision).
[[nodiscard]] QString previewQuality();
/// Stores the playback resolution id.
void setPreviewQuality(const QString& id);
/// True if the timeline zooms around the mouse pointer instead of the playhead.
[[nodiscard]] bool zoomAroundMouse();
/// True if the media pool shows the default smart bins (kDefaultSmartBins).
[[nodiscard]] bool showDefaultSmartBins();
/// Stores kDefaultSmartBins.
void setShowDefaultSmartBins(bool show);
/// True if timeline animations are on (kAnimations, default true).
[[nodiscard]] bool animationsEnabled();
/// Stores kAnimations.
void setAnimationsEnabled(bool on);
/// Animation speed factor (kAnimationSpeed), clamped to [0.5, 2], default 1.
[[nodiscard]] double animationSpeed();
/// Stores kAnimationSpeed (clamped to [0.5, 2]).
void setAnimationSpeed(double factor);

} // namespace kk::prefs
