// SPDX-License-Identifier: GPL-3.0-or-later
// ImageLoader — backend selection by extension with fall-back, the Qt (QImageReader) backend,
// and the process-wide LRU cache of decoded stills.
#include "media/ImageLoader.h"

#include "media/ImageBackends.h"
#include "media/MediaLog.h"
#include "media/MediaSource.h"
#include "media/SupportedFormats.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMutex>
#include <QPixelFormat>

#include <array>
#include <atomic>
#include <list>
#include <memory>
#include <mutex>

namespace kk::media {

using namespace detail;

namespace {

/// Default memory budget of the still cache. A 4000x3000 photo takes 46 MiB as ARGB32, so this
/// holds a handful of full photos or hundreds of small graphics.
constexpr int64_t kDefaultCacheLimitBytes = 256LL * 1024 * 1024;

/// QImageReader refuses images whose decoded size exceeds its allocation limit (Qt default
/// 256 MiB, i.e. ~67 MP). Photos and panoramas can be bigger; allow up to 1 GiB.
constexpr int kQtAllocationLimitMiB = 1024;

std::atomic<bool> s_rawFullSize {false};

/// Extensions whose first backend is not Qt. Everything else in SupportedFormats' Image
/// category starts with Qt (it has plugins for the common formats and is the fastest path).
ImageBackend preferredBackend(const QString& suffix)
{
    static const QHash<QString, ImageBackend> kBySuffix = [] {
        QHash<QString, ImageBackend> map;
        for (const char* s : {"heic", "heif", "hif", "avif"}) {
            map.insert(QString::fromLatin1(s), ImageBackend::Heif);
        }
        // tga: Qt's plugin reads only uncompressed true-colour TGA; FFmpeg also does RLE and
        // colour-mapped files (most real-world TGAs are RLE).
        for (const char* s :
            {"jxl", "exr", "dpx", "psd", "qoi", "pcx", "sgi", "rgb", "jp2", "j2k", "tga"}) {
            map.insert(QString::fromLatin1(s), ImageBackend::FFmpeg);
        }
        for (const QString& s : SupportedFormats::extensions(MediaCategory::RawImage)) {
            map.insert(s, ImageBackend::Raw);
        }
        return map;
    }();
    return kBySuffix.value(suffix, ImageBackend::Qt);
}

/// Backends in the order they are tried for `path`: the preferred one, then the fall-backs.
std::array<ImageBackend, 4> backendOrder(const QString& path)
{
    const ImageBackend first = preferredBackend(QFileInfo(path).suffix().toLower());
    std::array<ImageBackend, 4> order {first};
    int n = 1;
    for (ImageBackend b :
        {ImageBackend::Qt, ImageBackend::Heif, ImageBackend::FFmpeg, ImageBackend::Raw}) {
        if (b != first) {
            order[n++] = b;
        }
    }
    return order;
}

bool readHeaderWith(ImageBackend backend, const QString& path, ImageHeader& header)
{
    switch (backend) {
    case ImageBackend::Qt:
        return readHeaderQt(path, header);
    case ImageBackend::Heif:
        return readHeaderHeif(path, header);
    case ImageBackend::FFmpeg:
        return readHeaderFFmpeg(path, header);
    case ImageBackend::Raw:
        return readHeaderRaw(path, s_rawFullSize, header);
    case ImageBackend::None:
        break;
    }
    return false;
}

bool loadWith(ImageBackend backend, const QString& path, LoadedImage& result)
{
    switch (backend) {
    case ImageBackend::Qt:
        return loadQt(path, result);
    case ImageBackend::Heif:
        return loadHeif(path, result);
    case ImageBackend::FFmpeg:
        return loadFFmpeg(path, result);
    case ImageBackend::Raw:
        return loadRaw(path, s_rawFullSize, result);
    case ImageBackend::None:
        break;
    }
    return false;
}

// ---- Still cache ------------------------------------------------------------------------------
//
// Key → decoded image, with an LRU list for eviction by byte budget. QImage is implicitly
// shared, so a hit hands out a reference to the cached pixels without copying.

struct CacheEntry
{
    LoadedImage loaded;
    int64_t bytes = 0;
    std::list<QString>::iterator lruPosition;
};

struct StillCache
{
    QMutex mutex;
    QHash<QString, CacheEntry> entries;
    std::list<QString> lru; ///< most recently used first
    int64_t bytes = 0;
    int64_t limit = kDefaultCacheLimitBytes;

