// SPDX-License-Identifier: GPL-3.0-or-later
// VideoDecoder — FFmpeg based frame-accurate video decoding (seek/decode-forward state machine,
// hardware decoding with software fall-back).
#include "media/VideoDecoder.h"

#include "media/FFmpegUtil.h"
#include "media/FrameConverter.h"
#include "media/HwDevices.h"
#include "media/MediaLog.h"
#include "media/VideoFrameFactory.h"

#include <QFileInfo>
#include <QImageReader>
#include <QThread>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace cw::media {

using namespace detail;

namespace {

/// Without a usable keyframe index, a target less than this far ahead of the decoder position
/// is reached by decoding forward; anything further triggers a seek.
constexpr int64_t kForwardDecodeLimitUs = 2'000'000;

/// When a seek lands after the target (B-frame reordering, imprecise index) or nothing is
/// decodable after it, we seek again this much earlier, doubling each time; the last retry
/// rewinds to the start of the stream.
constexpr int64_t kSeekBackoffUs = 1'000'000;
constexpr int kMaxSeekRetries = 4;

/// After a seek, decoders may emit damaged frames until the next keyframe; skip at most
/// this many of them (a genuinely damaged file still returns *something*).
constexpr int kMaxSkippedCorruptFrames = 60;

/// Software decoding threads when the caller does not choose: the target machines are
/// 4-core/8-thread laptops that also composite, mix audio and run the UI.
constexpr int kMaxAutoSoftwareThreads = 4;

/// Hardware decoders do the work on the GPU; extra CPU threads only add latency.
constexpr int kHardwareDecodeThreads = 1;

/// Consecutive packet errors from a hardware decoder before we give up on it.
constexpr int kMaxHwPacketErrors = 3;

/// Used to synthesise timestamps when a file has none.
constexpr AVRational kFallbackFrameRate {25, 1};

/// A decoder output frame (hardware surface or software frame) plus its timestamp, and the
/// VideoFrame made from it — created lazily, only for frames that are actually returned.
struct DecodedFrame
{
    FramePtr av;
    int64_t pts = AV_NOPTS_VALUE;
    std::shared_ptr<const VideoFrame> video;
    explicit operator bool() const { return av != nullptr; }
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
//
// Allocation-free steady state: AVPacket/AVFrame structs are recycled (spareFrames), decoder
// buffers come from FFmpeg's pools, GPU downloads from VideoFrameFactory's pool.

struct VideoDecoder::Impl
{
    MediaInfo info;
    int threadCount = 0;
    std::optional<HwAccelMode> hwOverride;
    bool isOpen = false;
    int64_t lastReturnedUs = -1;

    // Still images: decoded once, scaled copies on demand.
    bool isStill = false;
    QImage still;
    QImage stillScaled;
    QSize stillScaledFor;
    std::shared_ptr<const VideoFrame> stillFrame;

    // FFmpeg state (video files).
    FormatContextPtr fmt;
    CodecContextPtr codec;
    AVStream* stream = nullptr;
    int streamIndex = -1;
    PacketPtr packet;
    FramePtr scratch;
    std::vector<FramePtr> spareFrames; ///< recycled AVFrame structs (no per-frame malloc)
    FrameConverter converter;
    VideoFrameFactory factory;
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

    // Hardware decoding.
    HwAccelMode activeHw = HwAccelMode::Off;
    HwDecodeState hwState; ///< get_format callback state (codec->opaque points here)
    bool hwFailed = false; ///< hw decoding broke → switch to software at the next chance
    int hwPacketErrors = 0;

    // Soft seeks (see seek()): skip avcodec_flush_buffers() when re-initialising the decoder
    // is expensive and the new position starts with an IDR picture.
    bool softSeekAllowed = false;
    bool softSeekPending = false; ///< demuxer moved, decoder not flushed yet
    intptr_t generation = 0; ///< tags packets/frames; frames of older generations are stale

