// SPDX-License-Identifier: GPL-3.0-or-later
// FFmpegUtil — implementation of the internal FFmpeg helpers (opening files, decoders, logging).
#include "media/FFmpegUtil.h"

#include "media/HwDevices.h"
#include "media/MediaLog.h"

#include <QByteArray>
#include <QFile>

#include <cstdarg>
#include <limits>
#include <mutex>

namespace kk::media::detail {

namespace {

/// When rewinding to the start we aim this far before the first timestamp (see seekDemuxer).
constexpr int64_t kRewindMarginUs = 10'000'000;

/// FFmpeg messages above this level (i.e. less important) are dropped before formatting.
constexpr int kFfmpegLogLevel = AV_LOG_WARNING;

/// Routes FFmpeg log lines into the `kk.media` category: errors become warnings, everything
/// else debug output. Called by FFmpeg from arbitrary threads.
void ffmpegLogCallback(void* avClass, int level, const char* format, va_list args)
{
    if (level > av_log_get_level()) {
        return;
    }
    // FFmpeg tracks "start of line" state per call site; per thread is the best we can do.
    thread_local int printPrefix = 1;
    char line[1024];
    av_log_format_line2(avClass, level, format, args, line, int(sizeof line), &printPrefix);
    const QByteArray text = QByteArray(line).trimmed();
    if (text.isEmpty()) {
        return;
    }
    if (level <= AV_LOG_ERROR) {
        qCWarning(lcMedia, "ffmpeg: %s", text.constData());
    } else {
        qCDebug(lcMedia, "ffmpeg: %s", text.constData());
    }
}

/// Converts a local path to the byte string libavformat expects.
QByteArray encodePath(const QString& path)
{
#ifdef Q_OS_WIN
    // FFmpeg's file protocol expects UTF-8 on Windows.
    return path.toUtf8();
#else
    // The "file:" prefix stops FFmpeg from interpreting "name:..." as a protocol.
    return QByteArrayLiteral("file:") + QFile::encodeName(path);
#endif
}

void setError(QString* error, const QString& message)
{
    if (error) {
        *error = message;
    }
}

int findStream(const AVFormatContext* fmt, AVMediaType type)
{
    int best = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type != type) {
            continue;
        }
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            continue; // cover art in MP3/M4A files is not a video stream
        }
        if (!avcodec_find_decoder(st->codecpar->codec_id)) {
            continue;
        }
        if (best < 0 || (st->disposition & AV_DISPOSITION_DEFAULT)) {
            best = int(i);
            if (st->disposition & AV_DISPOSITION_DEFAULT) {
                break;
            }
        }
    }
    return best;
}

} // namespace

void initFfmpeg()
{
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(kFfmpegLogLevel);
        av_log_set_callback(ffmpegLogCallback);
    });
}

QString avErrorString(int errorCode)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, buffer, sizeof buffer);
    return QString::fromUtf8(buffer);
}

FormatContextPtr openInput(const QString& path, QString* error)
{
    initFfmpeg();
    if (!QFile::exists(path)) {
        setError(error, QStringLiteral("File not found: %1").arg(path));
        return {};
    }
    AVFormatContext* raw = nullptr;
    const QByteArray url = encodePath(path);
    int ret = avformat_open_input(&raw, url.constData(), nullptr, nullptr);
    if (ret < 0) {
        // avformat_open_input() frees the context on failure.
        setError(error, QStringLiteral("Unsupported or damaged file (%1)").arg(avErrorString(ret)));
        return {};
    }
    FormatContextPtr fmt(raw);
    ret = avformat_find_stream_info(fmt.get(), nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("Cannot read stream info (%1)").arg(avErrorString(ret)));
        return {};
    }
    return fmt;
}

int findVideoStream(const AVFormatContext* fmt)
{
    return findStream(fmt, AVMEDIA_TYPE_VIDEO);
}

int findAudioStream(const AVFormatContext* fmt)
{
    return findStream(fmt, AVMEDIA_TYPE_AUDIO);
}

