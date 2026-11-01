// SPDX-License-Identifier: GPL-3.0-or-later
// VideoDecoder — FFmpeg based frame-accurate video decoding (seek/decode-forward state machine).
#include "media/VideoDecoder.h"

#include "media/FFmpegUtil.h"
#include "media/FrameConverter.h"
#include "media/MediaLog.h"

#include <QFileInfo>
#include <QImageReader>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <limits>

namespace cw::media {

using namespace detail;

namespace {

/// Without a usable keyframe index, a target less than this far ahead of the decoder position
/// is reached by decoding forward; anything further triggers a seek.
constexpr int64_t kForwardDecodeLimitUs = 2'000'000;

/// When a seek lands after the target (B-frame reordering, imprecise index), we seek again
/// this much earlier, doubling each time; the last retry seeks to the start of the stream.
constexpr int64_t kSeekBackoffUs = 1'000'000;
constexpr int kMaxSeekRetries = 4;

/// After a seek, decoders may emit damaged frames until the next keyframe; skip at most
/// this many of them (a genuinely damaged file still returns *something*).
constexpr int kMaxSkippedCorruptFrames = 60;

/// Used to synthesise timestamps when a file has none and to size frame steps.
constexpr AVRational kFallbackFrameRate {25, 1};

/// A decoded frame plus its presentation timestamp (stream time base).
struct DecodedFrame
{
    FramePtr frame;
    int64_t pts = AV_NOPTS_VALUE;
    explicit operator bool() const { return frame != nullptr; }
};

void setError(QString* error, const QString& message)
{
    if (error) {
        *error = message;
    }
}

} // namespace

// ---- Implementation state ---------------------------------------------------------------
//
// The decoder keeps two frames: `cur` (the last frame handed out, or about to be) and `next`
// (the frame after it, decoded as look-ahead). `cur` is the right answer for every time in
// [cur.pts, next.pts) — or [cur.pts, ∞) once the stream hit EOF. Knowing `next` is what makes
// the answer exact even for variable-frame-rate files with unreliable frame durations.

struct VideoDecoder::Impl
{
    MediaInfo info;
    int threadCount = 0;
    bool isOpen = false;
    int64_t lastReturnedUs = -1;

    // Still images: decoded once, scaled copies on demand.
    bool isStill = false;
    QImage still;
    QImage stillScaled;
    QSize stillScaledFor;

    // FFmpeg state (video files).
    FormatContextPtr fmt;
    CodecContextPtr codec;
    AVStream* stream = nullptr;
    int streamIndex = -1;
    PacketPtr packet;
    FramePtr scratch;
    FrameConverter converter;
    int64_t originTs = 0; ///< stream timestamp of media time 0
    int64_t firstTs = 0; ///< earliest timestamp of the stream (seek lower bound)
    int64_t firstFrameTs = 0; ///< pts of the first decodable frame (clamp lower bound)
    int64_t frameStepTs = 1; ///< one nominal frame period in stream time base
    int64_t lastDecodedTs = AV_NOPTS_VALUE;
    int64_t lastSeekTs = 0;
    bool eof = false; ///< decoder fully drained; `cur` is the last frame of the stream
    bool draining = false; ///< demuxer hit EOF, flush packet sent to the decoder
    bool afterSeek = false; ///< no frame delivered since the last seek
    DecodedFrame cur;
    DecodedFrame next;
    QImage curImage; ///< `cur` converted for curImageMaxSize
    QSize curImageMaxSize;
    bool curImageValid = false;

    bool openStill(const QString& path, QString* error);
    bool openVideo(const QString& path, QString* error);
    QImage stillAt(QSize maxSize);

    int64_t usToTs(int64_t us) const
    {
        return originTs
            + av_rescale_q_rnd(us, kMicrosecondsTb, stream->time_base,
                AVRounding(AV_ROUND_DOWN | AV_ROUND_PASS_MINMAX));
    }
    int64_t tsToUs(int64_t ts) const
    {
        return av_rescale_q(ts - originTs, stream->time_base, kMicrosecondsTb);
    }

