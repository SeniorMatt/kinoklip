// SPDX-License-Identifier: GPL-3.0-or-later
// HwDevices — internal: shared FFmpeg hardware device contexts and hwaccel helpers.
#pragma once

#include "media/FFmpegUtil.h"
#include "media/HwAccel.h"

#include <QList>

#include <mutex>

namespace kk::media::detail {

/// Per-decoder state for FFmpeg's get_format callback (AVCodecContext::opaque points here).
struct HwDecodeState
{
    AVPixelFormat hwFormat = AV_PIX_FMT_NONE; ///< the hardware surface format we asked for
    bool rejected = false; ///< FFmpeg could not use hw for this stream → software frames
};

/// Hardware backends to try for `mode`, best first (Auto → platform order; Off → none).
QList<HwAccelMode> hwCandidates(HwAccelMode mode);

/// The hardware surface pixel format `codec` produces with backend `mode`, or
/// AV_PIX_FMT_NONE when FFmpeg has no such hwaccel for this codec.
AVPixelFormat hwPixelFormat(const AVCodec* codec, HwAccelMode mode);

/// Configures `ctx` (not yet opened) to decode on `device`: sets hw_device_ctx, the
/// get_format callback and `state` as opaque. `device` is referenced, not taken over.
void attachHwDevice(AVCodecContext* ctx, AVBufferRef* device, HwDecodeState* state);

/// Which hardware decoders serialise their FFmpeg calls with a per-device mutex
/// (HwDeviceManager::callLock()). Environment: KINOKLIP_HW_SERIALIZE=off|vaapi|all.
enum class HwSerialise {
    Off, ///< no locking (drivers that are fully thread-safe)
    Vaapi, ///< default: VAAPI only (see HwDeviceManager::callLock())
    All, ///< every backend (diagnostics; tests run CUDA this way)
};

/// Process-wide owner of one AVHWDeviceContext per backend, shared by all decoders.
/// Devices are created lazily on first use; a failure is remembered for the process lifetime
/// (never retried, logged once), so decoder opens without a GPU cost nothing extra.
/// AVBufferRef reference counting keeps a device alive while any decoder uses it; the
/// manager itself is never destroyed (drivers must not be torn down during static exit).
/// Thread-safety: all methods are thread-safe.
class HwDeviceManager
{
public:
    static HwDeviceManager& instance();

    /// A new reference to the device context of `mode` (caller must av_buffer_unref() it),
    /// or nullptr if the backend is not available on this machine.
    AVBufferRef* acquire(HwAccelMode mode);

    /// Availability and details of backend `mode` (opens the device if not tried yet).
    HwDeviceInfo info(HwAccelMode mode);

    /// The details of the backends that were already tried (never opens a device).
    QList<HwDeviceInfo> probed();

    /// "VAAPI (iHD 24.1.0, /dev/dri/renderD128)": backend, driver and device for log lines.
    QString describe(HwAccelMode mode);

    /// The mutex every decoder on the shared device of `mode` holds around each libavcodec
    /// call that may reach the driver (send/receive/flush/open/free) and around downloads, or
    /// nullptr when that backend is not serialised (see HwSerialise).
    ///
    /// Why: all decoders of the process share one VADisplay (see the class comment), used
    /// concurrently by the render thread, its decode-ahead and parallel-decode workers, the
    /// thumbnail pool and export. FFmpeg's hwcontext_vaapi takes no lock, libva promises no
    /// thread safety, and Intel's iHD answered vaSyncSurface with "operation failed" in the
    /// alpha.3 log once decode-ahead added concurrency (its status reports are per decode
    /// context and surface heap; Chromium serialises libva with a global lock for the same
    /// reason). Holding the lock per call (not per frame) keeps the waits short: GPU decode
    /// is asynchronous, only the download's vaSyncSurface waits for the GPU.
    std::mutex* callLock(HwAccelMode mode);

    /// Records that a decoder on backend `mode` gave up and fell back to software. After
    /// kMaxDecoderFailures such failures the backend is disabled for the rest of the session
    /// (acquire() returns nullptr) and one warning says so. @return true if now disabled.
    bool reportFailure(HwAccelMode mode, const QString& reason);

    /// Number of fallbacks reported for `mode` (tests).
    int failureCount(HwAccelMode mode);

    /// Test hooks: forget failures (re-enables disabled backends) / change the policy.
    void resetFailures();
    void setSerialise(HwSerialise policy);

    /// Fall-backs after which a backend is disabled for the session.
    static constexpr int kMaxDecoderFailures = 3;

private:
    HwDeviceManager();

    struct Entry
    {
        bool tried = false;
        AVBufferRef* device = nullptr;
        HwDeviceInfo info;
        int failures = 0;
        bool disabled = false;
        std::mutex callMutex;
    };
    Entry& ensure(HwAccelMode mode); // m_mutex must be held

    std::mutex m_mutex;
    HwSerialise m_serialise = HwSerialise::Vaapi;
    Entry m_entries[5]; ///< indexed by int(HwAccelMode)
};

/// Fault injection for tests: the next `count` hardware frame downloads (any decoder, any
/// thread) fail as if vaSyncSurface had failed (AVERROR(EIO)). Thread-safe.
void injectDownloadFailures(int count);

/// Consumes one injected failure. @return true if this download must fail.
bool takeInjectedDownloadFailure();

} // namespace kk::media::detail
