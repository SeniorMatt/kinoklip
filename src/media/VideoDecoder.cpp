// SPDX-License-Identifier: GPL-3.0-or-later
// VideoDecoder — opening files (software or hardware decoding, with fall-back), turning the
// current frame into a VideoFrame/QImage, and the public API. Seeking: VideoDecoderSeek.cpp.
#include "media/VideoDecoder.h"

#include "media/MediaLog.h"
#include "media/VideoDecoderImpl.h"

#include <QFileInfo>
#include <QSet>
#include <QThread>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace kk::media {

using namespace detail;

namespace {

/// Software decoding threads when the caller does not choose: the target machines are
/// 4-core/8-thread laptops that also composite, mix audio and run the UI.
constexpr int kMaxAutoSoftwareThreads = 4;

/// Hardware decoders do the work on the GPU; extra CPU threads only add latency.
constexpr int kHardwareDecodeThreads = 1;

/// Used to synthesise timestamps when a file has none.
constexpr AVRational kFallbackFrameRate {25, 1};

void setError(QString* error, const QString& message)
{
    if (error) {
        *error = message;
    }
}

/// True the first time `key` is seen in this process: decoders are opened many times per file
/// (render pool, thumbnails, export), the log should say things once per file.
bool firstTime(const QString& key)
{
    static std::mutex mutex;
    static QSet<QString> seen;
    const std::lock_guard lock(mutex);
    if (seen.contains(key)) {
        return false;
    }
    seen.insert(key);
    return true;
}

} // namespace

VideoDecoder::Impl::~Impl()
{
    freeCodec();
}

void VideoDecoder::Impl::freeCodec()
{
    // Frames first (they return surfaces to the pool), then the codec (which destroys the VA
    // context and, with the last reference, the surface pool) - all under the device lock.
    const auto lock = lockHw();
    releaseAll();
    spareFrames.clear();
    if (scratch) {
        av_frame_unref(scratch.get());
    }
    codec.reset();
}

// ---- Opening --------------------------------------------------------------------------------

bool VideoDecoder::Impl::openStill(const QString& path, const ImageHeader& header, QString* error)
{
    // Shared process-wide cache: re-opening the same still (every clip of a slideshow, every
    // render worker) costs a hash lookup instead of a decode.
    const LoadedImage loaded = ImageLoader::loadCached(path);
    if (loaded.image.isNull()) {
        setError(error, loaded.error);
        return false;
    }
    info = stillInfo(path, header);
    info.width = info.displayWidth = loaded.image.width();
    info.height = info.displayHeight = loaded.image.height();
    info.hasAlpha = loaded.hasAlpha;
    still = loaded.image;
    isStill = true;
    return true;
}

bool VideoDecoder::Impl::openFrameList(
    const QString& path, MediaRoute route, const ImageHeader& header, QString* error)
{
    auto list = std::make_unique<ImageFrameList>();
    const bool ok = route == MediaRoute::Sequence ? list->openSequence(path, error)
                                                  : list->openAnimation(path, header, error);
    if (!ok) {
        return false;
    }
    info = list->info();
    frameList = std::move(list);
    // Decode frame 0 now, like openVideo(): validates the source and fixes the real size.
    if (!frameListPosition(0)) {
        setError(error, QStringLiteral("Cannot decode the first frame"));
        frameList.reset();
        return false;
    }
    info.width = info.displayWidth = frameListImage.width();
    info.height = info.displayHeight = frameListImage.height();
    return true;
}

bool VideoDecoder::Impl::frameListPosition(int64_t timeUs)
{
    const int index = frameList->indexAt(timeUs);
    if (index == frameListIndex) {
        return true;
    }
    QImage image = frameList->frame(index);
    if (image.isNull()) {
        return false;
    }
    if (image.cacheKey() != frameListImage.cacheKey()) {
        // A new picture (not a held frame in a sequence gap): the scaled copy is stale.
        frameListScaled = QImage();
    }
    frameListVideo.reset(); // re-wrapped on demand (no pixel copy) with this frame's pts
    frameListImage = std::move(image);
    frameListIndex = index;
    return true;
}

