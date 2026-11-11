// SPDX-License-Identifier: GPL-3.0-or-later
// VideoDecoder — frame-accurate random access to the video frames of one media file.
#pragma once

#include "media/HwAccel.h"
#include "media/MediaInfo.h"
#include "media/VideoFrame.h"

#include <QImage>
#include <QSize>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace kk::media {

/// Software decoding threads when the caller does not choose (VideoDecoder::setThreadCount(0)):
/// 4 up to 1080p, 3/4 of @p cores (at least 4, at most 8) above it. KINOKLIP_SW_THREADS=<n>
/// overrides it. Pure apart from that variable.
int autoSoftwareThreads(int width, int height, int cores);

/// Decodes the video stream of one file (or a still image) by media time — as raw planes for
/// the GPU compositor (decodeFrame()) or as a ready-to-paint QImage (frameAt()).
///
/// Usage:
/// ```cpp
/// VideoDecoder decoder;
/// if (!decoder.open(path, &error)) { ... }
/// auto frame = decoder.decodeFrame(2'500'000);                 // planes, for textures
/// QImage image = decoder.frameAt(2'500'000, QSize(960, 540));  // thumbnails, tests
/// ```
///
/// Time: `timeUs` is media time in microseconds (0 = start of the file, see MediaInfo).
/// Both calls return the frame whose presentation interval [pts, next pts) contains `timeUs`;
/// requests before the first / after the last frame are clamped to that frame.
///
/// Performance: the decoder remembers its position. Asking for the same frame again returns
/// the cached result; moving forward a little decodes forward without seeking (playback,
/// stepping); anything else seeks to the previous keyframe and decodes forward from there.
/// Hardware decoding (VAAPI/CUDA/Vulkan, see HwAccel.h) is used when available, with a
/// transparent fall-back to software (at open or mid-stream). Software decoding uses at most
/// 4 threads by default, more for frames above 1080p (see autoSoftwareThreads()). Intermediate
/// frames skipped while seeking are never converted or downloaded from the GPU.
///
/// Output of frameAt(): `QImage::Format_ARGB32_Premultiplied`, sample aspect ratio and display
/// rotation applied, alpha preserved (opaque sources have alpha 255). decodeFrame() leaves SAR
/// and rotation to the consumer (fields of VideoFrame).
///
/// Thread-safety: NOT thread-safe — use one instance per consumer/thread. Separate instances
/// (even for the same file) can be used concurrently from different threads.
class VideoDecoder
{
public:
    /// How exactly frameAt() must hit the requested time.
    enum class Accuracy {
        /// The frame whose presentation interval contains the requested time.
        Exact,
        /// Fast and approximate, for scrubbing and thumbnails: returns the keyframe at or
        /// before the requested time — or the frame the decoder is already on if that one is
        /// closer — and never decodes more than one frame after a seek.
        NearestKeyframe,
    };

    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /// Number of software decoding threads used by the next open(): 0 = automatic (the
    /// default, see autoSoftwareThreads()), 1 =
    /// single-threaded (best when many decoders run in parallel). Hardware decoding always
    /// uses one thread.
    void setThreadCount(int threads);

    /// Hardware decoding for the next open(), overriding the global preferredHwAccel().
    void setHwAccel(HwAccelMode mode);

    /// Zero-copy output of decodeFrame() (VAAPI only): frames come as DMA-BUF handles
    /// (VideoFrame::dmabuf, no pixel data, no copy through system memory) for a consumer that
    /// imports them as GPU textures (render/GlDmaBuf.h). Call before open() (a larger surface pool
    /// is allocated for it); may be turned off any time, and turns itself off for a stream or
    /// GPU that cannot export. frameAt() is not affected (it downloads). Default off.
    void setZeroCopy(bool enabled);
    /// A switch shared with the consumer (any thread may clear it): frames are exported only
    /// while it reads true; the consumer clears it when it cannot import them.
    void setZeroCopyGate(std::shared_ptr<const std::atomic<bool>> gate);
    /// True while decodeFrame() returns DMA-BUF frames for the open file.
    bool zeroCopyActive() const;

    /// Catch-up mode (the playback engine is far behind the clock): the decoder discards frames
    /// no other frame refers to (FFmpeg AVDISCARD_NONREF, as mpv's framedrop does), which costs
    /// about half the decoding in streams with B-frames; decodeFrame() then returns the nearest
    /// decoded frame before the time asked for. Takes effect at once and for later open()s.
    void setSkipNonReference(bool skip);

    /// Backend actually decoding the open file: Cuda/Vaapi/Vulkan, or Off for software
    /// (also after a fall-back). Never Auto.
    HwAccelMode activeHwAccel() const;

    /// Opens `path` and decodes its first frame to make sure it is usable.
    /// @return false (with a message in `*error`) if the file has no decodable video.
    [[nodiscard]] bool open(const QString& path, QString* error = nullptr);

    /// Releases the file and all decoder state. Called automatically by the destructor.
    void close();

    /// True between a successful open() and close().
    bool isOpen() const;

    /// Description of the open file (valid=false when nothing is open).
    const MediaInfo& info() const;

    /// The frame shown at media time `timeUs` (microseconds) as raw planes (see VideoFrame):
    /// hardware frames are downloaded to NV12/P010 (or, with setZeroCopy(), handed over as
    /// DMA-BUF handles without pixel data), software frames are shared without a copy.
    /// The same pointer is returned while `timeUs` stays within one frame.
    /// @return nullptr on decoding failure or if nothing is open.
    std::shared_ptr<const VideoFrame> decodeFrame(
        int64_t timeUs, Accuracy accuracy = Accuracy::Exact);

    /// Look-ahead (render::DecoderPool::prepare()): decodeFrame() that gives up between two
    /// decoded frames once @p stop returns true. On success the decoder holds the frame, so the
    /// decodeFrame() of the same time costs nothing; after a stop it is left in a valid state
    /// (the next call continues or seeks as usual).
    /// @return true if the frame is ready.
    ///
    /// With @p readAheadFrames > 0 the frames after it are decoded too (and held, bounded by that
    /// number): the requests for them cost nothing either, as long as they come before any
    /// request the held frames cannot answer.
    bool prepareFrame(
        int64_t timeUs, const std::function<bool()>& stop, int readAheadFrames = 0);

    /// Frames this decoder took from libavcodec since open(), skipped ones included
    /// (measurements and tests; not synchronised with a decode running on another thread).
    int64_t decodedFrameCount() const;

    /// The frame shown at media time `timeUs` (microseconds), scaled to fit inside `maxSize`
    /// while keeping the display aspect ratio (scales up or down).
    /// `maxSize` components <= 0 are unbounded, so `QSize(0, 90)` means "90 px high" and the
    /// default `QSize()` returns the native display size (MediaInfo::displayWidth/Height).
    /// @return a null QImage on decoding failure or if nothing is open.
    QImage frameAt(int64_t timeUs, QSize maxSize = {}, Accuracy accuracy = Accuracy::Exact);

    /// Media time (µs) at which the frame returned by the last frameAt() starts,
    /// or -1 if no frame has been returned yet. Always 0 for still images.
    int64_t lastFrameTimeUs() const;

    /// Largest size with the aspect ratio of `source` that fits into `maxSize`
    /// (components <= 0 are unbounded). Never returns less than 1×1 for a non-empty source.
    static QSize fitSize(QSize source, QSize maxSize);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace kk::media
