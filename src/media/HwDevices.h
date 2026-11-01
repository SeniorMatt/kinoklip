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

/// Process-wide owner of one AVHWDeviceContext per backend, shared by all decoders.
/// Devices are created lazily on first use; a failure is remembered (not retried).
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

private:
    struct Entry
    {
        bool tried = false;
        AVBufferRef* device = nullptr;
        HwDeviceInfo info;
    };
    Entry& ensure(HwAccelMode mode); // m_mutex must be held

    std::mutex m_mutex;
    Entry m_entries[5]; ///< indexed by int(HwAccelMode)
};

} // namespace kk::media::detail
