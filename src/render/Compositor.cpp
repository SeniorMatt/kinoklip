// SPDX-License-Identifier: GPL-3.0-or-later
// Compositor — request queue + render thread around a RenderBackend (GPU or CPU).
#include "render/Compositor.h"

#include "render/RenderBackend.h"
#include "render/RenderLog.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QThread>

#include <condition_variable>
#include <deque>
#include <mutex>

namespace kk {

using render::RenderBackend;
using render::RenderJob;

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
    std::shared_ptr<const MediaPathMap> defaultMedia;

    /// Body of the render thread: initialise, then serve the queue until stopped.
    void run(bool forceCpu, std::promise<void>& ready)
    {
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
                wake.wait(lock, [this] { return stopping || !queue.empty(); });
                if (queue.empty()) // stopping and drained
                    break;
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
            backend->render(job);
            // Keep the newest readback in flight only while more work is waiting: its DMA then
            // overlaps the next frame's rendering instead of stalling this thread.
            backend->finishPending(moreQueued);
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
    m_impl->thread.reset(QThread::create([impl, forceCpu, &ready] { impl->run(forceCpu, ready); }));
    m_impl->thread->setObjectName(QStringLiteral("kk.render"));
    m_impl->thread->start();
    readyFuture.wait();
}

Compositor::~Compositor()
{
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->stopping = true;
    }
    m_impl->wake.notify_all();
    m_impl->thread->wait();
    m_impl->surface.reset(); // GUI thread, after the context using it is gone
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
    auto future = job.promise.get_future();
    {
        std::lock_guard lock(m_impl->mutex);
        job.media = job.request.media ? job.request.media : m_impl->defaultMedia;
        m_impl->queue.push_back(std::move(job));
    }
    m_impl->wake.notify_one();
    return future;
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
