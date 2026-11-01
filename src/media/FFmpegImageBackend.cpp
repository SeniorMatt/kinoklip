// SPDX-License-Identifier: GPL-3.0-or-later
// FFmpegImageBackend — stills that only libavcodec decodes (JPEG XL, OpenEXR, DPX, PSD, QOI,
// PCX, SGI, JPEG 2000...): first frame of the file, converted with swscale.
#include "media/FFmpegUtil.h"
#include "media/FrameConverter.h"
#include "media/ImageBackends.h"
#include "media/MediaLog.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

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

/// Opens a decoder for a still. OpenEXR stores linear light; `apply_trc=iec61966_2_1` makes
/// the decoder apply the sRGB curve so the 8-bit result looks right (values > 1 clamp later).
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
    ctx->thread_count = 0; // big EXR/JPEG 2000 frames decode slice-parallel
    AVDictionary* options = nullptr;
    if (stream->codecpar->codec_id == AV_CODEC_ID_EXR) {
        av_dict_set(&options, "apply_trc", "iec61966_2_1", 0);
    }
    const int ret = avcodec_open2(ctx.get(), codec, &options);
    av_dict_free(&options);
    if (ret < 0) {
        *error = QStringLiteral("Cannot open decoder (%1)").arg(avErrorString(ret));
        return {};
    }
    return ctx;
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
    // swscale handles every format these decoders emit, including planar float (EXR), with
    // clamping to 0..1 on the way to 8 bit.
    FrameConverter converter;
    result.image = converter.convert(frame.get(), QSize(frame->width, frame->height));
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
