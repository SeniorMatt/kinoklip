// SPDX-License-Identifier: GPL-3.0-or-later
// FrameConverter — libswscale based AVFrame → QImage conversion (scaling + colour conversion).
#include "media/FrameConverter.h"

#include "media/HdrToSdr.h"
#include "media/MediaLog.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
}

namespace kk::media::detail {

namespace {

/// Row alignment of the images we allocate; lets swscale use its fastest SIMD paths.
constexpr int kRowAlignmentBytes = 64;

/// Frames at least this tall are assumed to be BT.709 when the file does not say.
constexpr int kAssumeBt709MinHeight = 720;

/// Packed 8-bit BGRA in memory == QImage::Format_ARGB32 on little-endian machines.
constexpr AVPixelFormat kQtArgb32PixelFormat
    = Q_BYTE_ORDER == Q_LITTLE_ENDIAN ? AV_PIX_FMT_BGRA : AV_PIX_FMT_ARGB;

/// Maps the deprecated full-range "yuvj" formats to their plain equivalent (+ full range),
/// which avoids swscale warnings and lets us pass the range explicitly.
AVPixelFormat normalizedFormat(AVPixelFormat format, bool* fullRange)
{
    switch (format) {
    case AV_PIX_FMT_YUVJ420P:
        *fullRange = true;
        return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P:
        *fullRange = true;
        return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P:
        *fullRange = true;
        return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P:
        *fullRange = true;
        return AV_PIX_FMT_YUV440P;
    case AV_PIX_FMT_YUVJ411P:
        *fullRange = true;
        return AV_PIX_FMT_YUV411P;
    default:
        return format;
    }
}

/// libswscale colour-space constant (SWS_CS_*) for a frame.
int swsColorspace(const AVFrame* frame)
{
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
        return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return SWS_CS_ITU601;
    case AVCOL_SPC_SMPTE240M:
        return SWS_CS_SMPTE240M;
    case AVCOL_SPC_FCC:
        return SWS_CS_FCC;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return SWS_CS_BT2020;
    default:
        // Untagged: HD and larger is practically always BT.709, SD is BT.601.
        return frame->height >= kAssumeBt709MinHeight ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

/// Allocates an uninitialised QImage whose rows are aligned for SIMD; the buffer is freed with
/// av_free() when the last QImage copy referencing it goes away.
QImage allocateAlignedImage(QSize size, QImage::Format format)
{
    const qsizetype stride = FFALIGN(qsizetype(size.width()) * 4, kRowAlignmentBytes);
    auto* buffer = static_cast<uchar*>(av_malloc(size_t(stride) * size_t(size.height())));
    if (!buffer) {
        return {};
    }
    return QImage(buffer, size.width(), size.height(), stride, format, av_free, buffer);
}

} // namespace

QImage FrameConverter::convert(const AVFrame* frame, QSize outSize)
{
    if (!frame || frame->width <= 0 || frame->height <= 0 || outSize.isEmpty()) {
        return {};
    }
    bool fullRange = frame->color_range == AVCOL_RANGE_JPEG;
    const AVPixelFormat srcFormat = normalizedFormat(AVPixelFormat(frame->format), &fullRange);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(srcFormat);
    if (!desc) {
        return {};
    }
    const bool srcIsRgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    const bool hasAlpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    const int colorspace = swsColorspace(frame);

    SwsContext* sws = sws_getCachedContext(m_sws.release(), frame->width, frame->height, srcFormat,
        outSize.width(), outSize.height(), kQtArgb32PixelFormat, SWS_BICUBIC, nullptr, nullptr,
        nullptr);
    m_sws.reset(sws);
    if (!sws) {
        qCWarning(
            lcMedia, "swscale cannot convert %s %dx%d", desc->name, frame->width, frame->height);
        return {};
    }
    const bool reconfigured = frame->width != m_srcWidth || frame->height != m_srcHeight
        || int(srcFormat) != m_srcFormat || outSize != m_dstSize;
    if (!srcIsRgb && (reconfigured || colorspace != m_colorspace || fullRange != m_fullRange)) {
        // YUV → RGB matrix and input range; output is always full-range RGB.
        const int* coefficients = sws_getCoefficients(colorspace);
        sws_setColorspaceDetails(sws, coefficients, fullRange ? 1 : 0,
            sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    }
    m_srcWidth = frame->width;
    m_srcHeight = frame->height;
    m_srcFormat = int(srcFormat);
    m_dstSize = outSize;
    m_colorspace = colorspace;
    m_fullRange = fullRange;

    // swscale writes straight (non-premultiplied) alpha. For opaque sources alpha is 255
    // everywhere, which is already valid premultiplied data, so no extra pass is needed.
    QImage image = allocateAlignedImage(
        outSize, hasAlpha ? QImage::Format_ARGB32 : QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return {};
    }
    uint8_t* dstData[4] = {image.bits(), nullptr, nullptr, nullptr};
    int dstLinesize[4] = {int(image.bytesPerLine()), 0, 0, 0};
    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize);
    if (hasAlpha) {
        image.convertTo(QImage::Format_ARGB32_Premultiplied);
    } else if (!srcIsRgb) {
        // HLG / PQ / BT.2020 → SDR BT.709, as the GPU shader does (media/HdrToSdr.h).
        mapImageToSdr(&image, hdrMappingOf(frame));
    }
    return image;
}

} // namespace kk::media::detail
