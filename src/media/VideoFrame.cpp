// SPDX-License-Identifier: GPL-3.0-or-later
// VideoFrame — plane geometry helpers + VideoFrameFactory (decoder output → VideoFrame).
#include "media/VideoFrame.h"

#include "media/DecodeStats.h"
#include "media/HwDevices.h"
#include "media/MediaLog.h"
#include "media/VideoFrameFactory.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <QElapsedTimer>

#include <optional>

namespace kk::media {

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

constexpr uint32_t fourcc(char a, char b, char c, char d)
{
    return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
}
constexpr uint32_t kDrmR8 = fourcc('R', '8', ' ', ' ');
constexpr uint32_t kDrmGr88 = fourcc('G', 'R', '8', '8');
constexpr uint32_t kDrmR16 = fourcc('R', '1', '6', ' ');
constexpr uint32_t kDrmGr1616 = fourcc('G', 'R', '3', '2');

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

int VideoFrameFactory::copyMapped(AVFrame* dst, const AVFrame* hwFrame)
{
    // av_hwframe_transfer_data() maps the VA surface and copies it with plain loads. The
    // mapping is write-combining (uncached) memory on Intel/AMD, where plain loads crawl
    // (~250 MB/s: 1080p NV12 took ~11 ms on a UHD 630). av_image_copy_uc_from() uses SSE4.1
    // streaming loads made for exactly this (several GB/s); it is a plain copy elsewhere.
    // KINOKLIP_VAAPI_COPY=transfer: always use av_hwframe_transfer_data (diagnostics).
    //
    // FFmpeg contract (hwcontext_vaapi.c, vaapi_map_frame): av_hwframe_map() with
    // AV_HWFRAME_MAP_READ first calls vaSyncSurface() (waits until the GPU finished decoding
    // into the surface; "Failed to sync surface" + AVERROR(EIO) otherwise), then - READ
    // without DIRECT - copies the surface into a VAImage (vaCreateImage + vaGetImage) and maps
    // that. `mapped` holds a reference to the hardware frame until it is freed below, so the
    // surface cannot return to the decoder's pool while we copy; the caller (VideoDecoder)
    // holds the device's call lock (HwDeviceManager::callLock) around the whole download.
    static const bool disabled
        = qEnvironmentVariable("KINOKLIP_VAAPI_COPY") == QLatin1String("transfer");
    if (m_mapFailed || disabled) {
        return AVERROR(ENOSYS);
    }
    FramePtr mapped(av_frame_alloc());
    if (!mapped) {
        return AVERROR(ENOMEM);
    }
    mapped->format = dst->format;
    const int ret = av_hwframe_map(mapped.get(), hwFrame, AV_HWFRAME_MAP_READ);
    if (ret == AVERROR(EIO)) {
        // The surface itself is bad (vaSyncSurface failed): av_hwframe_transfer_data() would
        // sync the same surface again and fail the same way. Report it; keep mapping enabled.
        return ret;
    }
    if (ret < 0 || mapped->format != dst->format || mapped->width < dst->width
        || mapped->height < dst->height) {
        qCDebug(lcMedia) << "hardware frame mapping unavailable (" << avErrorString(ret)
                         << "); using av_hwframe_transfer_data";
        m_mapFailed = true; // do not retry every frame
        return AVERROR(ENOSYS);
    }
    ptrdiff_t dstLines[4] = {};
    ptrdiff_t srcLines[4] = {};
    for (int i = 0; i < 4; ++i) {
        dstLines[i] = dst->linesize[i];
        srcLines[i] = mapped->linesize[i];
    }
    av_image_copy_uc_from(dst->data, dstLines, const_cast<const uint8_t**>(mapped->data), srcLines,
        AVPixelFormat(dst->format), dst->width, dst->height);
    return 0;
}

FramePtr VideoFrameFactory::download(const AVFrame* hwFrame)
{
    m_lastError = 0;
    m_lastErrorText.clear();
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(hwFrame->hw_frames_ctx->data);
    const AVPixelFormat format = frames->sw_format;
    const int size
        = av_image_get_buffer_size(format, hwFrame->width, hwFrame->height, kPlaneAlignment);
    if (size <= 0) {
        m_lastError = size < 0 ? size : AVERROR(EINVAL);
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
        m_lastError = AVERROR(ENOMEM);
        return nullptr;
    }
    out->buf[0] = av_buffer_pool_get(m_pool);
    if (!out->buf[0]) {
        m_lastError = AVERROR(ENOMEM);
        return nullptr;
    }
    out->format = format;
    out->width = hwFrame->width;
    out->height = hwFrame->height;
    av_image_fill_arrays(out->data, out->linesize, out->buf[0]->data, format, out->width,
        out->height, kPlaneAlignment);
    if (takeInjectedDownloadFailure()) {
        m_lastError = AVERROR(EIO);
        m_lastErrorText = QStringLiteral("injected download failure (test)");
        return nullptr;
    }
    // A failing download is handled by the decoder (retry, then software): capture FFmpeg's
    // messages ("Failed to sync surface ...") as the reason instead of logging every one.
    FfmpegLogCapture capture;
    int ret = AVERROR(ENOSYS);
    if (frames->device_ctx->type == AV_HWDEVICE_TYPE_VAAPI) {
        ret = copyMapped(out.get(), hwFrame);
    }
    if (ret == AVERROR(ENOSYS)) {
        ret = av_hwframe_transfer_data(out.get(), hwFrame, 0);
    }
    if (ret >= 0) {
        ret = av_frame_copy_props(out.get(), hwFrame);
    }
    if (ret < 0) {
        m_lastError = ret;
        m_lastErrorText = capture.firstError();
        qCDebug(lcMedia).noquote()
            << "hardware frame download failed:" << avErrorString(ret) << m_lastErrorText;
        return nullptr;
    }
    return out;
}

QString VideoFrameFactory::lastErrorString() const
{
    if (m_lastError == 0) {
        return {};
    }
    QString text = avErrorString(m_lastError);
    if (!m_lastErrorText.isEmpty()) {
        text += QStringLiteral(" (%1)").arg(m_lastErrorText);
    }
    return text;
}

std::shared_ptr<const VideoFrame> VideoFrameFactory::makeDmaBuf(
    const AVFrame* hwFrame, const FrameMeta& meta)
{
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(hwFrame->hw_frames_ctx->data);
    const bool is10 = frames->sw_format == AV_PIX_FMT_P010LE;
    if (frames->device_ctx->type != AV_HWDEVICE_TYPE_VAAPI
        || (frames->sw_format != AV_PIX_FMT_NV12 && !is10)) {
        m_exportFailed = true; // 8/10-bit 4:2:0 only: the shader reads exactly these layouts
        qCDebug(lcMedia) << "zero-copy: surface format" << av_get_pix_fmt_name(frames->sw_format)
                         << "is not exported; downloading";
        return nullptr;
    }
    m_lastError = 0;
    m_lastErrorText.clear();
    if (takeInjectedDownloadFailure()) {
        m_lastError = AVERROR(EIO); // test hook: the same failure the download would have
        m_lastErrorText = QStringLiteral("injected download failure (test)");
        return nullptr;
    }
    QElapsedTimer timer;
    timer.start();
    FfmpegLogCapture capture;
    FramePtr drm(av_frame_alloc());
    if (!drm) {
        return nullptr;
    }
    drm->format = AV_PIX_FMT_DRM_PRIME;
    // Syncs the surface (vaSyncSurface: waits for the decoder), then vaExportSurfaceHandle:
    // separate layers, one per plane. FFmpeg keeps a reference to the surface in `drm`.
    const int ret = av_hwframe_map(drm.get(), hwFrame, AV_HWFRAME_MAP_READ);
    if (ret == AVERROR(EIO)) {
        m_lastError = ret;
        m_lastErrorText = capture.firstError();
        return nullptr; // the surface is bad; the caller's download reports it the usual way
    }
    const auto* desc
        = ret >= 0 ? reinterpret_cast<const AVDRMFrameDescriptor*>(drm->data[0]) : nullptr;
    auto fail = [&](const QString& why) {
        m_exportFailed = true;
        qCDebug(lcMedia).noquote()
            << "zero-copy: DMA-BUF export unavailable (" << why << "); downloading frames";
        return std::shared_ptr<const VideoFrame>();
    };
    if (!desc) {
        return fail(avErrorString(ret) + QStringLiteral(" ") + capture.firstError());
    }
    // Flatten layers -> planes: separate layers (one plane each) or one layer with two planes.
    auto dmabuf = std::make_shared<DmaBufFrame>();
    const uint32_t formats[2] = {is10 ? kDrmR16 : kDrmR8, is10 ? kDrmGr1616 : kDrmGr88};
    for (int l = 0; l < desc->nb_layers; ++l) {
        for (int p = 0; p < desc->layers[l].nb_planes; ++p) {
            const AVDRMPlaneDescriptor& plane = desc->layers[l].planes[p];
            if (dmabuf->planeCount >= 2 || plane.object_index < 0
                || plane.object_index >= desc->nb_objects) {
                return fail(QStringLiteral("unexpected layout"));
            }
            const AVDRMObjectDescriptor& object = desc->objects[plane.object_index];
            DmaBufFrame::Plane& out = dmabuf->planes[size_t(dmabuf->planeCount)];
            out.drmFormat = formats[dmabuf->planeCount];
            out.fd = object.fd;
            out.offset = uint32_t(plane.offset);
            out.pitch = uint32_t(plane.pitch);
            out.modifier = object.format_modifier;
            out.width = dmabuf->planeCount == 0 ? hwFrame->width : (hwFrame->width + 1) / 2;
            out.height = dmabuf->planeCount == 0 ? hwFrame->height : (hwFrame->height + 1) / 2;
            ++dmabuf->planeCount;
        }
    }
    if (dmabuf->planeCount != 2) {
        return fail(QStringLiteral("%1 planes").arg(dmabuf->planeCount));
    }
    std::shared_ptr<AVFrame> owner(drm.release(), [](AVFrame* f) { av_frame_free(&f); });
    dmabuf->owner = owner;
    // Fallback for a GPU that cannot import the buffers: the plain download of the same surface.
    std::shared_ptr<AVFrame> hwRef(av_frame_clone(hwFrame), [](AVFrame* f) { av_frame_free(&f); });
    dmabuf->download = [hwRef, meta]() -> std::shared_ptr<const VideoFrame> {
        std::mutex* hwLock = HwDeviceManager::instance().callLock(HwAccelMode::Vaapi);
        const auto lock
            = hwLock ? std::unique_lock<std::mutex>(*hwLock) : std::unique_lock<std::mutex>();
        VideoFrameFactory plain;
        return plain.make(hwRef.get(), meta);
    };

    auto frame = std::make_shared<VideoFrame>();
    frame->format = is10 ? PixelFormat::P010 : PixelFormat::Nv12;
    frame->width = hwFrame->width;
    frame->height = hwFrame->height;
    frame->planeCount = 2;
    frame->matrix = colorMatrix(hwFrame);
    frame->range
        = hwFrame->color_range == AVCOL_RANGE_JPEG ? ColorRange::Full : ColorRange::Limited;
    frame->transfer = colorTransfer(hwFrame);
    applyMeta(*frame, meta);
    // Colour primaries (BT.2020 SDR detection) are read from the hardware frame's properties;
    // it holds no pixels the CPU can read.
    frame->avFrame = hwRef.get();
    frame->dmabuf = std::move(dmabuf);
    frame->storage = std::move(owner);
    countZeroCopyFrame(timer.nsecsElapsed() / 1000);
    return frame;
}

std::shared_ptr<const VideoFrame> VideoFrameFactory::make(
    const AVFrame* decoded, const FrameMeta& meta)
{
    if (decoded->hw_frames_ctx && zeroCopy()) {
        if (auto exported = makeDmaBuf(decoded, meta)) {
            return exported;
        }
        if (!m_exportFailed && m_lastError != 0) {
            return nullptr; // a bad surface: the caller handles it like a failed download
        }
    }
    QElapsedTimer timer;
    timer.start();
    const bool hardware = decoded->hw_frames_ctx != nullptr;
    FramePtr owned(hardware ? download(decoded).release() : av_frame_clone(decoded));
    if (!owned) {
        return nullptr;
    }
    if (hardware) {
        countDownloadedFrame(timer.nsecsElapsed() / 1000);
    } else {
        countSoftwareFrame();
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

} // namespace kk::media
