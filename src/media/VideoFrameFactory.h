// SPDX-License-Identifier: GPL-3.0-or-later
// VideoFrameFactory — internal: wraps decoder output as VideoFrames (hw download, pooling).
#pragma once

#include "media/FFmpegUtil.h"
#include "media/FrameConverter.h"
#include "media/VideoFrame.h"

#include <QImage>

#include <atomic>
#include <memory>

namespace kk::media::detail {

/// Timing/geometry that the decoder knows but the AVFrame does not carry reliably.
struct FrameMeta
{
    int64_t ptsUs = 0;
    int64_t durationUs = 0;
    Rational sampleAspectRatio {1, 1};
    int rotation = 0;
};

/// Builds VideoFrames from decoder output for one VideoDecoder:
/// - software frames in a PixelFormat layout are wrapped by reference (no copy);
/// - hardware frames are downloaded into NV12/P010 buffers taken from a pool, so steady-state
///   playback allocates no pixel memory (VAAPI: mapped + streaming-load copy, see
///   copyMapped(); others: av_hwframe_transfer_data);
/// - any other format is converted once to premultiplied Bgra.
/// Pools are reference counted by FFmpeg: frames may outlive the factory.
/// TODO(media): zero-copy GPU interop (VAAPI → EGLImage/dma-buf, CUDA → GL) instead of the
/// download, once the compositor runs on OpenGL.
/// Thread-safety: not thread-safe (owned by one decoder).
class VideoFrameFactory
{
public:
    VideoFrameFactory() = default;
    ~VideoFrameFactory();
    VideoFrameFactory(const VideoFrameFactory&) = delete;
    VideoFrameFactory& operator=(const VideoFrameFactory&) = delete;

    /// Zero-copy output: hardware frames of VAAPI in NV12/P010 are not downloaded but exported as
    /// DMA-BUF (VideoFrame::dmabuf, no pixel data). Falls back to the download, for good, when
    /// the export is not possible (no libdrm in FFmpeg, driver without PRIME export).
    void setZeroCopy(bool enabled) { m_zeroCopy = enabled; }
    /// Optional switch shared with the consumer: exports only while it reads true (the
    /// compositor clears it when the GPU cannot import the buffers).
    void setZeroCopyGate(std::shared_ptr<const std::atomic<bool>> gate)
    {
        m_gate = std::move(gate);
    }
    [[nodiscard]] bool zeroCopy() const
    {
        return m_zeroCopy && !m_exportFailed && (!m_gate || m_gate->load());
    }

    /// Wraps/downloads/converts `decoded`. @return nullptr on failure (a failed hardware
    /// download means the caller should fall back to software decoding).
    std::shared_ptr<const VideoFrame> make(const AVFrame* decoded, const FrameMeta& meta);

    /// Downloads a hardware frame into a pooled software frame (NV12/P010...).
    /// @return nullptr on failure (see lastError()); nothing is logged above debug level.
    FramePtr download(const AVFrame* hwFrame);

    /// FFmpeg error code of the last failed download (0 after a successful one).
    [[nodiscard]] int lastError() const { return m_lastError; }
    /// "Input/output error (Failed to sync surface 0x3: 1 (operation failed))" or empty.
    [[nodiscard]] QString lastErrorString() const;

    /// Wraps a premultiplied ARGB32 image (stills) as a Bgra frame sharing its pixels.
    static std::shared_ptr<const VideoFrame> wrapImage(const QImage& image, const FrameMeta& meta);

private:
    /// VAAPI: maps @p hwFrame and copies it into @p dst (allocated, same format) with
    /// streaming loads. @return 0 on success, AVERROR(ENOSYS) if mapping is not possible
    /// (caller transfers instead), another error if the surface cannot be read at all.
    int copyMapped(AVFrame* dst, const AVFrame* hwFrame);

    /// Exports @p hwFrame as DMA-BUF (see setZeroCopy()). nullptr when it cannot (the caller
    /// downloads instead).
    std::shared_ptr<const VideoFrame> makeDmaBuf(const AVFrame* hwFrame, const FrameMeta& meta);

    bool m_zeroCopy = false;
    std::shared_ptr<const std::atomic<bool>> m_gate;
    bool m_exportFailed = false; ///< export impossible for this stream: always download
    AVBufferPool* m_pool = nullptr;
    bool m_mapFailed = false; ///< mapping failed once: always transfer
    size_t m_poolBufferSize = 0;
    int m_lastError = 0;
    QString m_lastErrorText; ///< first FFmpeg error message of the last failed download
    FrameConverter m_converter; ///< for formats outside PixelFormat → Bgra
};

} // namespace kk::media::detail
