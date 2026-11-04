// SPDX-License-Identifier: GPL-3.0-or-later
// FFmpegUtil — implementation of the internal FFmpeg helpers (opening files, decoders, logging).
#include "media/FFmpegUtil.h"

#include "media/HwDevices.h"
#include "media/MediaLog.h"
#include "media/SupportedFormats.h"
#include "pack/EmbeddedMedia.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <cstdarg>
#include <limits>
#include <mutex>

namespace kk::media::detail {

namespace {

/// When rewinding to the start we aim this far before the first timestamp (see seekDemuxer).
constexpr int64_t kRewindMarginUs = 10'000'000;

/// FFmpeg messages above this level (i.e. less important) are dropped before formatting.
constexpr int kFfmpegLogLevel = AV_LOG_WARNING;

/// FFmpeg *error* messages that are harmless in an editor and only become debug output:
/// metadata the mov demuxer re-reads raw, and the reference-picture complaints every H.264 /
/// HEVC decoder prints for the first frames after a seek (VideoDecoder skips those frames).
constexpr const char* kHarmlessErrors[] = {
    "UDTA parsing failed",
    "co located POCs unavailable",
    "mmco: unref short failure",
    "Could not find ref with POC",
    "Missing reference picture",
    "Error constructing the frame RPS",
};

/// The innermost FfmpegLogCapture of this thread (captures form a stack).
thread_local FfmpegLogCapture* t_capture = nullptr;

bool isHarmless(const QByteArray& text)
{
    for (const char* pattern : kHarmlessErrors) {
        if (text.contains(pattern)) {
            return true;
        }
    }
    return false;
}

/// Routes FFmpeg log lines into the `kk.media` category: errors become warnings, everything
/// else (and known harmless errors) debug output. While an FfmpegLogCapture is active on the
/// calling thread, messages are recorded there and only appear as debug output.
/// Called by FFmpeg from arbitrary threads.
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
    if (t_capture) {
        t_capture->record(level, text);
        qCDebug(lcMedia, "ffmpeg (captured): %s", text.constData());
        return;
    }
    if (level <= AV_LOG_ERROR && !isHarmless(text)) {
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

FfmpegLogCapture::FfmpegLogCapture()
    : m_outer(t_capture)
{
    initFfmpeg();
    t_capture = this;
}

FfmpegLogCapture::~FfmpegLogCapture()
{
    t_capture = m_outer;
}

void FfmpegLogCapture::record(int level, const QByteArray& text)
{
    if (level <= AV_LOG_ERROR) {
        ++m_errorCount;
        if (m_firstError.isEmpty()) {
            // "[h264_qsv @ 0x56f7ebbcd800] Failed to ..." -> "h264_qsv: Failed to ..."
            static const QRegularExpression context(
                QStringLiteral(R"(\[([^\]@]+?) @ 0x[0-9a-fA-F]+\] )"));
            m_firstError = QString::fromUtf8(text).replace(context, QStringLiteral("\\1: "));
        }
    }
    if (m_outer) {
        m_outer->record(level, text);
    }
}

void initFfmpeg()
{
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(kFfmpegLogLevel); });
    // Every time, not once: Qt Multimedia's FFmpeg backend (QT_MEDIA_BACKEND=ffmpeg, the
    // bundle's default) installs its own av_log callback when it initialises - on the first
    // QMediaDevices/QAudioSink use, i.e. typically after our first decoder was opened - and
    // FFmpeg's messages then bypassed our routing, filters and captures (the raw
    // "[AVHWFramesContext] Failed to sync surface", UDTA, CUDA and MFX lines of alpha.3).
    // A plain pointer store; FFmpeg reads it unsynchronised either way.
    av_log_set_callback(ffmpegLogCallback);
}

QString avErrorString(int errorCode)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, buffer, sizeof buffer);
    return QString::fromUtf8(buffer);
}

namespace {

/// Reader state of a custom AVIOContext over an embedded entry (owned through pb->opaque).
struct AvioRange
{
    pack::RangeFile file;
    int64_t pos = 0;
};

/// 256 KiB reads: large enough for few syscalls on high-bitrate media, small enough for seeks.
constexpr int kAvioBufferSize = 256 * 1024;

int avioRead(void* opaque, uint8_t* buffer, int size)
{
    auto* r = static_cast<AvioRange*>(opaque);
    const qint64 n = r->file.read(r->pos, buffer, size);
    if (n < 0)
        return AVERROR(EIO);
    if (n == 0)
        return AVERROR_EOF;
    r->pos += n;
    return int(n);
}

int64_t avioSeek(void* opaque, int64_t offset, int whence)
{
    auto* r = static_cast<AvioRange*>(opaque);
    const int64_t size = r->file.size();
    whence &= ~AVSEEK_FORCE;
    int64_t target = 0;
    switch (whence) {
    case AVSEEK_SIZE:
        return size;
    case SEEK_SET:
        target = offset;
        break;
    case SEEK_CUR:
        target = r->pos + offset;
        break;
    case SEEK_END:
        target = size + offset;
        break;
    default:
        return AVERROR(EINVAL);
    }
    if (target < 0)
        return AVERROR(EINVAL);
    r->pos = target;
    return target;
}

void freeCustomIo(AVIOContext* pb)
{
    delete static_cast<AvioRange*>(pb->opaque);
    av_freep(&pb->buffer);
    avio_context_free(&pb);
}

} // namespace