CodecContextPtr openDecoder(const AVStream* stream, int threadCount, QString* error,
    AVBufferRef* hwDevice, HwDecodeState* hwState)
{
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        setError(error,
            QStringLiteral("No decoder for codec '%1'")
                .arg(QString::fromUtf8(avcodec_get_name(stream->codecpar->codec_id))));
        return {};
    }
    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) {
        setError(error, QStringLiteral("Out of memory"));
        return {};
    }
    int ret = avcodec_parameters_to_context(ctx.get(), stream->codecpar);
    if (ret < 0) {
        setError(error, QStringLiteral("Bad codec parameters (%1)").arg(avErrorString(ret)));
        return {};
    }
    ctx->pkt_timebase = stream->time_base;
    ctx->thread_count = threadCount; // 0 = let FFmpeg pick (one per core)
    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE; // packet.opaque → frame.opaque (seek generations)
    if (hwDevice && hwState) {
        attachHwDevice(ctx.get(), hwDevice, hwState);
    }
    ret = avcodec_open2(ctx.get(), codec, nullptr);
    if (ret < 0) {
        setError(error, QStringLiteral("Cannot open decoder (%1)").arg(avErrorString(ret)));
        return {};
    }
    return ctx;
}

bool isH264IdrPacket(const AVPacket* packet, const AVCodecParameters* par)
{
    constexpr int kNalTypeMask = 0x1f;
    constexpr int kNalSliceNonIdr = 1;
    constexpr int kNalSliceIdr = 5;
    // Returns 1 = IDR slice, 0 = other slice, -1 = not a slice (keep looking).
    auto classify = [](uint8_t header) {
        const int type = header & kNalTypeMask;
        return type == kNalSliceIdr ? 1 : (type >= kNalSliceNonIdr && type < kNalSliceIdr ? 0 : -1);
    };
    const uint8_t* data = packet->data;
    const int size = packet->size;
    const bool avcc = par->extradata_size >= 7 && par->extradata[0] == 1;
    if (avcc) {
        // MP4/MKV: NAL units prefixed by their big-endian length (1-4 bytes, from avcC).
        const int lengthSize = (par->extradata[4] & 3) + 1;
        for (int pos = 0; pos + lengthSize < size;) {
            int64_t nalSize = 0;
            for (int i = 0; i < lengthSize; ++i) {
                nalSize = (nalSize << 8) | data[pos + i];
            }
            pos += lengthSize;
            if (nalSize <= 0 || pos + nalSize > size) {
                return false;
            }
            const int kind = classify(data[pos]);
            if (kind >= 0) {
                return kind == 1;
            }
            pos += int(nalSize);
        }
        return false;
    }
    // Annex-B (MPEG-TS, raw .h264): NAL units separated by 00 00 01 start codes.
    for (int pos = 0; pos + 3 < size; ++pos) {
        if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
            const int kind = classify(data[pos + 3]);
            if (kind >= 0) {
                return kind == 1;
            }
            pos += 2;
        }
    }
    return false;
}

int seekDemuxer(AVFormatContext* fmt, int streamIndex, int64_t ts, int64_t firstTs)
{
    const bool toStart = ts <= firstTs;
    if (toStart) {
        // Some demuxers (MPEG-TS/PS) seek by DTS, which precedes the first PTS when there are
        // B-frames; seeking to the first PTS would skip the first keyframe. Aim well before.
        ts = firstTs
            - av_rescale_q(kRewindMarginUs, kMicrosecondsTb, fmt->streams[streamIndex]->time_base);
    }
    int ret = av_seek_frame(fmt, streamIndex, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        ret = avformat_seek_file(
            fmt, streamIndex, std::numeric_limits<int64_t>::min(), ts, toStart ? firstTs : ts, 0);
    }
    return ret;
}

int64_t originUs(const AVFormatContext* fmt)
{
    return fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0;
}

bool isStillImageSuffix(const QString& suffix)
{
    static const QStringList kSuffixes = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("jpe"),
        QStringLiteral("jfif"),
        QStringLiteral("webp"),
        QStringLiteral("bmp"),
        QStringLiteral("tif"),
        QStringLiteral("tiff"),
        QStringLiteral("tga"),
    };
    return kSuffixes.contains(suffix.toLower());
}

} // namespace kk::media::detail
