// SPDX-License-Identifier: GPL-3.0-or-later
// ExportFFmpeg — internal helpers of the export engine: RAII wrappers for FFmpeg objects,
// error strings and version-independent queries of encoder capabilities (FFmpeg 6.1 ... 9.x).
// Only files in src/export include this header (src/media has its own internal equivalent).
#pragma once

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/hwcontext.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <functional>
#include <memory>
#include <vector>

namespace kk::exporter {

struct CodecContextDeleter
{
    void operator()(AVCodecContext* ctx) const { avcodec_free_context(&ctx); }
};
struct FrameDeleter
{
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
struct PacketDeleter
{
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct SwsDeleter
{
    void operator()(SwsContext* ctx) const { sws_freeContext(ctx); }
};
struct SwrDeleter
{
    void operator()(SwrContext* ctx) const { swr_free(&ctx); }
};
struct BufferRefDeleter
{
    void operator()(AVBufferRef* ref) const { av_buffer_unref(&ref); }
};
struct AudioFifoDeleter
{
    void operator()(AVAudioFifo* fifo) const { av_audio_fifo_free(fifo); }
};
struct FilterGraphDeleter
{
    void operator()(AVFilterGraph* graph) const { avfilter_graph_free(&graph); }
};

using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;
using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;
using FilterGraphPtr = std::unique_ptr<AVFilterGraph, FilterGraphDeleter>;

/// Text of an FFmpeg error code.
inline QString avError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}

/// Called with every encoded packet (stream index not yet set). Return false to abort.
/// The packet is only valid during the call (the callee may av_packet_ref/move it).
using PacketSink = std::function<bool(AVPacket* packet)>;

/// Pixel formats @p codec accepts (empty = unknown/any). Guards the FFmpeg 7.1 API change
/// (AVCodec::pix_fmts deprecated in favour of avcodec_get_supported_config()).
inline std::vector<AVPixelFormat> supportedPixelFormats(const AVCodec* codec)
{
    std::vector<AVPixelFormat> out;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(
            nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count)
            >= 0
        && configs) {
        const auto* list = static_cast<const AVPixelFormat*>(configs);
        out.assign(list, list + count);
    }
#else
    for (const AVPixelFormat* p = codec->pix_fmts; p && *p != AV_PIX_FMT_NONE; ++p) {
        out.push_back(*p);
    }
#endif
    return out;
}

/// Sample formats @p codec accepts (empty = unknown/any).
inline std::vector<AVSampleFormat> supportedSampleFormats(const AVCodec* codec)
{
    std::vector<AVSampleFormat> out;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(
            nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &count)
            >= 0
        && configs) {
        const auto* list = static_cast<const AVSampleFormat*>(configs);
        out.assign(list, list + count);
    }
#else
    for (const AVSampleFormat* p = codec->sample_fmts; p && *p != AV_SAMPLE_FMT_NONE; ++p) {
        out.push_back(*p);
    }
#endif
    return out;
}

/// Sample rates @p codec accepts (empty = any).
inline std::vector<int> supportedSampleRates(const AVCodec* codec)
{
    std::vector<int> out;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(
            nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configs, &count)
            >= 0
        && configs) {
        const auto* list = static_cast<const int*>(configs);
        out.assign(list, list + count);
    }
#else
    for (const int* p = codec->supported_samplerates; p && *p; ++p) {
        out.push_back(*p);
    }
#endif
    return out;
}

/// One-time FFmpeg setup shared with the media engine (log level and routing of FFmpeg's
/// messages into Qt logging), so encoder chatter does not reach stderr. Idempotent.
void initExportFfmpeg();

} // namespace kk::exporter
