// SPDX-License-Identifier: GPL-3.0-or-later
// VideoFrame — plane geometry helpers + VideoFrameFactory (decoder output → VideoFrame).
#include "media/VideoFrame.h"

#include "media/MediaLog.h"
#include "media/VideoFrameFactory.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <optional>

namespace cw::media {

namespace {

/// Row alignment of pooled frames (also suits GL_UNPACK_ALIGNMENT / SIMD).
constexpr int kPlaneAlignment = 64;

/// Frames at least this tall are assumed to be BT.709 when the file does not say.
constexpr int kAssumeBt709MinHeight = 720;

bool isHalfWidthChroma(PixelFormat format)
{
    switch (format) {
    case PixelFormat::Nv12:
    case PixelFormat::P010:
    case PixelFormat::Yuv420p:
    case PixelFormat::Yuv422p:
    case PixelFormat::Yuv420p10:
    case PixelFormat::Yuv422p10:
        return true;
    default:
        return false;
    }
}

bool isHalfHeightChroma(PixelFormat format)
{
    return format == PixelFormat::Nv12 || format == PixelFormat::P010
        || format == PixelFormat::Yuv420p || format == PixelFormat::Yuv420p10;
}

/// PixelFormat for an FFmpeg software format, if it is one we pass through unchanged.
/// `fullRange` is set for the deprecated full-range "yuvj" formats.
std::optional<PixelFormat> mapPixelFormat(int format, bool* fullRange)
{
    switch (AVPixelFormat(format)) {
    case AV_PIX_FMT_YUVJ420P:
        *fullRange = true;
        [[fallthrough]];
    case AV_PIX_FMT_YUV420P:
        return PixelFormat::Yuv420p;
    case AV_PIX_FMT_YUVJ422P:
        *fullRange = true;
        [[fallthrough]];
    case AV_PIX_FMT_YUV422P:
        return PixelFormat::Yuv422p;
    case AV_PIX_FMT_YUVJ444P:
        *fullRange = true;
        [[fallthrough]];
    case AV_PIX_FMT_YUV444P:
        return PixelFormat::Yuv444p;
    case AV_PIX_FMT_YUV420P10LE:
        return PixelFormat::Yuv420p10;
    case AV_PIX_FMT_YUV422P10LE:
        return PixelFormat::Yuv422p10;
    case AV_PIX_FMT_YUV444P10LE:
        return PixelFormat::Yuv444p10;
    case AV_PIX_FMT_NV12:
        return PixelFormat::Nv12;
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_P016LE: // NVDEC's 10-bit output: same MSB-aligned layout as P010
        return PixelFormat::P010;
    default:
        return std::nullopt;
    }
}

ColorMatrix colorMatrix(const AVFrame* frame)
{
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
        return ColorMatrix::Bt709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return ColorMatrix::Bt601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return ColorMatrix::Bt2020;
    default:
        return frame->height >= kAssumeBt709MinHeight ? ColorMatrix::Bt709 : ColorMatrix::Bt601;
    }
}

ColorTransfer colorTransfer(const AVFrame* frame)
{
    switch (frame->color_trc) {
    case AVCOL_TRC_SMPTE2084:
        return ColorTransfer::Pq;
    case AVCOL_TRC_ARIB_STD_B67:
        return ColorTransfer::Hlg;
    default:
        return ColorTransfer::Sdr;
    }
}

void applyMeta(VideoFrame& frame, const detail::FrameMeta& meta)
{
    frame.ptsUs = meta.ptsUs;
    frame.durationUs = meta.durationUs;
    frame.sampleAspectRatio = meta.sampleAspectRatio;
    frame.rotation = meta.rotation;
}

} // namespace

int VideoFrame::planeWidth(int plane) const
{
    return plane > 0 && isHalfWidthChroma(format) ? (width + 1) / 2 : width;
}

int VideoFrame::planeHeight(int plane) const
{
    return plane > 0 && isHalfHeightChroma(format) ? (height + 1) / 2 : height;
}

int VideoFrame::bytesPerPixel(int plane) const
{
    switch (format) {
    case PixelFormat::Nv12:
        return plane == 0 ? 1 : 2;
    case PixelFormat::P010:
        return plane == 0 ? 2 : 4;
    case PixelFormat::Yuv420p10:
    case PixelFormat::Yuv422p10:
    case PixelFormat::Yuv444p10:
        return 2;
    case PixelFormat::Bgra:
        return 4;
    default:
        return 1;
    }
}