QImage VideoDecoder::Impl::scaledCopy(
    const QImage& image, QSize maxSize, QImage& scaled, QSize& scaledFor)
{
    const QSize out = fitSize(image.size(), maxSize);
    if (out == image.size()) {
        return image;
    }
    if (scaled.isNull() || scaledFor != maxSize) {
        scaled = image.scaled(out, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        scaledFor = maxSize;
    }
    return scaled;
}

bool VideoDecoder::Impl::openCodec(HwAccelMode requested, QString* error)
{
    freeCodec();
    activeHw = HwAccelMode::Off;
    hwLock = nullptr;
    hwState = {};
    hwFailed = false;
    hwFailReason.clear();
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
        hwLock = HwDeviceManager::instance().callLock(mode);
        {
            const auto lock = lockHw();
            codec = openDecoder(stream, kHardwareDecodeThreads, nullptr, device, &hwState);
        }
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
        hwLock = nullptr;
    }
    const int threads = threadCount > 0
        ? threadCount
        : std::min(kMaxAutoSoftwareThreads, QThread::idealThreadCount());
    codec = openDecoder(stream, threads, error);
    return codec != nullptr;
}

bool VideoDecoder::Impl::switchToSoftware(bool duringOpen)
{
    const HwAccelMode failed = activeHw;
    const QString name = hwAccelName(failed);
    const bool rejected = hwState.rejected; // openCodec() below resets it
    QString why;
    if (rejected) {
        why = QStringLiteral("the GPU cannot decode this stream");
    } else {
        why = hwFailReason.isEmpty() ? QStringLiteral("decoding error") : hwFailReason;
        // A rejected profile is not the device's fault; real failures count towards
        // disabling the backend for the session.
        HwDeviceManager::instance().reportFailure(failed, why);
    }
    const int64_t atTs = cur ? cur.pts : lastDecodedTs;
    const int64_t atUs = atTs != AV_NOPTS_VALUE ? std::max<int64_t>(0, tsToUs(atTs)) : 0;
    // The demuxer position is left as is; with no current frame the next request seeks.
    const bool ok = openCodec(HwAccelMode::Off, nullptr);
    if (duringOpen) {
        hwOpenFailure = QStringLiteral("%1 failed: %2").arg(name, why);
        hwOpenFailed = !rejected;
    } else {
        const QString line = QStringLiteral("%1 decoding failed for %2 at %3 s (%4); continuing "
                                            "in software (%5 threads)")
                                 .arg(name, QFileInfo(info.path).fileName())
                                 .arg(double(atUs) / 1e6, 0, 'f', 3)
                                 .arg(why)
                                 .arg(codec ? codec->thread_count : 0);
        if (firstTime(QStringLiteral("fallback|") + name + QLatin1Char('|') + info.path)) {
            qCWarning(lcMedia).noquote() << line;
        } else {
            qCDebug(lcMedia).noquote() << line;
        }
    }
    return ok;
}

void VideoDecoder::Impl::logOpened()
{
    QString profile;
    if (const char* p
        = avcodec_profile_name(stream->codecpar->codec_id, stream->codecpar->profile)) {
        profile = QLatin1Char(' ') + QString::fromUtf8(p).toLower();
    }
    const QString with = activeHw != HwAccelMode::Off
        ? HwDeviceManager::instance().describe(activeHw)
        : QStringLiteral("software (%1 threads)").arg(codec ? codec->thread_count : 0);
    QString line = QStringLiteral("decoding %1 (%2%3 %4x%5) with %6")
                       .arg(QFileInfo(info.path).fileName(),
                           QString::fromUtf8(avcodec_get_name(stream->codecpar->codec_id)), profile)
                       .arg(info.width)
                       .arg(info.height)
                       .arg(with);
    if (!hwOpenFailure.isEmpty()) {
        line += QStringLiteral("; ") + hwOpenFailure;
    }
    if (!firstTime(QStringLiteral("open|%1|%2|%3").arg(info.path, with, hwOpenFailure))) {
        qCDebug(lcMedia).noquote() << line;
    } else if (hwOpenFailed) {
        qCWarning(lcMedia).noquote() << line;
    } else {
        qCInfo(lcMedia).noquote() << line;
    }
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
        if (!ok && !hwFailed && hwFailReason.isEmpty()) {
            hwFailReason = QStringLiteral("no frame decoded");
        }
        ok = switchToSoftware(true) && seek(firstTs, 0) && decodeOne(cur);
    }
    if (!ok) {
        setError(error, QStringLiteral("No decodable video frames"));
        return false;
    }
    firstFrameTs = cur.pts;

    // Clip geometry is fixed at open (MediaInfo::displayWidth is the contract for frameAt()
    // sizes): every frame uses the container's sample aspect ratio, or — if the container
    // declares none — the first frame's. Some decoders report a different SAR on later frames
    // (H.263 CIF switches to 12:11 after a seek), which must not resize the clip.
    const AVRational frameSar = cur.av->sample_aspect_ratio;
    if (stream->sample_aspect_ratio.num <= 0 && stream->codecpar->sample_aspect_ratio.num <= 0
        && frameSar.num > 0 && frameSar.den > 0) {
        AVRational sar = frameSar;
        av_reduce(&sar.num, &sar.den, sar.num, sar.den, INT16_MAX);
        info.sampleAspectRatio = {sar.num, sar.den};
        info.displayWidth = int(std::lround(double(info.width) * sar.num / sar.den));
        info.displayHeight = info.height;
        if (info.rotation == 90 || info.rotation == 270) {
            std::swap(info.displayWidth, info.displayHeight);
        }
    }

    if (info.isStillImage) {
        // A single image FFmpeg can read but Qt cannot (e.g. DPX/EXR): keep it as a still.
        still = currentImage({});
        if (still.isNull()) {
            setError(error, QStringLiteral("Cannot convert image"));
            return false;
        }
        isStill = true;
        freeCodec();
        fmt.reset();
        return true;
    }
    logOpened();
    return true;
}

