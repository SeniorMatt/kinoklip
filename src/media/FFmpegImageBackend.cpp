// SPDX-License-Identifier: GPL-3.0-or-later
// FFmpegImageBackend — stills that only libavcodec decodes (JPEG XL, OpenEXR, DPX, PSD, QOI,
// PCX, SGI, JPEG 2000...): first frame of the file, converted with swscale (planar float EXR:
// own linear → sRGB lookup-table conversion).
#include "media/FFmpegUtil.h"
#include "media/FrameConverter.h"
#include "media/ImageBackends.h"
#include "media/MediaLog.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace kk::media::detail {

namespace {

/// Packets read before giving up on getting a frame (multi-packet images, damaged files).
constexpr int kMaxPacketsForStill = 64;

/// True if FFmpeg's demuxer for this file reads single images (image2 / *_pipe).
bool isImageDemuxer(const AVFormatContext* fmt)
{
    const char* name = fmt->iformat->name;
    const size_t length = strlen(name);
    return strcmp(name, "image2") == 0 || (length > 5 && strcmp(name + length - 5, "_pipe") == 0);
}

/// Opens a decoder for a still (all cores: big EXR/JPEG 2000 frames decode slice-parallel).
CodecContextPtr openStillDecoder(const AVStream* stream, QString* error)
{
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        *error = QStringLiteral("No decoder for '%1'")
                     .arg(QString::fromUtf8(avcodec_get_name(stream->codecpar->codec_id)));
        return {};
    }
    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx || avcodec_parameters_to_context(ctx.get(), stream->codecpar) < 0) {
        *error = QStringLiteral("Cannot set up decoder");
        return {};
    }
    ctx->thread_count = 0;
    const int ret = avcodec_open2(ctx.get(), codec, nullptr);
    if (ret < 0) {
        *error = QStringLiteral("Cannot open decoder (%1)").arg(avErrorString(ret));
        return {};
    }
    return ctx;
}

// ---- Linear float → 8-bit sRGB (OpenEXR) ----
//
// EXR holds scene-linear light as planar float (gbrpf32 / gbrapf32, associated alpha). Showing it
// needs the sRGB transfer curve; the decoder's own `apply_trc` option evaluates pow() per sample
// (~250 ms for a 1080p frame), while this table lookup takes a few ms. Values are clamped to
// 0..1 (no tone mapping yet). The table has kSrgbLutSize entries over linear 0..1: the steepest
// part of the curve (the linear toe, slope 12.92) then advances < 1 output level per entry.

constexpr int kSrgbLutSize = 16384;

const std::array<uint8_t, kSrgbLutSize>& srgbLut()
{
    static const std::array<uint8_t, kSrgbLutSize> kLut = [] {
        std::array<uint8_t, kSrgbLutSize> lut {};
        for (int i = 0; i < kSrgbLutSize; ++i) {
            const double linear = double(i) / (kSrgbLutSize - 1);
            const double encoded = linear <= 0.0031308
                ? 12.92 * linear
                : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
            lut[size_t(i)] = uint8_t(std::lround(std::clamp(encoded, 0.0, 1.0) * 255.0));
        }
        return lut;
    }();
    return kLut;
}

/// Index into srgbLut() for a linear value (NaN and negatives → 0, > 1 → 1).
inline int lutIndex(float linear)
{
    // The comparison form also maps NaN to 0.
    if (!(linear > 0.0f)) {
        return 0;
    }
    return linear >= 1.0f ? kSrgbLutSize - 1 : int(linear * (kSrgbLutSize - 1) + 0.5f);
}

/// True for the planar float formats convertLinearFloat() handles.
bool isPlanarFloat(int format)
{
    return format == AV_PIX_FMT_GBRPF32LE || format == AV_PIX_FMT_GBRAPF32LE;
}

