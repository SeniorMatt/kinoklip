// SPDX-License-Identifier: GPL-3.0-or-later
// Compositor — request queue + render thread around a RenderBackend (GPU or CPU).
#include "render/Compositor.h"

#include "core/ThreadNice.h"
#include "render/DecodeTargets.h"
#include "render/RenderBackend.h"
#include "render/RenderLog.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QThread>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace kk {

using render::RenderBackend;

namespace {
constexpr int kBackgroundNice = 10; ///< ~10 % of a core's share against normal threads
}
using render::RenderJob;

namespace {
std::mutex g_liveMutex;
std::vector<const Compositor*>& liveCompositors()
{
    static std::vector<const Compositor*> list;
    return list;
}
} // namespace

struct Compositor::Impl
{
    std::unique_ptr<QOffscreenSurface> surface; ///< GUI-thread object used by the GL context
    std::unique_ptr<QThread> thread;
    std::unique_ptr<RenderBackend> backend; ///< created, used and destroyed on `thread`
    QString description;
    bool gpu = false;

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<RenderJob> queue;
    bool stopping = false;
    std::atomic<int> statDecoders {0};
    std::atomic<int> statTitles {0};
    std::atomic<int64_t> statTargetBytes {0};
    std::atomic<int> statSlots {0};
    std::atomic<int> statSlotsInUse {0};
    void publishStats()
    {
        const render::ResourceStats s = backend->resources();
        statDecoders = s.decoders;
        statTitles = s.titleTextures;
        statTargetBytes = s.targetBytes;
        statSlots = s.outputSlots;
        statSlotsInUse = s.outputSlotsInUse;
    }
    std::chrono::milliseconds idleRelease {15000}; ///< see Options::idleReleaseMs
    std::shared_ptr<const MediaPathMap> defaultMedia;

    /// Look-ahead request: the decoders that frames of the range will need and the frames before
    /// them do not have, on the render thread (cheap: a look at the timeline per frame, the
    /// decoding runs on the backend's worker threads).
    void prepareDecoders(const RenderJob& job)
    {
        const RenderRequest& rq = job.request;
        const PrepareRange& range = *rq.prepare;
        if (!rq.timeline)
            return;
        // A frame inside a cached chunk is decoded from the chunk's file (see renderAsync()).
        const RenderCacheView* cache = rq.renderCache.get();
        if (cache && cache->snapshot != rq.timeline)
            cache = nullptr;
        render::DecoderPool& pool = backend->decoders();
        std::vector<render::DecodeTarget> before;
        std::vector<render::DecodeTarget> now;
        auto collect = [&](int64_t frame, std::vector<render::DecodeTarget>& out) {
            out.clear();
            const RenderCacheChunk* chunk = cache ? cache->find(frame) : nullptr;
            const TimelineData& tl = chunk ? *chunk->timeline : *rq.timeline;
            render::collectDecodeTargets(tl, frame, tl.nested,
                chunk ? chunk->media.get() : job.media.get(), chunk ? nullptr : rq.proxies.get(),
                out);
        };
        if (range.previous >= 0)
            collect(range.previous, before);
        for (int64_t frame = range.first; frame < range.end; ++frame) {
            collect(frame, now);
            const double dueMs = range.firstDueMs + double(frame - range.first) * range.frameMs;
            for (const render::DecodeTarget& target : now) {
                if (!render::continuesFrom(target, before))
                    pool.prepare(target.path, target.timeUs, dueMs, rq.cancelled);
            }
            before.swap(now);
        }
    }