    /// Drops least recently used entries until the budget is met. Caller holds `mutex`.
    void trim()
    {
        while (bytes > limit && !lru.empty()) {
            const auto it = entries.find(lru.back());
            bytes -= it->bytes;
            entries.erase(it);
            lru.pop_back();
        }
    }
};

StillCache& stillCache()
{
    static StillCache cache;
    return cache;
}

/// Identity of a decoded still: absolute path + mtime + size (a changed file misses) + RAW mode.
QString cacheKey(const QString& path)
{
    return MediaSource::resolve(path).identity()
        + (s_rawFullSize ? QLatin1String("|f") : QLatin1String("|h"));
}

} // namespace

// ---- Qt backend -----------------------------------------------------------------------------

namespace detail {

QImage toPremultipliedArgb(QImage image)
{
    if (!image.isNull() && image.format() != QImage::Format_ARGB32_Premultiplied) {
        image.convertTo(QImage::Format_ARGB32_Premultiplied);
    }
    return image;
}

namespace {

void raiseQtAllocationLimit()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (QImageReader::allocationLimit() < kQtAllocationLimitMiB) {
            QImageReader::setAllocationLimit(kQtAllocationLimitMiB);
        }
    });
}

} // namespace

bool readHeaderQt(const QString& path, ImageHeader& header)
{
    raiseQtAllocationLimit();
    QImageReader reader;
    std::unique_ptr<QIODevice> device; // embedded media: the entry's bytes
    setImageReaderSource(reader, path, device);
    reader.setAutoTransform(true);
    if (!reader.canRead()) {
        return false;
    }
    QSize size = reader.size();
    QImage::Format format = reader.imageFormat();
    if (!size.isValid()) {
        // Some plugins only know the size after decoding.
        const QImage image = reader.read();
        if (image.isNull()) {
            return false;
        }
        size = image.size(); // read() with auto-transform: already upright
        format = image.format();
    } else if (reader.transformation() & QImageIOHandler::TransformationRotate90) {
        size.transpose(); // EXIF orientation 5..8: the upright image is transposed
    }
    header.valid = true;
    header.size = size;
    header.format = QString::fromLatin1(reader.format());
    header.backend = ImageBackend::Qt;
    header.hasAlpha = format != QImage::Format_Invalid
        && QImage::toPixelFormat(format).alphaUsage() == QPixelFormat::UsesAlpha;
    header.frameCount = reader.supportsAnimation() ? std::max(0, reader.imageCount()) : 1;
    header.animated = header.frameCount > 1;
    return true;
}

bool loadQt(const QString& path, LoadedImage& result)
{
    raiseQtAllocationLimit();
    QImageReader reader;
    std::unique_ptr<QIODevice> device;
    setImageReaderSource(reader, path, device);
    reader.setAutoTransform(true); // EXIF orientation
    QImage image = reader.read();
    if (image.isNull()) {
        result.error = reader.errorString();
        return false;
    }
    result.hasAlpha = image.hasAlphaChannel();
    result.image = toPremultipliedArgb(std::move(image));
    result.backend = ImageBackend::Qt;
    return true;
}

} // namespace detail

// ---- Public API -------------------------------------------------------------------------------

QString imageBackendName(ImageBackend backend)
{
    switch (backend) {
    case ImageBackend::Qt:
        return QStringLiteral("qt");
    case ImageBackend::Heif:
        return QStringLiteral("heif");
    case ImageBackend::FFmpeg:
        return QStringLiteral("ffmpeg");
    case ImageBackend::Raw:
        return QStringLiteral("raw");
    case ImageBackend::None:
        break;
    }
    return QStringLiteral("none");
}