    bool isCurrent(int64_t ts) const
    {
        return cur && cur.pts <= ts && (next ? ts < next.pts : eof);
    }
    bool decodeOne(DecodedFrame& out);
    bool seek(int64_t targetTs, int attempt);
    int64_t keyframeAtOrBefore(int64_t ts) const;
    bool canDecodeForwardTo(int64_t ts) const;
    void advanceTo(int64_t ts);
    bool positionExact(int64_t ts);
    bool positionNearKeyframe(int64_t ts);
    QImage currentImage(QSize maxSize);
};

bool VideoDecoder::Impl::openStill(const QString& path, QString* error)
{
    QImageReader reader(path);
    reader.setAutoTransform(true); // apply EXIF orientation
    const QImage image = reader.read();
    if (image.isNull()) {
        setError(error, reader.errorString());
        return false;
    }
    info = probe(path);
    info.valid = info.hasVideo = info.isStillImage = true;
    info.width = info.displayWidth = image.width();
    info.height = info.displayHeight = image.height();
    info.hasAlpha = image.hasAlphaChannel();
    still = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    isStill = true;
    return true;
}

bool VideoDecoder::Impl::openVideo(const QString& path, QString* error)
{
    fmt = openInput(path, error);
    if (!fmt) {
        return false;
    }
    info = infoFromFormatContext(fmt.get(), path);
    streamIndex = findVideoStream(fmt.get());
    if (streamIndex < 0) {
        setError(error, QStringLiteral("No video stream"));
        return false;
    }
    stream = fmt->streams[streamIndex];
    codec = openDecoder(stream, threadCount, error);
    if (!codec) {
        return false;
    }
    // Let the demuxer drop packets of streams we do not decode.
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (int(i) != streamIndex) {
            fmt->streams[i]->discard = AVDISCARD_ALL;
        }
    }
    packet.reset(av_packet_alloc());
    scratch.reset(av_frame_alloc());
    if (!packet || !scratch) {
        setError(error, QStringLiteral("Out of memory"));
        return false;
    }

    const AVRational tb = stream->time_base;
    originTs = av_rescale_q(info.startTimeUs, kMicrosecondsTb, tb);
    firstTs = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : originTs;
    const AVRational rate = info.frameRate.isValid()
        ? AVRational {info.frameRate.num, info.frameRate.den}
        : kFallbackFrameRate;
    frameStepTs = std::max<int64_t>(1, av_rescale_q(1, av_inv_q(rate), tb));

    // Decode the first frame now: validates the file and gives us the clamp lower bound.
    if (!decodeOne(cur)) {
        setError(error, QStringLiteral("No decodable video frames"));
        return false;
    }
    firstFrameTs = cur.pts;

    if (info.isStillImage) {
        // A single image FFmpeg can read but Qt cannot (e.g. DPX/EXR): keep it as a still.
        still = currentImage({});
        if (still.isNull()) {
            setError(error, QStringLiteral("Cannot convert image"));
            return false;
        }
        isStill = true;
        cur = {};
        codec.reset();
        fmt.reset();
    }
    return true;
}

QImage VideoDecoder::Impl::stillAt(QSize maxSize)
{
    const QSize out = fitSize(still.size(), maxSize);
    if (out == still.size()) {
        return still;
    }
    if (stillScaled.isNull() || stillScaledFor != maxSize) {
        stillScaled = still.scaled(out, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        stillScaledFor = maxSize;
    }
    return stillScaled;
}

bool VideoDecoder::Impl::decodeOne(DecodedFrame& out)
{
    int skippedCorrupt = 0;
    for (;;) {
        int ret = avcodec_receive_frame(codec.get(), scratch.get());
        if (ret == 0) {
            if (afterSeek && (scratch->flags & AV_FRAME_FLAG_CORRUPT)
                && skippedCorrupt < kMaxSkippedCorruptFrames) {
                ++skippedCorrupt; // references missing after the seek; wait for a clean frame
                av_frame_unref(scratch.get());
                continue;
            }
            int64_t pts = scratch->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) {
                pts = lastDecodedTs != AV_NOPTS_VALUE ? lastDecodedTs + frameStepTs : firstTs;
            }
            if (afterSeek) {
                qCDebug(lcMedia) << "first frame after seek at" << tsToUs(pts) << "us";
                afterSeek = false;
            }
            lastDecodedTs = pts;
            out.frame.reset(av_frame_alloc());
            if (!out.frame) {
                av_frame_unref(scratch.get());
                return false;
            }
            av_frame_move_ref(out.frame.get(), scratch.get());
            out.pts = pts;
            return true;
        }
        if (ret == AVERROR_EOF) {
            return false;
        }
        if (ret != AVERROR(EAGAIN)) {
            qCDebug(lcMedia) << "video decoder error:" << avErrorString(ret);
            return false;
        }
        if (draining) {
            return false;
        }
        // The decoder wants more input.
        ret = av_read_frame(fmt.get(), packet.get());
        if (ret < 0) {
            if (ret != AVERROR_EOF) {
                qCDebug(lcMedia) << "read error, treating as end of stream:" << avErrorString(ret);
            }
            avcodec_send_packet(codec.get(), nullptr); // flush: returns the delayed frames
            draining = true;
            continue;
        }
        if (packet->stream_index == streamIndex) {
            ret = avcodec_send_packet(codec.get(), packet.get());
            if (ret < 0) {
                qCDebug(lcMedia) << "skipping undecodable packet:" << avErrorString(ret);
            }
        }
        av_packet_unref(packet.get());
    }
}