    /// Body of the render thread: initialise, then serve the queue until stopped.
    void run(bool forceCpu, bool background, std::promise<void>& ready)
    {
        if (background)
            yieldToOtherThreads(kBackgroundNice);
        if (!forceCpu && surface) {
            backend = render::createGlBackend(surface.get());
            QString why;
            if (!backend->initialize(&why)) {
                qCWarning(lcRender).noquote()
                    << "OpenGL compositor unavailable (" + why + "); using the CPU fallback";
                backend.reset();
            }
        }
        if (!backend) {
            backend = render::createCpuBackend();
            QString why;
            (void)backend->initialize(&why);
        }
        gpu = backend->isGpu();
        description = backend->description();
        qCInfo(lcRender).noquote() << "compositor:" << description;
        ready.set_value();

        bool released = true; // nothing to give back before the first request
        for (;;) {
            RenderJob job;
            bool moreQueued = false;
            {
                std::unique_lock lock(mutex);
                // With deferred readbacks, do not sleep: finish them first when idle.
                if (queue.empty() && !stopping && backend->hasPending()) {
                    lock.unlock();
                    backend->finishPending(false);
                    continue;
                }
                const auto ready = [this] { return stopping || !queue.empty(); };
                if (released || idleRelease.count() <= 0) {
                    wake.wait(lock, ready);
                } else if (!wake.wait_for(lock, idleRelease, ready)) {
                    // Nothing to do for a while: give the GPU memory back (see releaseIdle()).
                    lock.unlock();
                    backend->releaseIdle();
                    publishStats();
                    released = true;
                    continue;
                }
                if (queue.empty()) // stopping and drained
                    break;
                released = false;
                job = std::move(queue.front());
                queue.pop_front();
                moreQueued = !queue.empty();
            }
            if (job.request.cancelled && job.request.cancelled->load(std::memory_order_relaxed)) {
                auto skipped = std::make_shared<RenderedFrame>();
                skipped->frame = job.request.frame;
                skipped->cancelled = true;
                job.promise.set_value(std::move(skipped));
                continue;
            }
            if (job.request.prepare) {
                prepareDecoders(job);
                job.promise.set_value(std::make_shared<RenderedFrame>());
                continue;
            }
            backend->render(job);
            // Keep the newest readback in flight only while more work is waiting: its DMA then
            // overlaps the next frame's rendering instead of stalling this thread.
            backend->finishPending(moreQueued);
            publishStats();
        }
        backend->finishPending(false);
        backend->shutdown();
        backend.reset();
    }
};

Compositor::Compositor()
    : Compositor(Options {})
{ }

Compositor::Compositor(Options options)
    : m_impl(std::make_unique<Impl>())
{
    bool forceCpu = options.forceCpu
        || qEnvironmentVariable("KINOKLIP_RENDER")
                .compare(QLatin1String("cpu"), Qt::CaseInsensitive)
            == 0;
    // QOffscreenSurface is a platform window on some platforms: create it on the GUI thread.
    if (!forceCpu && qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        m_impl->surface = std::make_unique<QOffscreenSurface>();
        m_impl->surface->setFormat(format);
        m_impl->surface->create();
        if (!m_impl->surface->isValid())
            m_impl->surface.reset();
    }

    std::promise<void> ready;
    auto readyFuture = ready.get_future();
    Impl* impl = m_impl.get();
    impl->idleRelease = std::chrono::milliseconds(options.idleReleaseMs);
    m_impl->thread.reset(QThread::create([impl, forceCpu, background = options.background, &ready] {
        impl->run(forceCpu, background, ready);
    }));
    m_impl->thread->setObjectName(QStringLiteral("kk.render"));
    m_impl->thread->start();
    readyFuture.wait();
    const std::lock_guard live(g_liveMutex);
    liveCompositors().push_back(this);
}

Compositor::~Compositor()
{
    {
        const std::lock_guard live(g_liveMutex);
        std::erase(liveCompositors(), this);
    }
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->stopping = true;
    }
    m_impl->wake.notify_all();
    m_impl->thread->wait();
    m_impl->surface.reset(); // GUI thread, after the context using it is gone
}