void FormatContextDeleter::operator()(AVFormatContext* ctx) const
{
    AVIOContext* custom = ctx && (ctx->flags & AVFMT_FLAG_CUSTOM_IO) ? ctx->pb : nullptr;
    avformat_close_input(&ctx);
    if (custom)
        freeCustomIo(custom);
}

namespace {

/// True if @p path starts like an ISO / QuickTime file (a top-level box: ftyp, mdat, ...).
bool looksLikeIsoFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = file.read(8).mid(4);
    for (const char* box : {"ftyp", "mdat", "moov", "free", "skip", "wide", "pnot"}) {
        if (head == box)
            return true;
    }
    return false;
}

/// The user's message for an avformat_open_input() failure @p ret on @p path; @p ffmpegError
/// is the first error FFmpeg logged meanwhile.
QString openErrorMessage(const QString& path, int ret, const QString& ffmpegError)
{
    if (ret == AVERROR(EACCES) || ret == AVERROR(EPERM))
        return QStringLiteral("Cannot read the file: permission denied");
    // A real MP4 / MOV without its index (random bytes named .mp4 say the same in FFmpeg).
    if (ffmpegError.contains(QLatin1String("moov atom not found")) && looksLikeIsoFile(path))
        return QStringLiteral("The file is incomplete: its index (the MP4 / MOV \"moov\" atom) "
                              "is missing, as when a recording or a copy is interrupted");
    if (ret == AVERROR_INVALIDDATA)
        return QStringLiteral("Not a video, audio or image file, or the file is damaged");
    return QStringLiteral("Unsupported or damaged file (%1)").arg(avErrorString(ret));
}

} // namespace

FormatContextPtr openInput(const QString& path, QString* error)
{
    initFfmpeg();
    AVFormatContext* raw = nullptr;
    AVIOContext* custom = nullptr;
    QByteArray url;
    if (QFileInfo(path).isFile()) {
        url = encodePath(path);
    } else if (pack::mayBeEmbeddedPath(path)) {
        auto range = std::make_unique<AvioRange>();
        QString why;
        if (!range->file.openPath(path, &why)) {
            setError(error, why);
            return {};
        }
        auto* buffer = static_cast<uint8_t*>(av_malloc(kAvioBufferSize));
        custom = buffer ? avio_alloc_context(
                              buffer, kAvioBufferSize, 0, range.get(), avioRead, nullptr, avioSeek)
                        : nullptr;
        raw = custom ? avformat_alloc_context() : nullptr;
        if (!raw) {
            if (custom) {
                custom->opaque = nullptr; // range is still owned by the unique_ptr
                freeCustomIo(custom); // also frees the buffer
            } else
                av_free(buffer);
            setError(error, QStringLiteral("Out of memory"));
            return {};
        }
        range.release(); // owned by custom->opaque from here on
        raw->pb = custom;
        raw->flags |= AVFMT_FLAG_CUSTOM_IO;
        // The entry's file name lets demuxers that score by extension recognise the file.
        url = QFileInfo(path).fileName().toUtf8();
    } else {
        setError(error, QStringLiteral("File not found: %1").arg(path));
        return {};
    }
    if (!custom && QFileInfo(path).size() == 0) {
        setError(error, QStringLiteral("The file is empty (0 bytes)"));
        return {};
    }
    // FFmpeg's own messages say what is wrong with a damaged file ("moov atom not found");
    // capture them for the user's message instead of logging them as warnings.
    int ret = 0;
    QString ffmpegError;
    {
        FfmpegLogCapture capture;
        ret = avformat_open_input(&raw, url.constData(), nullptr, nullptr);
        ffmpegError = capture.firstError();
    }
    if (ret < 0) {
        // avformat_open_input() frees the context on failure, but never a custom pb.
        if (custom)
            freeCustomIo(custom);
        setError(error, openErrorMessage(path, ret, ffmpegError));
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
    const QString lower = suffix.toLower();
    if (lower == QLatin1String("gif") || lower == QLatin1String("webp")) {
        return false; // possibly animated
    }
    const MediaCategory category = SupportedFormats::categoryFor(QLatin1String("x.") + lower);
    return category == MediaCategory::Image || category == MediaCategory::RawImage;
}

} // namespace kk::media::detail

namespace kk::media {

void installFfmpegLogRouting()
{
    detail::initFfmpeg();
}

} // namespace kk::media
