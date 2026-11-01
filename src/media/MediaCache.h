// SPDX-License-Identifier: GPL-3.0-or-later
// MediaCache — asynchronous, cached thumbnails, poster frames and waveforms for the UI.
#pragma once

#include "media/WaveformData.h"

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <utility>

namespace kk::media {

class VideoDecoder;

/// Process-wide cache of UI-sized media previews. Every getter returns immediately: either
/// the cached result, or null/nullptr after scheduling the work on a background thread pool;
/// when the work is done the matching signal fires and the getter then returns the result.
///
/// ```cpp
/// connect(&MediaCache::instance(), &MediaCache::thumbnailReady, this, [this] { update(); });
/// const QImage thumb = MediaCache::instance().thumbnail(path, timeUs, 64);
/// if (!thumb.isNull()) painter.drawImage(rect, thumb);   // else: draw a placeholder
/// ```
///
/// Caching: results live in a memory LRU (bounded) and on disk under
/// `QStandardPaths::CacheLocation + "/media"`, keyed by file path + modification time + size,
/// so they survive restarts and are recomputed when a file changes. Identical in-flight
/// requests are deduplicated; failures (unreadable file, no audio...) are remembered and not
/// retried until invalidate() or clearMemoryCache().
///
/// Thread-safety: the getters may be called from any thread; signals are emitted in the
/// thread the cache lives in (the main thread). Background work never blocks the caller.
/// The singleton is owned by the QCoreApplication and shut down when it quits.
class MediaCache : public QObject
{
    Q_OBJECT

public:
    /// The process-wide instance. Requires an existing Q(Core)Application.
    static MediaCache& instance();

    ~MediaCache() override;

    /// Thumbnail of the frame at media time `timeUs` (µs), `height` pixels high (width follows
    /// the display aspect ratio). To raise the hit rate the request is snapped (see
    /// snapThumbnailTimeUs() / snapThumbnailHeight()), so the image may be up to 7 px taller
    /// than asked and show a frame up to 250 ms away — draw it scaled into your target rect.
    /// @return the cached image, or a null QImage → thumbnailReady(path) follows later.
    QImage thumbnail(const QString& path, int64_t timeUs, int height);

    /// Representative image for media pool icons: the frame at 10 % of the duration (or the
    /// still image itself), `height` pixels high (snapped like thumbnail()).
    /// @return the cached image, or a null QImage → posterFrameReady(path) follows later.
    QImage posterFrame(const QString& path, int height);

    /// Waveform peaks of the file's audio (WaveformData::peaksPerSecond = 100).
    /// @return the cached data, or nullptr → waveformReady(path) follows (unless the file has
    /// no audio, in which case it stays nullptr).
    std::shared_ptr<const WaveformData> waveform(const QString& path);

    /// Time actually used for a thumbnail request: rounded to a 0.5 s grid for heights up to
    /// 135 px (timeline strips) and to a 0.1 s grid above (hover/scrub previews). Stills → 0.
    static int64_t snapThumbnailTimeUs(int64_t timeUs, int height);

    /// Height actually rendered for a request: rounded up to a multiple of 8, within 8..2160.
    static int snapThumbnailHeight(int height);

    /// Root directory of the on-disk cache (default `CacheLocation/media`).
    QString cacheDirectory() const;

    /// Moves the on-disk cache (e.g. a temporary dir in tests). Existing memory entries stay.
    void setCacheDirectory(const QString& directory);

    /// Drops all in-memory results, remembered failures and idle decoders (disk is kept).
    void clearMemoryCache();

    /// Forgets everything cached in memory for `path` (call after the file changed).
    void invalidate(const QString& path);

    /// Cancels queued work and waits for running jobs; afterwards all getters only return
    /// already cached results. Called automatically when the application quits.
    void shutdown();

signals:
    /// A thumbnail for `path` became available; call thumbnail() again.
    void thumbnailReady(const QString& path);
    /// A poster frame for `path` became available; call posterFrame() again.
    void posterFrameReady(const QString& path);
    /// The waveform of `path` became available; call waveform() again.
    void waveformReady(const QString& path);

private:
    enum class ImageKind {
        Thumbnail,
        Poster,
    };
    /// One image job: what to render and its memory-cache key.
    struct ImageRequest
    {
        ImageKind kind = ImageKind::Thumbnail;
        QString path;
        int64_t timeUs = 0;
        int height = 0;
        QString key;
    };

    explicit MediaCache(QObject* parent = nullptr);

    QImage requestImage(const ImageRequest& request);
    void runImageJob(const ImageRequest& request);
    QImage renderImage(const ImageRequest& request, bool* hasAlpha);
    void finishImage(const ImageRequest& request, const QImage& image);
    void runWaveformJob(const QString& path);
    void finishWaveform(const QString& path, const std::shared_ptr<const WaveformData>& data);
    std::unique_ptr<VideoDecoder> acquireDecoder(const QString& path);
    void releaseDecoder(const QString& path, std::unique_ptr<VideoDecoder> decoder);
    QString diskDirectory(const QString& path, const char* kind) const;

    mutable QMutex m_mutex; ///< guards everything below except the pools / decoder list
    QCache<QString, QImage> m_images;
    QCache<QString, std::shared_ptr<const WaveformData>> m_waveforms;
    QSet<QString> m_inFlight;
    QSet<QString> m_failed;
    QString m_cacheDirectory;

    std::atomic<bool> m_cancelled {false};
    QThreadPool m_imagePool; ///< thumbnails/posters: short jobs
    QThreadPool m_waveformPool; ///< waveforms: long jobs, kept apart so they cannot starve images

    std::mutex m_decoderMutex;
    std::list<std::pair<QString, std::unique_ptr<VideoDecoder>>> m_idleDecoders; ///< MRU first
};

} // namespace kk::media