QStringList Compositor::liveStats()
{
    QStringList out;
    const std::lock_guard live(g_liveMutex);
    int n = 0;
    for (const Compositor* c : liveCompositors()) {
        const render::ResourceStats s = c->resourceStats();
        out.append(QStringLiteral("Compositor %1 (%2): %3 decoders, %4 title textures, %5 MiB of "
                                  "render targets")
                       .arg(++n)
                       .arg(c->isGpu() ? QStringLiteral("GPU") : QStringLiteral("CPU"))
                       .arg(s.decoders)
                       .arg(s.titleTextures)
                       .arg(double(s.targetBytes) / (1 << 20), 0, 'f', 0));
    }
    return out;
}

render::ResourceStats Compositor::resourceStats() const
{
    return {m_impl->statDecoders, m_impl->statTitles, m_impl->statTargetBytes, m_impl->statSlots,
        m_impl->statSlotsInUse};
}

bool Compositor::isGpu() const
{
    return m_impl->gpu;
}

QString Compositor::backendDescription() const
{
    return m_impl->description;
}

void Compositor::setMediaPaths(std::shared_ptr<const MediaPathMap> media)
{
    std::lock_guard lock(m_impl->mutex);
    m_impl->defaultMedia = std::move(media);
}

std::future<RenderedFramePtr> Compositor::renderAsync(RenderRequest request)
{
    RenderJob job;
    job.request = std::move(request);
    // Render cache hit: show the cached chunk (one media clip of the chunk file) instead of
    // the timeline. Only for the exact snapshot the view was made for (pointer identity), so
    // an edit (= new snapshot) can never show an outdated picture.
    if (const RenderCacheView* view = job.request.renderCache.get(); view && view->snapshot
        && !job.request.prepare && view->snapshot == job.request.timeline
        && !job.request.compositionNode && !job.request.bypassGrades && job.request.dynamicZoom) {
        if (const RenderCacheChunk* chunk = view->find(job.request.frame)) {
            job.request.timeline = chunk->timeline;
            job.request.media = chunk->media;
            job.request.proxies = nullptr;
            job.fromCache = true;
        }
    }
    if (!job.request.prepare) // only the look-ahead below still needs it; drop the reference
        job.request.renderCache = nullptr;
    auto future = job.promise.get_future();
    {
        std::lock_guard lock(m_impl->mutex);
        job.media = job.request.media ? job.request.media : m_impl->defaultMedia;
        m_impl->queue.push_back(std::move(job));
    }
    m_impl->wake.notify_one();
    return future;
}

std::future<RenderedFramePtr> Compositor::renderCompositionNode(
    std::shared_ptr<const TimelineData> timeline, quint64 clipId, quint64 nodeId, int64_t compFrame,
    QSize outSize, Quality quality, unsigned outputs, std::shared_ptr<const MediaPathMap> media,
    std::shared_ptr<const ProxyMap> proxies)
{
    RenderRequest request;
    request.timeline = std::move(timeline);
    request.media = std::move(media);
    request.proxies = std::move(proxies);
    request.outSize = outSize;
    request.quality = quality;
    request.outputs = outputs;
    request.subtitles = false;
    request.compositionNode = CompositionNodeView {clipId, nodeId, compFrame};
    return renderAsync(std::move(request));
}

render::DecoderPool::PrepareStats Compositor::prepareStats() const
{
    return m_impl->backend ? m_impl->backend->decoders().prepareStats()
                           : render::DecoderPool::PrepareStats {};
}

RenderedFramePtr Compositor::render(std::shared_ptr<const TimelineData> timeline, int64_t frame,
    QSize outSize, Quality quality, unsigned outputs)
{
    RenderRequest request;
    request.timeline = std::move(timeline);
    request.frame = frame;
    request.outSize = outSize;
    request.quality = quality;
    request.outputs = outputs;
    return renderAsync(std::move(request)).get();
}

QImage Compositor::renderImage(
    std::shared_ptr<const TimelineData> timeline, int64_t frame, QSize outSize, Quality quality)
{
    const RenderedFramePtr result
        = render(std::move(timeline), frame, outSize, quality, OutputImage);
    return result && result->ok ? result->image : QImage();
}

} // namespace kk
