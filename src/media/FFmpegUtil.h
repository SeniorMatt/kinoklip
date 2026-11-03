// SPDX-License-Identifier: GPL-3.0-or-later
// FFmpegUtil — internal RAII wrappers and helpers shared by the media engine's FFmpeg code.
// Not part of the public API: only files in src/media include this header.
#pragma once

#include "media/MediaInfo.h"

#include <QByteArray>
#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <memory>

namespace kk::media::detail {

struct HwDecodeState;

// ---- RAII wrappers (std::unique_ptr with FFmpeg's free functions) ----

/// Closes an input context; one opened on an embedded path (custom AVIOContext over a byte
/// range of a single-file project) also frees that I/O context and its reader.
struct FormatContextDeleter
{
    void operator()(AVFormatContext* ctx) const;
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

/// One-time global FFmpeg setup (log level + routing of FFmpeg logs into `kk.media`).
/// Idempotent and thread-safe; called by every entry point of the media engine.
void initFfmpeg();

/// Captures FFmpeg's log output of the current thread while alive (RAII, nestable): the
/// messages are not shown (debug output only) but remembered, so a caller can turn an expected
/// failure (hardware probes, a broken GPU download) into one meaningful line of its own
/// instead of FFmpeg's per-call error spam. Messages FFmpeg logs from other threads (its own
/// worker threads) are not captured.
class FfmpegLogCapture
{
public:
    FfmpegLogCapture();
    ~FfmpegLogCapture();
    FfmpegLogCapture(const FfmpegLogCapture&) = delete;
    FfmpegLogCapture& operator=(const FfmpegLogCapture&) = delete;

    /// Number of error (or worse) messages captured.
    [[nodiscard]] int errorCount() const { return m_errorCount; }
    /// The first error message captured, e.g. "h264_qsv: Error creating a MFX session", or empty.
    [[nodiscard]] QString firstError() const { return m_firstError; }

    /// Internal: called by the log callback (also forwards to the enclosing capture).
    void record(int level, const QByteArray& text);

private:
    FfmpegLogCapture* m_outer = nullptr;
    int m_errorCount = 0;
    QString m_firstError;
};

/// Human-readable text for an FFmpeg error code (AVERROR(...)).
QString avErrorString(int errorCode);

/// Opens `path` with libavformat and reads stream info. Embedded paths (media inside a
/// single-file project, MediaSource.h) are read in place through a custom AVIOContext over the
/// entry's byte range, with a file handle of their own (no shared seek position).
/// @return the context, or nullptr with a message in `*error` (if non-null).
FormatContextPtr openInput(const QString& path, QString* error);

/// Index of the primary video stream, or -1. Cover art (attached pictures) is ignored.
int findVideoStream(const AVFormatContext* fmt);

/// Index of the primary audio stream, or -1.
int findAudioStream(const AVFormatContext* fmt);

/// Creates and opens a decoder for `stream`.
/// @param threadCount 0 = automatic (one per core), otherwise the exact number of threads.
/// @param hwDevice optional hardware device (see HwDevices.h); `hwState` must then be given
///        and outlive the codec context.
/// @return the opened codec context, or nullptr with a message in `*error`.
CodecContextPtr openDecoder(const AVStream* stream, int threadCount, QString* error,
    AVBufferRef* hwDevice = nullptr, HwDecodeState* hwState = nullptr);

/// True if `packet` of an H.264 stream starts an IDR picture (handles both AVCC/MP4 and
/// Annex-B/MPEG-TS framing). An IDR resets the decoder's reference state by itself.
bool isH264IdrPacket(const AVPacket* packet, const AVCodecParameters* par);

/// Seeks the demuxer so the following packets of `streamIndex` start at or before `ts`
/// (stream time base), normally on a keyframe. Targets at or before `firstTs` (the stream's
/// earliest timestamp) rewind to the very first packet. The caller must flush its decoder.
/// @return >= 0 on success, an FFmpeg error code otherwise.
int seekDemuxer(AVFormatContext* fmt, int streamIndex, int64_t ts, int64_t firstTs);

/// Media time 0 of a file in AV_TIME_BASE units (microseconds): the container start time,
/// i.e. the earliest start of any stream. All public media APIs measure time from here, which
/// keeps audio and video of one file in sync even when their streams start at different times.
int64_t originUs(const AVFormatContext* fmt);

/// True if the file name extension denotes a format that is always a still image
/// (SupportedFormats Image/RawImage, except gif/webp which may be animated). Used to snap
/// thumbnail times to 0.
bool isStillImageSuffix(const QString& suffix);

/// Fills a MediaInfo from an opened (and stream-info-probed) format context.
/// Defined in MediaProbe.cpp; shared with the decoders so they do not probe twice.
MediaInfo infoFromFormatContext(const AVFormatContext* fmt, const QString& path);

} // namespace kk::media::detail
