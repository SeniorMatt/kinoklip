// SPDX-License-Identifier: GPL-3.0-or-later
// HwAccel — hardware video decoding: user preference, capability listing (for Preferences).
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

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

} // namespace kk::media
