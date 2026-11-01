// SPDX-License-Identifier: GPL-3.0-or-later
// VideoFrameFactory — internal: wraps decoder output as VideoFrames (hw download, pooling).
#pragma once

#include "media/FFmpegUtil.h"
#include "media/FrameConverter.h"
#include "media/VideoFrame.h"

#include <QImage>

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
/// - hardware frames are downloaded (av_hwframe_transfer_data) into NV12/P010 buffers taken
///   from a pool, so steady-state playback allocates no pixel memory;
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

    /// Wraps/downloads/converts `decoded`. @return nullptr on failure (a failed hardware
    /// download means the caller should fall back to software decoding).
    std::shared_ptr<const VideoFrame> make(const AVFrame* decoded, const FrameMeta& meta);

    /// Downloads a hardware frame into a pooled software frame (NV12/P010...).
    /// @return nullptr on failure.
    FramePtr download(const AVFrame* hwFrame);

    /// Wraps a premultiplied ARGB32 image (stills) as a Bgra frame sharing its pixels.
    static std::shared_ptr<const VideoFrame> wrapImage(const QImage& image, const FrameMeta& meta);

private:
    AVBufferPool* m_pool = nullptr;
    size_t m_poolBufferSize = 0;
    FrameConverter m_converter; ///< for formats outside PixelFormat → Bgra
};

} // namespace kk::media::detail
