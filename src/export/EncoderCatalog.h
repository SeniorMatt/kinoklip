// SPDX-License-Identifier: GPL-3.0-or-later
// EncoderCatalog — which libavcodec encoders implement each export codec, and which hardware
// encoders (NVENC, VAAPI, QSV) actually work on this machine (detected at run time).
#pragma once

#include "export/ExportSettings.h"

#include <QList>
#include <QString>

struct AVBufferRef;

namespace kk {

/// One concrete encoder for a codec.
struct EncoderChoice
{
    QString encoderName; ///< libavcodec name ("h264_vaapi", "libx264", ...)
    EncoderBackend backend = EncoderBackend::Software;

    friend bool operator==(const EncoderChoice&, const EncoderChoice&) = default;
};

/// Detection result of one hardware encoder (for the Deliver page / logs / tests).
struct HwEncoderStatus
{
    QString codecId; ///< VideoCodecInfo id
    QString encoderName;
    EncoderBackend backend = EncoderBackend::Auto;
    bool available = false;
    QString device; ///< VAAPI render node used ("/dev/dri/renderD128"), else empty
    QString error; ///< why it is unavailable
};

/// Encoder lookup and hardware detection.
///
/// Detection opens each hardware encoder once with a small test picture and encodes one
/// frame (a successful avcodec_open2 alone is not enough: NVENC without a capable GPU and
/// VAAPI drivers without the entry point fail later). VAAPI is tried on every render node
/// (`/dev/dri/renderD*`), first with the normal and then with the low-power entry point
/// (Intel Gen 11+ only supports some codecs in low-power mode). Results are cached for the
/// process lifetime; the first full detection takes ~0.2-2 s, so call hardwareEncoders()
/// from a worker thread. The environment variable `KINOKLIP_HWENC=off` disables hardware
/// encoding (tests, debugging).
///
/// Thread-safety: all functions are thread-safe.
class EncoderCatalog
{
public:
    /// Encoders to try for @p codecId with @p preference, best first. Auto: every hardware
    /// encoder that works here (NVENC, VAAPI, QSV), then software. A specific backend: that
    /// one if it works, then software as the fallback. Probes hardware lazily (may block).
    [[nodiscard]] static QList<EncoderChoice> candidates(
        const QString& codecId, EncoderBackend preference);
    /// The software encoder(s) of @p codecId that exist in this FFmpeg build, best first.
    [[nodiscard]] static QList<EncoderChoice> softwareEncoders(const QString& codecId);
    /// Probes (once) and returns every known hardware encoder with its status.
    [[nodiscard]] static QList<HwEncoderStatus> hardwareEncoders();
    /// Status of one hardware encoder (probes it once).
    [[nodiscard]] static HwEncoderStatus hardwareStatus(const QString& encoderName);
    /// True if the encoder works: software = present in FFmpeg; hardware = detection passed.
    [[nodiscard]] static bool isAvailable(const EncoderChoice& choice);

    /// For VAAPI encoders: a new reference to the device the encoder works on (caller
    /// unrefs) and whether it needs the low-power entry point. nullptr when unavailable.
    [[nodiscard]] static AVBufferRef* vaapiDevice(const QString& encoderName, bool* lowPower);
};

} // namespace kk