const char* pixelFormatName(PixelFormat format)
{
    switch (format) {
    case PixelFormat::Nv12:
        return "nv12";
    case PixelFormat::P010:
        return "p010";
    case PixelFormat::Yuv420p:
        return "yuv420p";
    case PixelFormat::Yuv422p:
        return "yuv422p";
    case PixelFormat::Yuv444p:
        return "yuv444p";
    case PixelFormat::Yuv420p10:
        return "yuv420p10";
    case PixelFormat::Yuv422p10:
        return "yuv422p10";
    case PixelFormat::Yuv444p10:
        return "yuv444p10";
    case PixelFormat::Bgra:
        return "bgra";
    }
    return "?";
}

// ---- VideoFrameFactory ----------------------------------------------------------------------

namespace detail {

VideoFrameFactory::~VideoFrameFactory()
{
    av_buffer_pool_uninit(&m_pool); // buffers still held by frames are freed when released
}

FramePtr VideoFrameFactory::download(const AVFrame* hwFrame)
{
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(hwFrame->hw_frames_ctx->data);
    const AVPixelFormat format = frames->sw_format;
    const int size
        = av_image_get_buffer_size(format, hwFrame->width, hwFrame->height, kPlaneAlignment);
    if (size <= 0) {
        return nullptr;
    }
    // One pool per frame size; re-created when the stream changes resolution/format.
    if (!m_pool || m_poolBufferSize != size_t(size)) {
        av_buffer_pool_uninit(&m_pool);
        m_pool = av_buffer_pool_init(size_t(size), nullptr);
        m_poolBufferSize = size_t(size);
    }
    FramePtr out(av_frame_alloc());
    if (!m_pool || !out) {
        return nullptr;
    }
    out->buf[0] = av_buffer_pool_get(m_pool);
    if (!out->buf[0]) {
        return nullptr;
    }
    out->format = format;
    out->width = hwFrame->width;
    out->height = hwFrame->height;
    av_image_fill_arrays(out->data, out->linesize, out->buf[0]->data, format, out->width,
        out->height, kPlaneAlignment);
    int ret = av_hwframe_transfer_data(out.get(), hwFrame, 0);
    if (ret >= 0) {
        ret = av_frame_copy_props(out.get(), hwFrame);
    }
    if (ret < 0) {
        qCWarning(lcMedia) << "hardware frame download failed:" << avErrorString(ret);
        return nullptr;
    }
    return out;
}

std::shared_ptr<const VideoFrame> VideoFrameFactory::make(
    const AVFrame* decoded, const FrameMeta& meta)
{
    FramePtr owned(decoded->hw_frames_ctx ? download(decoded).release() : av_frame_clone(decoded));
    if (!owned) {
        return nullptr;
    }
    bool fullRange = owned->color_range == AVCOL_RANGE_JPEG;
    const std::optional<PixelFormat> format = mapPixelFormat(owned->format, &fullRange);
    if (!format) {
        // Rare layouts (alpha, 12-bit, RGB codecs...): convert once to premultiplied BGRA.
        const QImage image = m_converter.convert(owned.get(), QSize(owned->width, owned->height));
        return image.isNull() ? nullptr : wrapImage(image, meta);
    }
    auto frame = std::make_shared<VideoFrame>();
    frame->format = *format;
    frame->width = owned->width;
    frame->height = owned->height;
    frame->planeCount = av_pix_fmt_count_planes(AVPixelFormat(owned->format));
    for (int plane = 0; plane < frame->planeCount && plane < 4; ++plane) {
        frame->data[size_t(plane)] = owned->data[plane];
        frame->linesize[size_t(plane)] = owned->linesize[plane];
    }
    frame->matrix = colorMatrix(owned.get());
    frame->range = fullRange ? ColorRange::Full : ColorRange::Limited;
    frame->transfer = colorTransfer(owned.get());
    applyMeta(*frame, meta);
    std::shared_ptr<AVFrame> storage(owned.release(), [](AVFrame* f) { av_frame_free(&f); });
    frame->avFrame = storage.get();
    frame->storage = std::move(storage);
    return frame;
}

std::shared_ptr<const VideoFrame> VideoFrameFactory::wrapImage(
    const QImage& image, const FrameMeta& meta)
{
    auto storage = std::make_shared<QImage>(image.format() == QImage::Format_ARGB32_Premultiplied
            ? image
            : image.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    auto frame = std::make_shared<VideoFrame>();
    frame->format = PixelFormat::Bgra;
    frame->width = storage->width();
    frame->height = storage->height();
    frame->planeCount = 1;
    frame->data[0] = storage->constBits(); // const access: never detaches the shared image
    frame->linesize[0] = int(storage->bytesPerLine());
    frame->range = ColorRange::Full;
    applyMeta(*frame, meta);
    frame->storage = std::move(storage);
    return frame;
}

} // namespace detail

} // namespace cw::media
