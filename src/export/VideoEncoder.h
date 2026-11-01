// SPDX-License-Identifier: GPL-3.0-or-later
// VideoEncoder — internal: one libavcodec video encoder plus the conversion of compositor
// frames (QImage ARGB32 premultiplied) into the encoder's pixel format (swscale with slice
// threads, an own float path for EXR, a palettegen/paletteuse graph for GIF, upload to VAAPI
// surfaces for VAAPI encoders).
#pragma once

#include "core/Rational.h"
#include "export/ExportFFmpeg.h"
#include "export/ExportSettings.h"

#include <QImage>
#include <QSize>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

class QThreadPool;

namespace kk::exporter {

/// Everything needed to open a VideoEncoder.
struct VideoEncoderConfig
{
    QString encoderName; ///< libavcodec encoder ("libx264", "h264_vaapi", "prores_ks", ...)
    EncoderBackend backend = EncoderBackend::Software; ///< backend of encoderName
    VideoExportSettings settings; ///< codec id, profile, quality, bitrate, speed
    QSize size; ///< frame size in pixels (images passed to encode() must match)
    Rational fps {25, 1}; ///< timeline frame rate = encoder time base 1/fps
    bool alpha = false; ///< keep the alpha channel (alpha-capable pixel format)
    bool globalHeader = false; ///< the muxer wants extradata (AVFMT_GLOBALHEADER)
    AVBufferRef* hwDevice = nullptr; ///< VAAPI device context (referenced, not taken over)
    bool lowPower = false; ///< VAAPI: request the low-power (VDEnc) entry point
};

/// One open video encoder.
///
/// Pixel conversion on the CPU (the Compositor API only hands out RGBA textures/images, so a
/// GPU RGB → YUV pass would need a second readback path in src/render; see readme.md):
/// * 8-bit 4:2:0 BT.709 (I420/NV12 — every delivery codec, hardware encoders included):
///   own fixed-point converter (RgbToYuv.h) run in row bands on a small private thread pool;
///   swscale's RGB → YUV path was 6-10 ms per 1080p frame on the dev box, this is < 1 ms.
/// * everything else (10-bit 4:2:2/4:4:4, RGB, JPEG range): swscale with slice threads.
/// * EXR: own sRGB → linear float loop; GIF: palettegen/paletteuse filter graph.
///
/// Thread-safety: not thread-safe; use from one thread.
class VideoEncoder
{
public:
    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    /// Opens the encoder. @return false with a message in @p error.
    [[nodiscard]] bool open(const VideoEncoderConfig& config, QString* error);
    /// The opened codec context (stream parameters for the muxer), or nullptr.
    [[nodiscard]] AVCodecContext* context() const { return m_ctx.get(); }

    /// Encodes @p image (ARGB32_Premultiplied, config size) as frame number @p pts (time base
    /// 1/fps). Encoded packets are handed to @p sink. @return false on errors.
    [[nodiscard]] bool encode(
        const QImage& image, int64_t pts, const PacketSink& sink, QString* error);
    /// Drains the encoder (and for GIF: computes the palette and encodes every frame).
    [[nodiscard]] bool flush(const PacketSink& sink, QString* error);

    /// Milliseconds spent in pixel conversion so far (statistics).
    [[nodiscard]] double convertMs() const { return m_convertMs; }
    /// Milliseconds spent inside the encoder so far (statistics).
    [[nodiscard]] double encodeMs() const { return m_encodeMs; }

private:
    bool openGifGraph(QString* error);
    bool convert(const QImage& image);
    void convertExr(const QImage& image);
    bool sendFrame(AVFrame* frame, const PacketSink& sink, QString* error);
    bool drainGif(const PacketSink& sink, QString* error);

    VideoEncoderConfig m_config;
    CodecContextPtr m_ctx;
    AVPixelFormat m_swFormat = AV_PIX_FMT_NONE; ///< format of m_frame (sw side)
    FramePtr m_frame; ///< converted frame (reused)
    FramePtr m_hwFrame; ///< VAAPI surface (re-acquired per frame)
    FramePtr m_filtered; ///< GIF: palettised frames from the filter graph
    BufferRefPtr m_hwFrames; ///< VAAPI frames context
    SwsPtr m_sws;
    FilterGraphPtr m_graph; ///< GIF only
    AVFilterContext* m_bufferSrc = nullptr;
    AVFilterContext* m_bufferSink = nullptr;
    PacketPtr m_packet;
    bool m_fastYuv = false; ///< use rgb32ToYuv420() instead of swscale
    std::unique_ptr<QThreadPool> m_pool; ///< conversion bands (fast path)
    std::vector<float> m_srgbToLinear; ///< EXR: 256-entry sRGB → linear table
    QImage m_straight; ///< alpha outputs: un-premultiplied copy of the input (reused)
    double m_convertMs = 0.0;
    double m_encodeMs = 0.0;
};

} // namespace kk::exporter