QImage VideoDecoder::Impl::stillAt(QSize maxSize)
{
    return scaledCopy(still, maxSize, stillScaled, stillScaledFor);
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
        meta.sampleAspectRatio = info.sampleAspectRatio; // fixed per clip, see openVideo()
        meta.rotation = info.rotation;
        const bool hwFrame = cur.av->hw_frames_ctx != nullptr;
        {
            const auto lock = hwFrame ? lockHw() : std::unique_lock<std::mutex>();
            cur.video = factory.make(cur.av.get(), meta);
            if (!cur.video && hwFrame) {
                // One retry: a transient failure (driver busy) succeeds now; a broken surface
                // fails again immediately and the caller re-decodes the frame in software.
                const QString first = factory.lastErrorString();
                cur.video = factory.make(cur.av.get(), meta);
                if (cur.video) {
                    qCDebug(lcMedia).noquote()
                        << "hardware frame download succeeded on retry after:" << first;
                }
            }
        }
        if (!cur.video && activeHw != HwAccelMode::Off) {
            hwFailed = true; // download failed twice
            hwFailReason = QStringLiteral("frame download: ") + factory.lastErrorString();
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
    const AVRational sar {info.sampleAspectRatio.num, info.sampleAspectRatio.den};
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
    ImageHeader header;
    const MediaRoute route = routeFor(path, &header);
    switch (route) {
    case MediaRoute::Sequence:
    case MediaRoute::Animation:
        ok = m_impl->openFrameList(path, route, header, error);
        break;
    case MediaRoute::Still:
        ok = m_impl->openStill(path, header, error);
        break;
    case MediaRoute::FFmpeg:
        break;
    }
    if (!ok && route != MediaRoute::Sequence) {
        // FFmpeg is the universal fall-back (also for images the loaders rejected).
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
    if (d.frameList) {
        if (!d.frameListPosition(std::max<int64_t>(timeUs, 0))) {
            return nullptr;
        }
        if (!d.frameListVideo) {
            FrameMeta meta;
            meta.ptsUs = d.frameList->startUs(d.frameListIndex);
            meta.durationUs = d.frameList->durationUs(d.frameListIndex);
            d.frameListVideo = VideoFrameFactory::wrapImage(d.frameListImage, meta);
        }
        d.lastReturnedUs = d.frameList->startUs(d.frameListIndex);
        return d.frameListVideo;
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

bool VideoDecoder::prepareFrame(int64_t timeUs, const std::function<bool()>& stop)
{
    Impl& d = *m_impl;
    d.stopCheck = &stop;
    d.stopped = false;
    const bool ok = decodeFrame(timeUs) != nullptr;
    d.stopCheck = nullptr;
    d.stopped = false;
    return ok;
}

int64_t VideoDecoder::decodedFrameCount() const
{
    return m_impl->decodedFrames;
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
    if (d.frameList) {
        if (!d.frameListPosition(std::max<int64_t>(timeUs, 0))) {
            return {};
        }
        d.lastReturnedUs = d.frameList->startUs(d.frameListIndex);
        return Impl::scaledCopy(d.frameListImage, maxSize, d.frameListScaled, d.frameListScaledFor);
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

} // namespace kk::media