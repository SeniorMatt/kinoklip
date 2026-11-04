// SPDX-License-Identifier: GPL-3.0-or-later
// HwAccel — hardware video decoding: user preference, capability listing (for Preferences).
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

struct AVBufferRef;

namespace kk::media {

/// Hardware decoding backend selection.
/// `Auto` tries the platform's backends in order of preference (Linux: VAAPI on the GPU that
/// drives the display → CUDA/NVDEC → Vulkan Video) and falls back to software decoding.
/// A specific backend is used if it works for the stream, otherwise software is used.
enum class HwAccelMode {
    Auto,
    Off, ///< always decode in software
    Cuda,
    Vaapi,
    Vulkan,
};

/// What one hardware backend can do on this machine (see hwCapabilities()).
struct HwDeviceInfo
{
    HwAccelMode mode = HwAccelMode::Off;
    QString name; ///< "VAAPI", "CUDA", "Vulkan"
    bool available = false;
    QString device; ///< device that was opened, e.g. "/dev/dri/renderD129" (may be empty)
    QString driver; ///< short driver description, e.g. "iHD 24.1.0" (VAAPI; may be empty)
    QString error; ///< why it is unavailable (when !available)
    /// Codecs FFmpeg can decode with this backend. Whether a given profile (e.g. HEVC 10-bit
    /// or AV1) really works depends on the GPU; decoders fall back to software if not.
    QStringList codecs;
};

/// Human-readable name of a mode ("Auto", "Off", "CUDA", "VAAPI", "Vulkan").
QString hwAccelName(HwAccelMode mode);

/// Sets the global preference used by every VideoDecoder opened afterwards (unless the
/// decoder has its own VideoDecoder::setHwAccel()). Default: Auto. Thread-safe.
void setPreferredHwAccel(HwAccelMode mode);

/// The current global preference. Thread-safe.
HwAccelMode preferredHwAccel();

/// The hardware backends of this platform in Auto order, each probed once (devices are
/// opened on first call, which can take ~0.1-1 s; cached afterwards). Thread-safe.
QList<HwDeviceInfo> hwCapabilities();

/// The backends this process already tried to open (by a decoder, the warm-up or the
/// Preferences page), in Auto order; opens nothing. For Help > Report a Bug. Thread-safe.
QList<HwDeviceInfo> probedHwDevices();

/// Opens the hardware decoding device that Auto (or the preferred backend) will use, on a
/// background thread, so the first decoder after start-up does not pay for it (creating a
/// CUDA context takes 200 - 300 ms; the first frame after an import used to wait for it).
/// Stops at the first available backend in Auto order; Vulkan only when it is preferred.
/// Does nothing when hardware decoding is off or after the first call. Thread-safe.
void warmUpHwDecoding();

/// True if an NVIDIA driver device node exists (/dev/nvidiactl).
/// Without it CUDA/NVDEC/NVENC cannot work, so they are not even tried (FFmpeg would log
/// CUDA_ERROR_NO_DEVICE errors). Linux only; true elsewhere (let the driver decide).
bool nvidiaDriverPresent();

/// True if Intel Quick Sync (FFmpeg's *_qsv) can possibly work: an Intel GPU render node and
/// a oneVPL/Media SDK runtime library (libmfx-gen.so.1.2 or libmfxhw64.so.1) can be loaded,
/// or ONEVPL_SEARCH_PATH is set. Without them FFmpeg logs "Error creating a MFX session".
bool qsvRuntimePresent();

/// Short name of the VA driver behind a VAAPI device context, e.g. "iHD 24.1.0" or
/// "Mesa Gallium 24.2.1"; empty if unknown. Used for log lines.
QString vaapiDriverName(AVBufferRef* device);

/// The libva library actually loaded, e.g. "libva 2.22.0 (host, /usr/lib/libva.so.2.2200.0)"
/// or "... (bundled, ...)"; empty if libva is not loaded.
QString libvaDescription();

} // namespace kk::media
