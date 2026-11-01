// SPDX-License-Identifier: GPL-3.0-or-later
// FFmpegUtil — internal RAII wrappers and helpers shared by the media engine's FFmpeg code.
// Not part of the public API: only files in src/media include this header.
#pragma once

#include "media/MediaInfo.h"

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <memory>

namespace cw::media::detail {

// ---- RAII wrappers (std::unique_ptr with FFmpeg's free functions) ----

struct FormatContextDeleter
{
    void operator()(AVFormatContext* ctx) const { avformat_close_input(&ctx); }
};
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
struct SwsContextDeleter
{
    void operator()(SwsContext* ctx) const { sws_freeContext(ctx); }
};
struct SwrContextDeleter
{
    void operator()(SwrContext* ctx) const { swr_free(&ctx); }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

/// Microsecond time base ({1, 1'000'000}) for av_rescale_q().
inline constexpr AVRational kMicrosecondsTb {1, 1000000};

// ---- Helpers ----

/// One-time global FFmpeg setup (log level + routing of FFmpeg logs into `cw.media`).
/// Idempotent and thread-safe; called by every entry point of the media engine.
void initFfmpeg();

/// Human-readable text for an FFmpeg error code (AVERROR(...)).
QString avErrorString(int errorCode);

/// Opens `path` with libavformat and reads stream info.
/// @return the context, or nullptr with a message in `*error` (if non-null).
FormatContextPtr openInput(const QString& path, QString* error);

/// Index of the primary video stream, or -1. Cover art (attached pictures) is ignored.
int findVideoStream(const AVFormatContext* fmt);

/// Index of the primary audio stream, or -1.
int findAudioStream(const AVFormatContext* fmt);

/// Creates and opens a decoder for `stream`.
/// @param threadCount 0 = automatic (one per core), otherwise the exact number of threads.
/// @return the opened codec context, or nullptr with a message in `*error`.
CodecContextPtr openDecoder(const AVStream* stream, int threadCount, QString* error);

/// Seeks the demuxer so the following packets of `streamIndex` start at or before `ts`
/// (stream time base), normally on a keyframe. Targets at or before `firstTs` (the stream's
/// earliest timestamp) rewind to the very first packet. The caller must flush its decoder.
/// @return >= 0 on success, an FFmpeg error code otherwise.
int seekDemuxer(AVFormatContext* fmt, int streamIndex, int64_t ts, int64_t firstTs);

/// Media time 0 of a file in AV_TIME_BASE units (microseconds): the container start time,
/// i.e. the earliest start of any stream. All public media APIs measure time from here, which
/// keeps audio and video of one file in sync even when their streams start at different times.
int64_t originUs(const AVFormatContext* fmt);

/// True if the file name extension denotes a still image format we load with QImageReader.
bool isStillImageSuffix(const QString& suffix);

/// Fills a MediaInfo from an opened (and stream-info-probed) format context.
/// Defined in MediaProbe.cpp; shared with the decoders so they do not probe twice.
MediaInfo infoFromFormatContext(const AVFormatContext* fmt, const QString& path);

} // namespace cw::media::detail