    bool openStill(const QString& path, QString* error);
    bool openVideo(const QString& path, QString* error);
    bool openCodec(HwAccelMode requested, QString* error);
    bool switchToSoftware();
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

    FramePtr takeFrame();
    void release(DecodedFrame& frame);
    void releaseAll();
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
    bool position(int64_t ts, Accuracy accuracy);
    std::shared_ptr<const VideoFrame> currentVideoFrame();
    QImage currentImage(QSize maxSize);
};

// ---- Opening --------------------------------------------------------------------------------

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

bool VideoDecoder::Impl::openCodec(HwAccelMode requested, QString* error)
{
    releaseAll();
    codec.reset();
    activeHw = HwAccelMode::Off;
    hwState = {};
    hwFailed = false;
    hwPacketErrors = 0;
    softSeekAllowed = softSeekPending = false;
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    for (const HwAccelMode mode : hwCandidates(requested)) {
        const AVPixelFormat hwFormat = hwPixelFormat(decoder, mode);
        if (hwFormat == AV_PIX_FMT_NONE) {
            continue; // FFmpeg has no hwaccel for this codec on this backend (ProRes, PNG...)
        }
        AVBufferRef* device = HwDeviceManager::instance().acquire(mode);
        if (!device) {
            continue;
        }
        hwState.hwFormat = hwFormat;
        hwState.rejected = false;
        codec = openDecoder(stream, kHardwareDecodeThreads, nullptr, device, &hwState);
        av_buffer_unref(&device);
        if (codec) {
            activeHw = mode;
            // FFmpeg's H.264 flush forces a full hwaccel re-init on the next slice. That costs
            // ~250 ms per seek with NVDEC (decoder re-creation) and Vulkan (video session), but
            // only ~5 ms with VAAPI and nothing for HEVC/VP9/AV1, whose flush keeps state.
            softSeekAllowed = stream->codecpar->codec_id == AV_CODEC_ID_H264
                && (mode == HwAccelMode::Cuda || mode == HwAccelMode::Vulkan);
            return true;
        }
    }
    const int threads = threadCount > 0
        ? threadCount
        : std::min(kMaxAutoSoftwareThreads, QThread::idealThreadCount());
    codec = openDecoder(stream, threads, error);
    return codec != nullptr;
}

bool VideoDecoder::Impl::switchToSoftware()
{
    qCInfo(lcMedia).noquote() << "hardware decoding (" + hwAccelName(activeHw) + ") failed for"
                              << info.path << "- falling back to software";
    // The demuxer position is left as is; with no current frame the next request seeks.
    return openCodec(HwAccelMode::Off, nullptr);
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

    const HwAccelMode requested
        = info.isStillImage ? HwAccelMode::Off : hwOverride.value_or(preferredHwAccel());
    if (!openCodec(requested, error)) {
        return false;
    }
    // Decode (and for hardware: download) the first frame now. This validates the file and
    // the whole hardware path end to end, and gives us the clamp lower bound.
    bool ok = decodeOne(cur);
    if (activeHw != HwAccelMode::Off && (!ok || hwFailed || !currentVideoFrame())) {
        ok = switchToSoftware() && seek(firstTs, 0) && decodeOne(cur);
    }
    if (!ok) {
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
        releaseAll();
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

// ---- Frame bookkeeping ----------------------------------------------------------------------

FramePtr VideoDecoder::Impl::takeFrame()
{
    if (spareFrames.empty()) {
        return FramePtr(av_frame_alloc());
    }
    FramePtr frame = std::move(spareFrames.back());
    spareFrames.pop_back();
    return frame;
}

void VideoDecoder::Impl::release(DecodedFrame& frame)
{
    if (frame.av) {
        av_frame_unref(frame.av.get()); // returns the buffer / hw surface to its pool
        spareFrames.push_back(std::move(frame.av));
    }
    frame.av = nullptr;
    frame.video.reset();
    frame.pts = AV_NOPTS_VALUE;
}

void VideoDecoder::Impl::releaseAll()
{
    release(cur);
    release(next);
    curImageValid = false;
}

// ---- Decoding and seeking -------------------------------------------------------------------

bool VideoDecoder::Impl::decodeOne(DecodedFrame& out)
{
    int skippedCorrupt = 0;
    for (;;) {
        int ret = avcodec_receive_frame(codec.get(), scratch.get());
        if (ret == 0) {
            if (reinterpret_cast<intptr_t>(scratch->opaque) != generation) {
                av_frame_unref(scratch.get()); // decoded before a soft seek: stale
                continue;
            }
            if (activeHw != HwAccelMode::Off && hwState.rejected) {
                // FFmpeg could not use the GPU for this stream and decodes in software with
                // our single hardware thread: better to reopen a proper software decoder.
                av_frame_unref(scratch.get());
                hwFailed = true;
                return false;
            }
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
            release(out);
            out.av = takeFrame();
            if (!out.av) {
                av_frame_unref(scratch.get());
                return false;
            }
            av_frame_move_ref(out.av.get(), scratch.get());
            out.pts = pts;
            return true;
        }
        if (ret == AVERROR_EOF) {
            return false;
        }
        if (ret != AVERROR(EAGAIN)) {
            qCDebug(lcMedia) << "video decoder error:" << avErrorString(ret);
            hwFailed = activeHw != HwAccelMode::Off;
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
            if (softSeekPending) {
                // First packet at the new position: an IDR resets all decoder references, so
                // the flush can be skipped; anything else needs the real flush.
                softSeekPending = false;
                if (!isH264IdrPacket(packet.get(), stream->codecpar)) {
                    avcodec_flush_buffers(codec.get());
                }
            }
            packet->opaque = reinterpret_cast<void*>(generation);
            ret = avcodec_send_packet(codec.get(), packet.get());
            if (ret < 0) {
                qCDebug(lcMedia) << "skipping undecodable packet:" << avErrorString(ret);
                if (activeHw != HwAccelMode::Off && ++hwPacketErrors >= kMaxHwPacketErrors) {
                    av_packet_unref(packet.get());
                    hwFailed = true;
                    return false;
                }
            } else {
                hwPacketErrors = 0;
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
    ++generation;
    if (softSeekAllowed && !draining) {
        softSeekPending = true; // decided at the first packet (see decodeOne)
    } else {
        avcodec_flush_buffers(codec.get()); // also required to leave the drained (EOF) state
        softSeekPending = false;
    }
    releaseAll();
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
                eof = !hwFailed;
                return;
            }
        }
        if (next.pts > ts) {
            return;
        }
        // Skipped frames are recycled without ever being converted or downloaded.
        release(cur);
        cur = std::move(next);
        next = DecodedFrame {};
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
        if (cur || hwFailed) {
            return cur && !hwFailed;
        }
        const bool atStart = lastSeekTs <= firstTs || attempt >= kMaxSeekRetries;
        if (next && atStart) {
            // Started at the beginning and the first frame is still after the target:
            // the target precedes the first frame → clamp to it.
            cur = std::move(next);
            next = DecodedFrame {};
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
            eof = !hwFailed;
            if (atStart || hwFailed) {
                return false;
            }
            continue; // nothing decodable after this seek point: retry further back
        }
        if (frame.pts <= ts || atStart) {
            cur = std::move(frame);
            return true;
        }
        release(frame);
    }
    return false;
}

bool VideoDecoder::Impl::position(int64_t ts, Accuracy accuracy)
{
    auto run = [&] {
        return accuracy == Accuracy::Exact ? positionExact(ts) : positionNearKeyframe(ts);
    };
    bool ok = run();
    if (activeHw != HwAccelMode::Off && (hwFailed || !ok)) {
        // Mid-stream hardware failure (or a stream the GPU cannot handle): redo in software.
        ok = switchToSoftware() && run();
    }
    return ok && cur;
}

// ---- Output ---------------------------------------------------------------------------------

std::shared_ptr<const VideoFrame> VideoDecoder::Impl::currentVideoFrame()
{
    if (!cur.video && cur) {
        FrameMeta meta;
        meta.ptsUs = std::max<int64_t>(0, tsToUs(cur.pts));
        if (next) {
            meta.durationUs = tsToUs(next.pts) - meta.ptsUs;
        } else if (cur.av->duration > 0) {
            meta.durationUs = av_rescale_q(cur.av->duration, stream->time_base, kMicrosecondsTb);
        }
        const AVRational sar = cur.av->sample_aspect_ratio;
        meta.sampleAspectRatio
            = sar.num > 0 && sar.den > 0 ? Rational {sar.num, sar.den} : info.sampleAspectRatio;
        meta.rotation = info.rotation;
        cur.video = factory.make(cur.av.get(), meta);
        if (!cur.video && activeHw != HwAccelMode::Off) {
            hwFailed = true; // download failed
        }
    }
    return cur.video;
}

QImage VideoDecoder::Impl::currentImage(QSize maxSize)
{
    if (curImageValid && maxSize == curImageMaxSize) {
        return curImage;
    }
    // Hardware surfaces are converted from their downloaded copy (shared with decodeFrame()),
    // software frames straight from the decoder output.
    const AVFrame* frame = cur.av.get();
    if (frame->hw_frames_ctx) {
        const std::shared_ptr<const VideoFrame> video = currentVideoFrame();
        if (!video || !video->avFrame) {
            return {};
        }
        frame = video->avFrame;
    }
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

void VideoDecoder::setHwAccel(HwAccelMode mode)
{
    m_impl->hwOverride = mode;
}

HwAccelMode VideoDecoder::activeHwAccel() const
{
    return m_impl->activeHw;
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
    const std::optional<HwAccelMode> hw = m_impl->hwOverride;
    m_impl = std::make_unique<Impl>();
    m_impl->threadCount = threads;
    m_impl->hwOverride = hw;
}

bool VideoDecoder::isOpen() const
{
    return m_impl->isOpen;
}

const MediaInfo& VideoDecoder::info() const
{
    return m_impl->info;
}

std::shared_ptr<const VideoFrame> VideoDecoder::decodeFrame(int64_t timeUs, Accuracy accuracy)
{
    Impl& d = *m_impl;
    if (!d.isOpen) {
        return nullptr;
    }
    if (d.isStill) {
        if (!d.stillFrame) {
            d.stillFrame = VideoFrameFactory::wrapImage(d.still, FrameMeta {});
        }
        d.lastReturnedUs = 0;
        return d.stillFrame;
    }
    // No upper clamp on purpose: container durations can be short of the real last frame
    // (e.g. VFR MP4s). A target past the end seeks to the last keyframe, decodes to EOF and
    // returns the last frame, which stays cached for further past-the-end requests.
    const int64_t ts = std::max(d.usToTs(std::max<int64_t>(timeUs, 0)), d.firstFrameTs);
    if (!d.position(ts, accuracy)) {
        return nullptr;
    }
    std::shared_ptr<const VideoFrame> frame = d.currentVideoFrame();
    if (!frame && d.hwFailed && d.switchToSoftware() && d.position(ts, accuracy)) {
        frame = d.currentVideoFrame();
    }
    if (frame) {
        d.lastReturnedUs = frame->ptsUs;
    }
    return frame;
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
    const int64_t ts = std::max(d.usToTs(std::max<int64_t>(timeUs, 0)), d.firstFrameTs);
    if (!d.position(ts, accuracy)) {
        return {};
    }
    QImage image = d.currentImage(maxSize);
    if (image.isNull() && d.hwFailed && d.switchToSoftware() && d.position(ts, accuracy)) {
        image = d.currentImage(maxSize);
    }
    if (!image.isNull()) {
        d.lastReturnedUs = std::max<int64_t>(0, d.tsToUs(d.cur.pts));
    }
    return image;
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