bool VideoDecoder::Impl::seek(int64_t targetTs, int attempt)
{
    int64_t seekTs = targetTs;
    if (attempt >= kMaxSeekRetries) {
        seekTs = firstTs;
    } else if (attempt > 0) {
        seekTs = targetTs
            - av_rescale_q(kSeekBackoffUs << (attempt - 1), kMicrosecondsTb, stream->time_base);
    }
    seekTs = std::max(seekTs, firstTs);
    const int ret = seekDemuxer(fmt.get(), streamIndex, seekTs, firstTs);
    if (ret < 0) {
        qCWarning(lcMedia) << "seek failed in" << info.path << avErrorString(ret);
        return false;
    }
    qCDebug(lcMedia) << "video seek" << info.path << "target" << tsToUs(targetTs) << "us, attempt"
                     << attempt << "seek point" << tsToUs(seekTs) << "us";
    avcodec_flush_buffers(codec.get());
    cur = {};
    next = {};
    curImageValid = false;
    eof = draining = false;
    afterSeek = true;
    lastSeekTs = seekTs;
    lastDecodedTs = AV_NOPTS_VALUE;
    return true;
}

int64_t VideoDecoder::Impl::keyframeAtOrBefore(int64_t ts) const
{
    const int count = avformat_index_get_entries_count(stream);
    if (count <= 0) {
        return AV_NOPTS_VALUE;
    }
    // Some demuxers (MPEG-TS, ...) build their index while reading; such an index says nothing
    // about regions not read yet, so only trust it when it reaches the target.
    const AVIndexEntry* last = avformat_index_get_entry(stream, count - 1);
    if (!last || last->timestamp < ts) {
        return AV_NOPTS_VALUE;
    }
    const AVIndexEntry* entry
        = avformat_index_get_entry_from_timestamp(stream, ts, AVSEEK_FLAG_BACKWARD);
    return entry ? entry->timestamp : AV_NOPTS_VALUE;
}

bool VideoDecoder::Impl::canDecodeForwardTo(int64_t ts) const
{
    if (!cur || eof || ts < cur.pts) {
        return false;
    }
    const int64_t position = next ? next.pts : cur.pts;
    if (ts <= position) {
        return true;
    }
    // Seeking only pays off if it lands on a keyframe after our current position.
    const int64_t keyframe = keyframeAtOrBefore(ts);
    if (keyframe != AV_NOPTS_VALUE) {
        return keyframe <= position;
    }
    return ts - position < av_rescale_q(kForwardDecodeLimitUs, kMicrosecondsTb, stream->time_base);
}

void VideoDecoder::Impl::advanceTo(int64_t ts)
{
    for (;;) {
        if (!next) {
            if (eof) {
                return;
            }
            if (!decodeOne(next)) {
                eof = true;
                return;
            }
        }
        if (next.pts > ts) {
            return;
        }
        cur = std::move(next);
        next = {};
        curImageValid = false;
    }
}

bool VideoDecoder::Impl::positionExact(int64_t ts)
{
    if (isCurrent(ts)) {
        return true;
    }
    if (!canDecodeForwardTo(ts) && !seek(ts, 0)) {
        return false;
    }
    for (int attempt = 0;; ++attempt) {
        advanceTo(ts);
        if (cur) {
            return true;
        }
        const bool atStart = lastSeekTs <= firstTs || attempt >= kMaxSeekRetries;
        if (next && atStart) {
            // Started at the beginning and the first frame is still after the target:
            // the target precedes the first frame → clamp to it.
            cur = std::move(next);
            next = {};
            return true;
        }
        if (atStart) {
            return false; // nothing decodable in the whole stream
        }
        // Either the first frame after the seek is past the target (B-frame reordering,
        // imprecise index) or nothing was decodable after the seek point (seek landed mid-GOP
        // near the end, truncated file): seek further back and try again.
        if (!seek(ts, attempt + 1)) {
            return false;
        }
    }
}

