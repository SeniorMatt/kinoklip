// SPDX-License-Identifier: GPL-3.0-or-later
// RenderCacheManager — keys, background chunk rendering, view and ruler ranges.
#include "export/RenderCacheManager.h"

#include "core/Preferences.h"
#include "core/Project.h"
#include "export/ProxyMedia.h"
#include "export/RenderQueue.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <algorithm>

namespace kk {

namespace {

/// Key recomputations are coalesced: a drag emits Timeline::changed for every mouse move.
constexpr int kRefreshDelayMs = 30;
constexpr qint64 kBytesPerGiB = qint64(1) << 30;
const QLatin1String kPartialSuffix(".partial.mov");

} // namespace

RenderCacheManager::RenderCacheManager(Project* project, QString folder, QObject* parent)
    : QObject(parent)
    , m_project(project)
    , m_folder(folder.isEmpty() ? prefs::renderCacheDirectory() : std::move(folder))
    , m_queue(std::make_unique<RenderQueue>())
    , m_view(std::make_shared<RenderCacheView>())
{
    m_queue->setBackground(true);
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(kRefreshDelayMs);
    connect(&m_refreshTimer, &QTimer::timeout, this, &RenderCacheManager::refresh);
    m_idleTimer.setSingleShot(true);
    connect(&m_idleTimer, &QTimer::timeout, this, &RenderCacheManager::startNext);
    connect(m_queue.get(), &RenderQueue::jobFinished, this, &RenderCacheManager::onJobFinished);
    connect(m_queue.get(), &RenderQueue::jobChanged, this, [this](quint64 id) {
        const RenderQueue::Job* job = m_queue->job(id);
        if (id == m_runningJob && job && job->status == RenderQueue::Status::Cancelled)
            onJobFinished(id, false); // cancelled before it started: no jobFinished()
    });

    connect(m_project->timeline(), &Timeline::changed, this, [this] {
        scheduleRefresh();
        restartIdle();
    });
    connect(m_project->mediaPool(), &MediaPool::changed, this, [this] {
        m_mediaKeysDirty = true;
        scheduleRefresh();
    });
    connect(m_project, &Project::cacheSettingsChanged, this, [this] {
        scheduleRefresh();
        restartIdle();
    });
    connect(m_project, &Project::projectReset, this, [this] {
        m_mediaKeysDirty = true;
        m_failed.clear();
        scheduleRefresh();
    });
    scanFolder();
    refresh();
}

RenderCacheManager::~RenderCacheManager()
{
    if (m_runningJob)
        m_queue->cancel(m_runningJob);
    m_queue.reset(); // waits for the worker
    if (!m_runningPartial.isEmpty())
        QFile::remove(m_runningPartial);
}

void RenderCacheManager::setCompositor(Compositor* compositor)
{
    m_compositor = compositor;
    m_queue->setCompositor(compositor);
}

void RenderCacheManager::setPaused(bool paused)
{
    if (paused == m_paused)
        return;
    m_paused = paused;
    if (paused) {
        m_idleTimer.stop();
        if (m_runningJob)
            m_queue->cancel(m_runningJob);
    } else {
        restartIdle();
    }
}

void RenderCacheManager::setFolder(const QString& folder)
{
    const QString f = folder.isEmpty() ? prefs::renderCacheDirectory() : folder;
    if (f == m_folder)
        return;
    m_folder = f;
    scanFolder();
    rebuildView();
}

CacheKeyOptions RenderCacheManager::options() const
{
    const MediaCacheSettings& s = m_project->cacheSettings();
    CacheKeyOptions o;
    o.mode = s.renderCacheMode;
    o.userTransitions = s.userCacheTransitions;
    o.userComposites = s.userCacheComposites;
    o.format = s.renderCacheFormat;
    return o;
}

void RenderCacheManager::scheduleRefresh()
{
    m_refreshTimer.start();
}

void RenderCacheManager::refresh()
{
    m_refreshTimer.stop();
    if (m_mediaKeysDirty) {
        m_mediaKeys = makeMediaKeys(m_project->mediaPool()->state());
        m_mediaKeysDirty = false;
    }
    m_snapshot = m_project->timeline()->snapshot();
    const CacheKeyOptions o = options();
    if (o.mode == RenderCacheMode::None || !m_snapshot)
        m_chunks.clear();
    else
        m_chunks = renderCacheChunks(*m_snapshot, m_mediaKeys, o);
    rebuildView();
}

void RenderCacheManager::rebuildView()
{
    m_view = makeRenderCacheView(m_snapshot, m_chunks, m_present, m_folder);
    // Ruler ranges: wanted chunks red / blue, other cached chunks blue; neighbours merged.
    m_ranges.clear();
    for (const CacheChunk& c : m_chunks) {
        const bool cached = m_present.contains(c.key);
        if (!c.wanted && !cached)
            continue;
        const RangeState state = cached ? RangeState::Cached : RangeState::Uncached;
        if (!m_ranges.isEmpty() && m_ranges.back().end == c.start && m_ranges.back().state == state)
            m_ranges.back().end = c.end;
        else
            m_ranges.append({c.start, c.end, state});
    }
    emit viewChanged(m_view);
    emit rangesChanged();
}

bool RenderCacheManager::isCached(int64_t frame) const
{
    return m_view && m_view->find(frame) != nullptr;
}

void RenderCacheManager::restartIdle()
{
    const MediaCacheSettings& s = m_project->cacheSettings();
    if (m_paused || s.renderCacheMode == RenderCacheMode::None || !s.backgroundCache) {
        m_idleTimer.stop();
        return;
    }
    m_idleTimer.start(s.backgroundCacheDelaySec * 1000);
}

const CacheChunk* RenderCacheManager::nextWanted() const
{
    const int64_t playhead = m_project->timeline()->playhead();
    const CacheChunk* before = nullptr;
    for (const CacheChunk& c : m_chunks) {
        if (!c.wanted || m_present.contains(c.key) || m_failed.contains(c.key))
            continue;
        if (c.end > playhead)
            return &c; // first wanted chunk at / after the playhead
        if (!before)
            before = &c;
    }
    return before;
}

void RenderCacheManager::startNext()
{
    if (m_runningJob || m_paused)
        return;
    if (m_refreshTimer.isActive())
        refresh(); // never render chunks of an outdated key set
    const CacheChunk* chunk = nextWanted();
    if (!chunk || !m_snapshot) {
        m_renderAll = false;
        return;
    }
    const IntermediateFormat* f
        = findIntermediateFormat(m_project->cacheSettings().renderCacheFormat);
    if (!f)
        f = &renderCacheFormats().front();
    ExportSettings s
        = intermediateSettings(*f, QSize(m_snapshot->width, m_snapshot->height), false);
    s.range = RangeMode::Custom;
    s.rangeIn = chunk->start;
    s.rangeOut = chunk->end;
    QDir().mkpath(m_folder);
    m_runningKey = chunk->key;
    m_runningPartial = renderCacheFile(m_folder, chunk->key) + kPartialSuffix;
    s.outputPath = m_runningPartial;
    // Chunks are rendered from the camera originals (no proxies): full quality.
    m_runningJob = m_queue->addJob(QString::fromLatin1(chunk->key), s, m_snapshot,
        makeMediaPathMap(m_project->mediaPool()->state()));
    m_queue->startAll();
}

void RenderCacheManager::onJobFinished(quint64 id, bool ok)
{
    if (id != m_runningJob)
        return;
    const RenderQueue::Job* job = m_queue->job(id);
    const bool cancelled = job && job->status == RenderQueue::Status::Cancelled;
    const QString finalPath = renderCacheFile(m_folder, m_runningKey);
    if (ok) {
        QFile::remove(finalPath);
        if (QFile::rename(m_runningPartial, finalPath))
            m_present.insert(m_runningKey);
        else
            ok = false;
    }
    if (!ok && !cancelled)
        m_failed.insert(m_runningKey); // do not loop on a chunk that cannot be rendered
    QFile::remove(m_runningPartial);
    m_queue->removeJob(id);
    m_runningJob = 0;
    m_runningKey.clear();
    m_runningPartial.clear();
    if (ok) {
        evict();
        rebuildView();
    }
    // Continue while idle (or rendering everything); a pause stops here.
    if (!m_paused && (m_renderAll || !m_idleTimer.isActive()) && !cancelled) {
        const MediaCacheSettings& st = m_project->cacheSettings();
        if (m_renderAll || (st.backgroundCache && st.renderCacheMode != RenderCacheMode::None))
            QTimer::singleShot(0, this, &RenderCacheManager::startNext);
    }
}

void RenderCacheManager::renderAllNow()
{
    m_renderAll = true;
    refresh();
    startNext();
}

bool RenderCacheManager::waitForIdle(int timeoutMs)
{
    QDeadlineTimer deadline(timeoutMs);
    while (m_runningJob || (m_renderAll && nextWanted())) {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

void RenderCacheManager::scanFolder()
{
    m_present.clear();
    QDirIterator it(m_folder, {QStringLiteral("*.mov")}, QDir::Files);
    while (it.hasNext()) {
        const QString name = QFileInfo(it.next()).fileName();
        if (name.endsWith(kPartialSuffix)) {
            QFile::remove(it.filePath()); // left over from a crash
            continue;
        }
        m_present.insert(name.left(name.size() - 4).toLatin1());
    }
}

void RenderCacheManager::evict()
{
    const qint64 limit = qint64(prefs::renderCacheLimitGiB()) * kBytesPerGiB;
    QFileInfoList files = QDir(m_folder).entryInfoList(
        {QStringLiteral("*.mov")}, QDir::Files, QDir::Time | QDir::Reversed); // oldest first
    qint64 total = 0;
    for (const QFileInfo& f : files)
        total += f.size();
    for (const QFileInfo& f : files) {
        if (total <= limit)
            break;
        const QByteArray key = f.completeBaseName().toLatin1();
        if (key == m_runningKey)
            continue;
        total -= f.size();
        QFile::remove(f.absoluteFilePath());
        m_present.remove(key);
    }
}

void RenderCacheManager::deleteAll()
{
    if (m_runningJob)
        m_queue->cancel(m_runningJob);
    QDirIterator it(m_folder, {QStringLiteral("*.mov")}, QDir::Files);
    while (it.hasNext())
        QFile::remove(it.next());
    m_present.clear();
    m_failed.clear();
    rebuildView();
    restartIdle();
}

} // namespace kk
