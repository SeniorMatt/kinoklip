// SPDX-License-Identifier: GPL-3.0-or-later
// MediaCache — thread-pool jobs, memory LRU and disk cache for thumbnails/posters/waveforms.
#include "media/MediaCache.h"

#include "media/FFmpegUtil.h"
#include "media/MediaLog.h"
#include "media/MediaSource.h"
#include "media/VideoDecoder.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>

namespace kk::media {

namespace {

/// Memory budgets of the LRU caches, in KiB (QCache cost unit used below).
constexpr int kImageMemoryBudgetKb = 256 * 1024;
constexpr int kWaveformMemoryBudgetKb = 64 * 1024;

/// Thumbnail snapping (see MediaCache::snapThumbnailTimeUs / snapThumbnailHeight).
constexpr int kThumbnailHeightStep = 8;
constexpr int kMinThumbnailHeight = 8;
constexpr int kMaxThumbnailHeight = 2160;
constexpr int kSmallThumbnailMaxHeight = 135;
constexpr int64_t kSmallThumbnailGridUs = 500'000;
constexpr int64_t kLargeThumbnailGridUs = 100'000;

/// Poster frames show the frame at this fraction of the duration (skips black intros).
constexpr int kPosterPositionDivisor = 10;

/// Decoders kept open between thumbnail jobs (sequential thumbnails of one file then decode
/// forward instead of re-opening and seeking). Each costs tens of MB for HD video.
constexpr size_t kMaxIdleDecoders = 4;
/// PreviewDecoding::BySize: pictures up to this many pixels (1920x1200) decode in software.
/// Measured with test_mediaperf filmstripLatency (20 cold 64 px thumbnails): 1080p60 H.264
/// software 0.6 s vs hardware 2.4 s, 2160p30 HEVC software 2.9 s vs hardware 1.3 s.
constexpr int64_t kSoftwarePreviewMaxPixels = int64_t(1920) * 1200;

/// Waveform jobs decode whole files; two in parallel keep the disk and CPU busy enough.
constexpr int kWaveformThreads = 2;

/// JPEG quality of opaque thumbnails on disk (alpha thumbnails are stored as PNG).
constexpr int kThumbnailJpegQuality = 85;

int imageCostKb(const QImage& image)
{
    return int(image.sizeInBytes() / 1024) + 1;
}

/// Loads a disk-cached image stored as either `<base>.jpg` or `<base>.png`.
QImage loadDiskImage(const QString& basePath)
{
    for (const char* suffix : {".jpg", ".png"}) {
        const QString file = basePath + QLatin1String(suffix);
        if (QFileInfo::exists(file)) {
            QImage image(file);
            if (!image.isNull()) {
                return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            }
        }
    }
    return {};
}

void saveDiskImage(const QString& basePath, const QImage& image, bool hasAlpha)
{
    QDir().mkpath(QFileInfo(basePath).absolutePath());
    QSaveFile file(basePath + QLatin1String(hasAlpha ? ".png" : ".jpg"));
    if (!file.open(QIODevice::WriteOnly)
        || !image.save(&file, hasAlpha ? "PNG" : "JPG", hasAlpha ? -1 : kThumbnailJpegQuality)
        || !file.commit()) {
        qCDebug(lcMedia) << "cannot write thumbnail cache file" << file.fileName();
    }
}

} // namespace

// ---- Singleton & lifetime -------------------------------------------------------------------

MediaCache& MediaCache::instance()
{
    static MediaCache* const s_instance = [] {
        QCoreApplication* app = QCoreApplication::instance();
        Q_ASSERT_X(app, "MediaCache::instance", "needs a QCoreApplication");
        auto* cache = new MediaCache;
        if (app) {
            // Live in the main thread even if first used from a worker, so signals are
            // delivered there; owned by the app and shut down before it goes away.
            cache->moveToThread(app->thread());
            cache->setParent(app);
            connect(app, &QCoreApplication::aboutToQuit, cache, &MediaCache::shutdown);
            qAddPostRoutine([] { MediaCache::instance().shutdown(); });
        }
        return cache;
    }();
    return *s_instance;
}

MediaCache::MediaCache(QObject* parent)
    : QObject(parent)
    , m_images(kImageMemoryBudgetKb)
    , m_waveforms(kWaveformMemoryBudgetKb)
    , m_cacheDirectory(
          QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QLatin1String("/media"))
{
    detail::initFfmpeg();
    m_imagePool.setMaxThreadCount(std::max(2, QThread::idealThreadCount() / 2));
    m_waveformPool.setMaxThreadCount(kWaveformThreads);
}

MediaCache::~MediaCache()
{
    shutdown();
}

void MediaCache::shutdown()
{
    m_cancelled = true;
    m_imagePool.clear();
    m_waveformPool.clear();
    m_imagePool.waitForDone();
    m_waveformPool.waitForDone();
    const std::lock_guard lock(m_decoderMutex);
    m_idleDecoders.clear();
}

// ---- Public getters -------------------------------------------------------------------------

int64_t MediaCache::snapThumbnailTimeUs(int64_t timeUs, int height)
{
    const int64_t grid = snapThumbnailHeight(height) <= kSmallThumbnailMaxHeight
        ? kSmallThumbnailGridUs
        : kLargeThumbnailGridUs;
    return (std::max<int64_t>(timeUs, 0) + grid / 2) / grid * grid;
}

int MediaCache::snapThumbnailHeight(int height)
{
    const int rounded
        = (height + kThumbnailHeightStep - 1) / kThumbnailHeightStep * kThumbnailHeightStep;
    return std::clamp(rounded, kMinThumbnailHeight, kMaxThumbnailHeight);
}

QImage MediaCache::thumbnail(const QString& path, int64_t timeUs, int height)
{
    if (path.isEmpty() || height <= 0) {
        return {};
    }
    ImageRequest request;
    request.kind = ImageKind::Thumbnail;
    request.path = path;
    request.height = snapThumbnailHeight(height);
    request.timeUs = detail::isStillImageSuffix(QFileInfo(path).suffix())
        ? 0
        : snapThumbnailTimeUs(timeUs, height);
    request.key = QStringLiteral("t|%1|%2|%3").arg(path).arg(request.height).arg(request.timeUs);
    return requestImage(request);
}

QImage MediaCache::frame(const QString& path, int64_t timeUs, int height)
{
    if (path.isEmpty() || height <= 0) {
        return {};
    }
    ImageRequest request;
    request.kind = ImageKind::Frame;
    request.path = path;
    request.height = snapThumbnailHeight(height);
    request.timeUs
        = detail::isStillImageSuffix(QFileInfo(path).suffix()) ? 0 : std::max<int64_t>(timeUs, 0);
    request.key = QStringLiteral("f|%1|%2|%3").arg(path).arg(request.height).arg(request.timeUs);
    return requestImage(request);
}

QImage MediaCache::posterFrame(const QString& path, int height)
{
    if (path.isEmpty() || height <= 0) {
        return {};
    }
    ImageRequest request;
    request.kind = ImageKind::Poster;
    request.path = path;
    request.height = snapThumbnailHeight(height);
    request.key = QStringLiteral("p|%1|%2").arg(path).arg(request.height);
    return requestImage(request);
}

std::shared_ptr<const WaveformData> MediaCache::waveform(const QString& path)
{
    if (path.isEmpty()) {
        return nullptr;
    }
    QMutexLocker lock(&m_mutex);
    if (const auto* hit = m_waveforms.object(path)) {
        return *hit;
    }
    const QString key = QLatin1String("w|") + path;
    if (m_cancelled || m_failed.contains(key) || m_inFlight.contains(key)) {
        return nullptr;
    }
    m_inFlight.insert(key);
    lock.unlock();
    m_waveformPool.start([this, path] { runWaveformJob(path); });
    return nullptr;
}

QString MediaCache::cacheDirectory() const
{
    const QMutexLocker lock(&m_mutex);
    return m_cacheDirectory;
}

void MediaCache::setCacheDirectory(const QString& directory)
{
    const QMutexLocker lock(&m_mutex);
    m_cacheDirectory = directory;
}

void MediaCache::clearMemoryCache()
{
    {
        const QMutexLocker lock(&m_mutex);
        m_images.clear();
        m_waveforms.clear();
        m_failed.clear();
    }
    const std::lock_guard lock(m_decoderMutex);
    m_idleDecoders.clear();
}

void MediaCache::invalidate(const QString& path)
{
    {
        const QMutexLocker lock(&m_mutex);
        const QString thumbPrefix = QStringLiteral("t|%1|").arg(path);
        const QString posterPrefix = QStringLiteral("p|%1|").arg(path);
        const QString framePrefix = QStringLiteral("f|%1|").arg(path);
        const auto isForPath = [&](const QString& key) {
            return key.startsWith(thumbPrefix) || key.startsWith(posterPrefix)
                || key.startsWith(framePrefix) || key == QLatin1String("w|") + path;
        };
        const QList<QString> keys = m_images.keys();
        for (const QString& key : keys) {
            if (isForPath(key)) {
                m_images.remove(key);
            }
        }
        m_waveforms.remove(path);
        m_failed.removeIf(isForPath);
    }
    const std::lock_guard lock(m_decoderMutex);
    m_idleDecoders.remove_if([&](const auto& entry) { return entry.first == path; });
}

// ---- Image jobs -----------------------------------------------------------------------------

QImage MediaCache::requestImage(const ImageRequest& request)
{
    QMutexLocker lock(&m_mutex);
    if (const QImage* hit = m_images.object(request.key)) {
        return *hit;
    }
    if (m_cancelled || m_failed.contains(request.key) || m_inFlight.contains(request.key)) {
        return {};
    }
    m_inFlight.insert(request.key);
    lock.unlock();
    m_imagePool.start([this, request] { runImageJob(request); });
    return {};
}

void MediaCache::runImageJob(const ImageRequest& request)
{
    QImage image;
    if (!m_cancelled) {
        // Exact frames are transient previews: memory cache only.
        const QString directory
            = request.kind == ImageKind::Frame ? QString() : diskDirectory(request.path, "thumbs");
        QString basePath;
        if (!directory.isEmpty()) {
            basePath = directory
                + (request.kind == ImageKind::Poster
                        ? QStringLiteral("/poster_%1").arg(request.height)
                        : QStringLiteral("/%1_%2").arg(request.height).arg(request.timeUs / 1000));
            image = loadDiskImage(basePath);
        }
        if (image.isNull() && !m_cancelled) {
            bool hasAlpha = false;
            image = renderImage(request, &hasAlpha);
            if (!image.isNull() && !basePath.isEmpty()) {
                saveDiskImage(basePath, image, hasAlpha);
            }
        }
    }
    QMetaObject::invokeMethod(
        this, [this, request, image] { finishImage(request, image); }, Qt::QueuedConnection);
}

QImage MediaCache::renderImage(const ImageRequest& request, bool* hasAlpha)
{
    std::unique_ptr<VideoDecoder> decoder = acquireDecoder(request.path);
    if (!decoder) {
        return {};
    }
    const MediaInfo& info = decoder->info();
    int64_t timeUs = request.timeUs;
    if (request.kind == ImageKind::Poster) {
        timeUs = info.isStillImage ? 0 : info.durationUs / kPosterPositionDivisor;
    }
    const QImage image = decoder->frameAt(timeUs, QSize(0, request.height));
    *hasAlpha = info.hasAlpha;
    releaseDecoder(request.path, std::move(decoder));
    return image;
}

void MediaCache::finishImage(const ImageRequest& request, const QImage& image)
{
    {
        const QMutexLocker lock(&m_mutex);
        m_inFlight.remove(request.key);
        if (image.isNull()) {
            m_failed.insert(request.key);
            return;
        }
        m_images.insert(request.key, new QImage(image), imageCostKb(image));
    }
    if (m_cancelled) {
        // A job that was already running when shutdown() was called: keep its result, but stay
        // silent — receivers may already be gone and "nothing after shutdown" is the contract.
        return;
    }
    if (request.kind == ImageKind::Poster) {
        emit posterFrameReady(request.path);
    } else if (request.kind == ImageKind::Frame) {
        emit frameReady(request.path);
    } else {
        emit thumbnailReady(request.path);
    }
}

std::unique_ptr<VideoDecoder> MediaCache::acquireDecoder(const QString& path)
{
    {
        const std::lock_guard lock(m_decoderMutex);
        const auto it = std::find_if(m_idleDecoders.begin(), m_idleDecoders.end(),
            [&](const auto& entry) { return entry.first == path; });
        if (it != m_idleDecoders.end()) {
            std::unique_ptr<VideoDecoder> decoder = std::move(it->second);
            m_idleDecoders.erase(it);
            return decoder;
        }
    }
    const PreviewDecoding mode = m_previewDecoding.load();
    auto open = [&path](HwAccelMode hw) -> std::unique_ptr<VideoDecoder> {
        auto decoder = std::make_unique<VideoDecoder>();
        decoder->setThreadCount(1); // parallelism comes from the pool, not from each decoder
        decoder->setHwAccel(hw);
        QString error;
        if (!decoder->open(path, &error)) {
            qCDebug(lcMedia) << "no thumbnail for" << path << ":" << error;
            return nullptr;
        }
        return decoder;
    };
    auto decoder = open(mode == PreviewDecoding::Hardware ? HwAccelMode::Auto : HwAccelMode::Off);
    // BySize: a software open is cheap (no device), so large pictures are simply reopened on
    // the hardware decoder, where seeking to a 4K HEVC frame is about twice as fast.
    if (decoder && mode == PreviewDecoding::BySize
        && int64_t(decoder->info().width) * decoder->info().height > kSoftwarePreviewMaxPixels) {
        if (auto hw = open(HwAccelMode::Auto))
            decoder = std::move(hw);
    }
    return decoder;
}

void MediaCache::setPreviewDecoding(PreviewDecoding mode)
{
    m_previewDecoding = mode;
    // Idle decoders keep the backend they were opened with; drop them so the mode applies.
    const std::lock_guard lock(m_decoderMutex);
    m_idleDecoders.clear();
}

void MediaCache::releaseDecoder(const QString& path, std::unique_ptr<VideoDecoder> decoder)
{
    const std::lock_guard lock(m_decoderMutex);
    if (m_cancelled) {
        return;
    }
    m_idleDecoders.emplace_front(path, std::move(decoder));
    while (m_idleDecoders.size() > kMaxIdleDecoders) {
        m_idleDecoders.pop_back();
    }
}

// ---- Waveform jobs --------------------------------------------------------------------------

void MediaCache::runWaveformJob(const QString& path)
{
    std::shared_ptr<WaveformData> data;
    if (!m_cancelled) {
        const QString directory = diskDirectory(path, "waveforms");
        const QString file
            = directory.isEmpty() ? QString() : directory + QStringLiteral("/waveform.cwwf");
        if (!file.isEmpty()) {
            data = loadWaveform(file);
        }
        if (!data) {
            QString error;
            data = computeWaveform(path, &m_cancelled, &error);
            if (!data) {
                qCDebug(lcMedia) << "no waveform for" << path << ":" << error;
            } else if (!file.isEmpty() && QDir().mkpath(directory)) {
                saveWaveform(*data, file);
            }
        }
    }
    std::shared_ptr<const WaveformData> result = std::move(data);
    QMetaObject::invokeMethod(
        this, [this, path, result] { finishWaveform(path, result); }, Qt::QueuedConnection);
}

void MediaCache::finishWaveform(
    const QString& path, const std::shared_ptr<const WaveformData>& data)
{
    {
        const QMutexLocker lock(&m_mutex);
        const QString key = QLatin1String("w|") + path;
        m_inFlight.remove(key);
        if (!data) {
            m_failed.insert(key);
            return;
        }
        const int costKb = data->size() * 4 / 1024 + 1;
        m_waveforms.insert(path, new std::shared_ptr<const WaveformData>(data), costKb);
    }
    if (m_cancelled) {
        return; // finished after shutdown(): cached, but no signal (see finishImage())
    }
    emit waveformReady(path);
}

// ---- Disk layout ----------------------------------------------------------------------------

QString MediaCache::diskDirectory(const QString& path, const char* kind) const
{
    // <cache>/<kind>/<sha1(absolute path, mtime, size)>: a changed file gets a new directory.
    // Embedded media (single-file projects): container, entry, CRC and size instead.
    const MediaSource source = MediaSource::resolve(path);
    if (!source.exists()) {
        return {};
    }
    const QFileInfo fileInfo(path);
    const QByteArray identity = source.isEmbedded() ? source.identity().toUtf8()
                                                    : fileInfo.absoluteFilePath().toUtf8() + '\n'
            + QByteArray::number(fileInfo.lastModified().toMSecsSinceEpoch()) + '\n'
            + QByteArray::number(fileInfo.size());
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha1).toHex().left(24));
    return cacheDirectory() + QLatin1Char('/') + QLatin1String(kind) + QLatin1Char('/') + hash;
}

} // namespace kk::media
