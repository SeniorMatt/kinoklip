// SPDX-License-Identifier: GPL-3.0-or-later
// EncoderCatalog — which libavcodec encoders implement each export codec, and which hardware
// encoders (NVENC, VAAPI, QSV) actually work on this machine (detected at run time).
#pragma once

#include "export/ExportSettings.h"

#include <QByteArray>
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
    /// The test itself failed: the probe program crashed or hung (a driver fault), as opposed to
    /// the encoder simply not being there. The reason is in @c error.
    bool probeFailed = false;
    bool lowPower = false; ///< VAAPI: needs the low-power entry point
    QString driver; ///< VAAPI driver ("iHD 24.1.0"), when known
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
/// The test runs in a process of its own, `kinoklip-hwenc-probe` (one process per encoder, 10 s
/// limit): a driver that crashes or hangs while opening an encoder (an AMD VCN timeout kills the
/// GPU context of the whole process) only loses the probe, and the encoder (with every other
/// VAAPI encoder, once the driver is known to be bad) is marked unavailable with the reason.
/// Answers are cached on disk, keyed by Kinoklip version, kernel, GPU and driver files, so a bad
/// driver is not tried at every start; retest() forgets them. Nothing is probed until
/// candidates(), hardwareEncoders() or hardwareStatus() is called: the program never probes at
/// start-up (cachedEncoders() answers from memory and disk only). `KINOKLIP_HWENC=off` and
/// `KINOKLIP_HWACCEL=off` disable hardware encoding.
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

    /// What is known without testing anything: results in memory or in the disk cache of this
    /// GPU and driver. Empty when nothing was tested yet.
    [[nodiscard]] static QList<HwEncoderStatus> cachedEncoders();
    /// True if a test program was started in this process.
    [[nodiscard]] static bool probeWasRun();
    /// An encoder that tested fine but failed while rendering: unavailable from now on (this
    /// process; the disk cache is kept, so retest() or a driver update starts again).
    static void markFailed(const QString& encoderName, const QString& reason);
    /// Forgets every answer (memory and disk) so the next call tests again; clears the
    /// "driver crashed" mark.
    static void retest();
    /// True when hardware encoding is switched off by KINOKLIP_HWENC=off or KINOKLIP_HWACCEL=off.
    [[nodiscard]] static bool hardwareDisabledByEnvironment();

    /// The key of the disk cache: Kinoklip version, kernel, FFmpeg, GPUs and driver files.
    [[nodiscard]] static QString driverKey();
    /// Tests only: the test program, its time limit and the cache file (empty = default).
    static void setProbeProgram(const QString& path);
    static void setProbeTimeoutMs(int ms);
    static void setCacheFile(const QString& path);
    /// Tests only: forgets the in-memory answers (not the disk cache).
    static void resetMemory();

    /// What `kinoklip-hwenc-probe` runs: tests @p encoderName in this process and returns the
    /// answer as JSON (available, device, lowPower, driver, error).
    [[nodiscard]] static QByteArray probeHere(const QString& encoderName);
};

} // namespace kk