/// gbr(a)pf32le frame (linear, associated alpha) → premultiplied ARGB32 in sRGB.
QImage convertLinearFloat(const AVFrame* frame)
{
    QImage image(frame->width, frame->height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return image;
    }
    const auto& lut = srgbLut();
    const bool alpha = frame->format == AV_PIX_FMT_GBRAPF32LE;
    for (int y = 0; y < frame->height; ++y) {
        // Plane order of FFmpeg's GBR formats: 0 = G, 1 = B, 2 = R, 3 = A.
        const auto* g = reinterpret_cast<const float*>(frame->data[0] + y * frame->linesize[0]);
        const auto* b = reinterpret_cast<const float*>(frame->data[1] + y * frame->linesize[1]);
        const auto* r = reinterpret_cast<const float*>(frame->data[2] + y * frame->linesize[2]);
        const auto* a = alpha
            ? reinterpret_cast<const float*>(frame->data[3] + y * frame->linesize[3])
            : nullptr;
        auto* out = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < frame->width; ++x) {
            if (!a) {
                out[x] = qRgb(lut[size_t(lutIndex(r[x]))], lut[size_t(lutIndex(g[x]))],
                    lut[size_t(lutIndex(b[x]))]);
                continue;
            }
            // Un-premultiply in linear light, encode, re-premultiply in 8 bit.
            const float av = std::clamp(a[x], 0.0f, 1.0f);
            if (av <= 0.0f) {
                out[x] = 0;
                continue;
            }
            const float inv = 1.0f / av;
            const int a8 = int(av * 255.0f + 0.5f);
            const auto premul = [a8](int c) { return (c * a8 + 127) / 255; };
            out[x] = qRgba(premul(lut[size_t(lutIndex(r[x] * inv))]),
                premul(lut[size_t(lutIndex(g[x] * inv))]),
                premul(lut[size_t(lutIndex(b[x] * inv))]), a8);
        }
    }
    return image;
}

} // namespace

bool readHeaderFFmpeg(const QString& path, ImageHeader& header)
{
    QString error;
    const FormatContextPtr fmt = openInput(path, &error);
    if (!fmt) {
        header.error = error;
        return false;
    }
    const int index = findVideoStream(fmt.get());
    if (index < 0) {
        header.error = QStringLiteral("No image stream");
        return false;
    }
    const AVStream* stream = fmt->streams[index];
    const AVCodecParameters* par = stream->codecpar;
    header.valid = par->width > 0 && par->height > 0;
    header.size = {par->width, par->height};
    header.format = QString::fromUtf8(avcodec_get_name(par->codec_id));
    if (const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(par->format))) {
        header.pixelFormat = QString::fromUtf8(desc->name);
        header.hasAlpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    }
    header.frameCount
        = isImageDemuxer(fmt.get()) ? 1 : int(std::max<int64_t>(0, stream->nb_frames));
    header.animated = header.frameCount > 1;
    header.backend = ImageBackend::FFmpeg;
    return header.valid;
}

bool loadFFmpeg(const QString& path, LoadedImage& result)
{
    const FormatContextPtr fmt = openInput(path, &result.error);
    if (!fmt) {
        return false;
    }
    const int index = findVideoStream(fmt.get());
    if (index < 0) {
        result.error = QStringLiteral("No image stream");
        return false;
    }
    const CodecContextPtr codec = openStillDecoder(fmt->streams[index], &result.error);
    if (!codec) {
        return false;
    }
    const PacketPtr packet(av_packet_alloc());
    const FramePtr frame(av_frame_alloc());
    if (!packet || !frame) {
        result.error = QStringLiteral("Out of memory");
        return false;
    }
    bool gotFrame = false;
    bool flushed = false;
    for (int packets = 0; !gotFrame && packets < kMaxPacketsForStill;) {
        const int ret = avcodec_receive_frame(codec.get(), frame.get());
        if (ret == 0) {
            gotFrame = true;
            break;
        }
        if (ret != AVERROR(EAGAIN) || flushed) {
            break; // EOF or decoding error
        }
        if (av_read_frame(fmt.get(), packet.get()) < 0) {
            avcodec_send_packet(codec.get(), nullptr); // drain delayed frames
            flushed = true;
            continue;
        }
        if (packet->stream_index == index) {
            ++packets;
            if (avcodec_send_packet(codec.get(), packet.get()) < 0) {
                qCDebug(lcMedia) << "undecodable image packet in" << path;
            }
        }
        av_packet_unref(packet.get());
    }
    if (!gotFrame) {
        result.error = QStringLiteral("No decodable image data");
        return false;
    }
    if (const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(frame->format))) {
        result.hasAlpha = (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    }
    if (isPlanarFloat(frame->format)) {
        result.image = convertLinearFloat(frame.get()); // EXR: linear light → sRGB
    } else {
        // swscale handles every other format these decoders emit.
        FrameConverter converter;
        result.image = converter.convert(frame.get(), QSize(frame->width, frame->height));
    }
    if (result.image.isNull()) {
        result.error
            = QStringLiteral("Cannot convert pixel format %1")
                  .arg(QString::fromUtf8(av_get_pix_fmt_name(AVPixelFormat(frame->format))));
        return false;
    }
    result.backend = ImageBackend::FFmpeg;
    return true;
}

} // namespace kk::media::detail