bool VideoDecoder::Impl::positionNearKeyframe(int64_t ts)
{
    if (isCurrent(ts)) {
        return true;
    }
    if (cur && ts >= cur.pts) {
        // If the keyframe a seek would land on is not after the current frame, the current
        // frame is at least as close to the target — keep it.
        const int64_t keyframe = keyframeAtOrBefore(ts);
        if (keyframe != AV_NOPTS_VALUE && keyframe <= cur.pts) {
            return true;
        }
    }
    for (int attempt = 0; attempt <= kMaxSeekRetries; ++attempt) {
        if (!seek(ts, attempt)) {
            return false;
        }
        const bool atStart = lastSeekTs <= firstTs || attempt == kMaxSeekRetries;
        DecodedFrame frame;
        if (!decodeOne(frame)) {
            eof = true;
            if (atStart) {
                return false;
            }
            continue; // nothing decodable after this seek point: retry further back
        }
        if (frame.pts <= ts || atStart) {
            cur = std::move(frame);
            return true;
        }
    }
    return false;
}

QImage VideoDecoder::Impl::currentImage(QSize maxSize)
{
    if (curImageValid && maxSize == curImageMaxSize) {
        return curImage;
    }
    const AVFrame* frame = cur.frame.get();
    AVRational sar = frame->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) {
        sar = {info.sampleAspectRatio.num, info.sampleAspectRatio.den};
    }
    QSize display(int(std::lround(double(frame->width) * sar.num / sar.den)), frame->height);
    const bool quarterTurn = info.rotation == 90 || info.rotation == 270;
    if (quarterTurn) {
        display.transpose();
    }
    const QSize out = fitSize(display, maxSize);
    QImage image = converter.convert(frame, quarterTurn ? out.transposed() : out);
    if (!image.isNull() && info.rotation != 0) {
        // QTransform::rotate() turns clockwise in Qt's y-down image coordinates.
        image = image.transformed(QTransform().rotate(info.rotation));
        if (image.format() != QImage::Format_ARGB32_Premultiplied) {
            image.convertTo(QImage::Format_ARGB32_Premultiplied);
        }
    }
    curImage = image;
    curImageMaxSize = maxSize;
    curImageValid = !image.isNull();
    return image;
}

// ---- Public API ---------------------------------------------------------------------------

VideoDecoder::VideoDecoder()
    : m_impl(std::make_unique<Impl>())
{ }

VideoDecoder::~VideoDecoder() = default;

void VideoDecoder::setThreadCount(int threads)
{
    m_impl->threadCount = std::max(0, threads);
}

bool VideoDecoder::open(const QString& path, QString* error)
{
    close();
    initFfmpeg();
    bool ok = false;
    if (isStillImageSuffix(QFileInfo(path).suffix())) {
        ok = m_impl->openStill(path, error);
    }
    if (!ok) {
        ok = m_impl->openVideo(path, error);
    }
    if (!ok) {
        close();
        return false;
    }
    m_impl->isOpen = true;
    return true;
}

void VideoDecoder::close()
{
    const int threads = m_impl->threadCount;
    m_impl = std::make_unique<Impl>();
    m_impl->threadCount = threads;
}

bool VideoDecoder::isOpen() const
{
    return m_impl->isOpen;
}

const MediaInfo& VideoDecoder::info() const
{
    return m_impl->info;
}

QImage VideoDecoder::frameAt(int64_t timeUs, QSize maxSize, Accuracy accuracy)
{
    Impl& d = *m_impl;
    if (!d.isOpen) {
        return {};
    }
    if (d.isStill) {
        d.lastReturnedUs = 0;
        return d.stillAt(maxSize);
    }
    // No upper clamp on purpose: container durations can be short of the real last frame
    // (e.g. VFR MP4s). A target past the end seeks to the last keyframe, decodes to EOF and
    // returns the last frame, which stays cached for further past-the-end requests.
    const int64_t ts = std::max(d.usToTs(std::max<int64_t>(timeUs, 0)), d.firstFrameTs);
    const bool ok = accuracy == Accuracy::Exact ? d.positionExact(ts) : d.positionNearKeyframe(ts);
    if (!ok || !d.cur) {
        return {};
    }
    d.lastReturnedUs = std::max<int64_t>(0, d.tsToUs(d.cur.pts));
    return d.currentImage(maxSize);
}

int64_t VideoDecoder::lastFrameTimeUs() const
{
    return m_impl->lastReturnedUs;
}

QSize VideoDecoder::fitSize(QSize source, QSize maxSize)
{
    if (source.isEmpty()) {
        return {};
    }
    constexpr double kUnbounded = std::numeric_limits<double>::infinity();
    const double scaleX
        = maxSize.width() > 0 ? double(maxSize.width()) / source.width() : kUnbounded;
    const double scaleY
        = maxSize.height() > 0 ? double(maxSize.height()) / source.height() : kUnbounded;
    const double scale = std::min(scaleX, scaleY);
    if (std::isinf(scale)) {
        return source;
    }
    return {std::max(1, int(std::lround(source.width() * scale))),
        std::max(1, int(std::lround(source.height() * scale)))};
}

} // namespace cw::media