bool ImageLoader::handlesExtension(const QString& path)
{
    const MediaCategory category = SupportedFormats::categoryFor(path);
    return category == MediaCategory::Image || category == MediaCategory::RawImage;
}

ImageHeader ImageLoader::readHeader(const QString& path)
{
    ImageHeader header;
    if (!mediaExists(path)) {
        header.error = QStringLiteral("File not found: %1").arg(path);
        return header;
    }
    for (const ImageBackend backend : backendOrder(path)) {
        ImageHeader candidate;
        if (readHeaderWith(backend, path, candidate) && candidate.valid
            && !candidate.size.isEmpty()) {
            return candidate;
        }
    }
    header.error = QStringLiteral("Unsupported or damaged image");
    return header;
}

LoadedImage ImageLoader::load(const QString& path)
{
    LoadedImage result;
    if (!mediaExists(path)) {
        result.error = QStringLiteral("File not found: %1").arg(path);
        return result;
    }
    QString firstError;
    for (const ImageBackend backend : backendOrder(path)) {
        LoadedImage attempt;
        if (loadWith(backend, path, attempt) && !attempt.image.isNull()) {
            return attempt;
        }
        if (firstError.isEmpty()) {
            firstError = attempt.error; // the preferred backend's reason is the useful one
        }
    }
    result.error
        = firstError.isEmpty() ? QStringLiteral("Unsupported or damaged image") : firstError;
    qCDebug(lcMedia) << "cannot load image" << path << result.error;
    return result;
}

LoadedImage ImageLoader::loadCached(const QString& path)
{
    const QString key = cacheKey(path);
    StillCache& cache = stillCache();
    {
        QMutexLocker lock(&cache.mutex);
        const auto it = cache.entries.find(key);
        if (it != cache.entries.end()) {
            cache.lru.splice(cache.lru.begin(), cache.lru, it->lruPosition); // mark as recent
            return it->loaded;
        }
    }
    // Decode outside the lock: other threads keep hitting the cache meanwhile. Two threads
    // missing on the same file decode it twice; the second insert is then dropped.
    LoadedImage loaded = load(path);
    if (loaded.image.isNull()) {
        return loaded; // failures are not cached: the file may appear or be fixed later
    }
    const int64_t bytes = loaded.image.sizeInBytes();
    QMutexLocker lock(&cache.mutex);
    if (bytes <= cache.limit && !cache.entries.contains(key)) {
        cache.lru.push_front(key);
        cache.entries.insert(key, CacheEntry {loaded, bytes, cache.lru.begin()});
        cache.bytes += bytes;
        cache.trim();
    }
    return loaded;
}

void ImageLoader::setRawFullSize(bool fullSize)
{
    s_rawFullSize = fullSize;
}

bool ImageLoader::rawFullSize()
{
    return s_rawFullSize;
}

void ImageLoader::setCacheLimitBytes(int64_t bytes)
{
    StillCache& cache = stillCache();
    QMutexLocker lock(&cache.mutex);
    cache.limit = std::max<int64_t>(0, bytes);
    cache.trim();
}

void ImageLoader::clearCache()
{
    StillCache& cache = stillCache();
    QMutexLocker lock(&cache.mutex);
    cache.entries.clear();
    cache.lru.clear();
    cache.bytes = 0;
}

int ImageLoader::cachedImageCount()
{
    StillCache& cache = stillCache();
    QMutexLocker lock(&cache.mutex);
    return int(cache.entries.size());
}

int64_t ImageLoader::cachedBytes()
{
    StillCache& cache = stillCache();
    QMutexLocker lock(&cache.mutex);
    return cache.bytes;
}

bool ImageLoader::hasHeifSupport()
{
#ifdef KK_HAVE_LIBHEIF
    return true;
#else
    return false;
#endif
}

bool ImageLoader::hasRawSupport()
{
#ifdef KK_HAVE_LIBRAW
    return true;
#else
    return false;
#endif
}

} // namespace kk::media
