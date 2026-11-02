// SPDX-License-Identifier: GPL-3.0-or-later
// RenderCacheManager — Playback > Render Cache (None / Smart / User): keeps the chunk keys of
// the active timeline up to date, renders wanted chunks in the background when the user is
// idle, publishes the RenderCacheView the playback engines use and the red / blue status the
// timeline ruler shows.
#pragma once

#include "render/RenderCacheKeys.h"

#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVector>

#include <memory>
#include <vector>

namespace kk {

class Compositor;
class Project;
class RenderQueue;

/// Background render cache of the active timeline.
///
/// **Keys** (render/RenderCacheKeys.h): recomputed whenever the active timeline, the media
/// pool or the cache settings change (coalesced to one computation per event loop turn).
/// A chunk is *cached* when "<folder>/<key>.mov" exists. Because the key is a hash of
/// everything that can change the chunk's pictures, an edit never needs to delete anything:
/// it changes the key, the old file is no longer looked up (and is evicted later).
///
/// **Background rendering**: after backgroundCacheDelaySec of idleness (no edit, no
/// playback: setPaused()), the wanted chunk nearest after the playhead (then the first
/// before it) is rendered from the camera originals at timeline resolution in the render
/// cache format (export pipeline, private RenderQueue, low priority), written to a partial
/// file and renamed into place, then the next one. An edit restarts the idle timer but does
/// not abort a chunk in progress (its content is still valid for its key); playback aborts
/// it (setPaused(true)).
///
/// **Disk**: the folder is shared by all projects (content-addressed); when it grows beyond
/// prefs::renderCacheLimitGiB() the oldest files are deleted first.
///
/// Thread-safety: GUI thread only.
class RenderCacheManager : public QObject
{
    Q_OBJECT
public:
    /// State of a frame range for the ruler.
    enum class RangeState {
        Uncached, ///< wanted but not cached yet (red)
        Cached ///< on disk (blue)
    };
    struct Range
    {
        int64_t start = 0;
        int64_t end = 0;
        RangeState state = RangeState::Uncached;
    };

    /// Follows @p project (its active timeline, pool and cache settings). @p folder = where
    /// chunks live (default prefs::renderCacheDirectory()).
    explicit RenderCacheManager(Project* project, QString folder = {}, QObject* parent = nullptr);
    ~RenderCacheManager() override;

    /// Renders with @p compositor (shared background render thread) instead of an own one.
    void setCompositor(Compositor* compositor);
    /// Pauses background rendering (playback running) and aborts the chunk in progress.
    void setPaused(bool paused);
    [[nodiscard]] bool isPaused() const { return m_paused; }
    /// Changes the chunk folder (Preferences > Media Storage); rescans it.
    void setFolder(const QString& folder);
    [[nodiscard]] const QString& folder() const { return m_folder; }

    /// Cache view of the current snapshot (never null; empty when the mode is None).
    [[nodiscard]] std::shared_ptr<const RenderCacheView> view() const { return m_view; }
    /// Chunks of the current snapshot (keys, wanted flags).
    [[nodiscard]] const std::vector<CacheChunk>& chunks() const { return m_chunks; }
    /// Merged ranges for the ruler: wanted chunks (red / blue), and cached ones even when no
    /// longer wanted (blue). Empty when the mode is None.
    [[nodiscard]] const QVector<Range>& ranges() const { return m_ranges; }
    /// True if timeline frame @p frame is cached.
    [[nodiscard]] bool isCached(int64_t frame) const;
    /// True while a chunk renders.
    [[nodiscard]] bool isRendering() const { return m_runningJob != 0; }

    /// Recomputes keys now (normally automatic). Tests call it after edits.
    void refresh();
    /// Renders every wanted chunk now, ignoring the idle delay and the background setting
    /// (Playback > Render Cache > Render Cache Now; tests). Pauses are respected.
    void renderAllNow();
    /// Blocks (processing events) until nothing renders or is wanted; tests.
    bool waitForIdle(int timeoutMs);
    /// Deletes every chunk file in the folder (Playback > Delete Render Cache).
    void deleteAll();

Q_SIGNALS:
    /// view() changed (new snapshot or a chunk finished): hand it to the playback engines.
    void viewChanged(std::shared_ptr<const RenderCacheView> view);
    /// ranges() changed: repaint the ruler.
    void rangesChanged();

private:
    void scheduleRefresh();
    void rebuildView();
    void restartIdle();
    void startNext();
    void onJobFinished(quint64 id, bool ok);
    void scanFolder();
    void evict();
    [[nodiscard]] CacheKeyOptions options() const;
    [[nodiscard]] const CacheChunk* nextWanted() const;

    Project* m_project;
    QString m_folder;
    std::unique_ptr<RenderQueue> m_queue;
    Compositor* m_compositor = nullptr;
    QTimer m_refreshTimer; ///< zero-delay coalescing of key recomputations
    QTimer m_idleTimer; ///< background caching delay
    bool m_paused = false;
    bool m_renderAll = false; ///< renderAllNow() in progress
    bool m_mediaKeysDirty = true;
    MediaKeyMap m_mediaKeys;
    std::shared_ptr<const TimelineData> m_snapshot;
    std::vector<CacheChunk> m_chunks;
    QSet<QByteArray> m_present; ///< keys with a file in m_folder
    QSet<QByteArray> m_failed; ///< keys whose render failed this session (not retried)
    std::shared_ptr<const RenderCacheView> m_view;
    QVector<Range> m_ranges;
    quint64 m_runningJob = 0;
    QByteArray m_runningKey;
    QString m_runningPartial;
};

} // namespace kk
