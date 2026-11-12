// SPDX-License-Identifier: GPL-3.0-or-later
// RenderQueue — job bookkeeping on the GUI thread, rendering on a worker QThread.
#include "export/RenderQueue.h"

#include "core/MemoryReport.h"
#include "core/ThreadNice.h"
#include "export/ExportLog.h"
#include "render/Compositor.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QThread>

#include <cmath>

namespace kk {

RenderQueue::RenderQueue(QObject* parent)
    : QObject(parent)
{ }

RenderQueue::~RenderQueue()
{
    m_processAll = false;
    if (m_running) {
        m_running->cancel();
    }
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
}

quint64 RenderQueue::addJob(const QString& name, const ExportSettings& settings,
    std::shared_ptr<const TimelineData> timeline, std::shared_ptr<const MediaPathMap> media,
    std::shared_ptr<const ProxyMap> proxies)
{
    Job job;
    job.proxies = std::move(proxies);
    job.id = m_nextId++;
    job.name = name;
    job.settings = settings;
    job.timeline = std::move(timeline);
    job.media = std::move(media);
    m_jobs.push_back(job);
    Q_EMIT jobAdded(job.id);
    return job.id;
}

bool RenderQueue::updateJob(quint64 id, const QString& name, const ExportSettings& settings,
    std::shared_ptr<const TimelineData> timeline, std::shared_ptr<const MediaPathMap> media,
    std::shared_ptr<const ProxyMap> proxies)
{
    Job* job = findJob(id);
    if (!job || job->status == Status::Rendering)
        return false;
    job->name = name;
    job->settings = settings;
    job->timeline = std::move(timeline);
    job->media = std::move(media);
    job->proxies = std::move(proxies);
    job->status = Status::Queued;
    job->progress = {};
    job->result = {};
    Q_EMIT jobChanged(id);
    return true;
}

bool RenderQueue::removeJob(quint64 id)
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].id == id) {
            if (m_jobs[i].status == Status::Rendering) {
                return false;
            }
            m_jobs.removeAt(i);
            Q_EMIT jobRemoved(id);
            return true;
        }
    }
    return false;
}

RenderQueue::Job* RenderQueue::findJob(quint64 id)
{
    for (Job& job : m_jobs) {
        if (job.id == id) {
            return &job;
        }
    }
    return nullptr;
}

const RenderQueue::Job* RenderQueue::job(quint64 id) const
{
    for (const Job& job : m_jobs) {
        if (job.id == id) {
            return &job;
        }
    }
    return nullptr;
}

void RenderQueue::startAll()
{
    m_processAll = true;
    m_onlyJob = 0;
    if (!m_thread) {
        startNext();
    }
}

void RenderQueue::start(quint64 id)
{
    if (m_thread) {
        return; // one job at a time; it will be picked up by startAll() later
    }
    m_processAll = false;
    m_onlyJob = id;
    startNext();
}

void RenderQueue::cancel(quint64 id)
{
    Job* job = findJob(id);
    if (!job) {
        return;
    }
    if (job->status == Status::Rendering && m_running && m_runningId == id) {
        m_running->cancel(); // onFinished() updates the status
    } else if (job->status == Status::Queued) {
        job->status = Status::Cancelled;
        Q_EMIT jobChanged(id);
    }
}

void RenderQueue::stop()
{
    m_processAll = false;
    if (m_running) {
        m_running->cancel();
    }
}

void RenderQueue::clearFinished()
{
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        const Status s = m_jobs[i].status;
        if (s == Status::Done || s == Status::Failed || s == Status::Cancelled) {
            const quint64 id = m_jobs[i].id;
            m_jobs.removeAt(i);
            Q_EMIT jobRemoved(id);
        }
    }
}

void RenderQueue::startNext()
{
    Job* next = nullptr;
    for (Job& job : m_jobs) {
        if (job.status == Status::Queued && (m_onlyJob == 0 || job.id == m_onlyJob)) {
            next = &job;
            break;
        }
    }
    if (!next) {
        m_processAll = false;
        m_onlyJob = 0;
        Q_EMIT runningChanged(false);
        return;
    }
    const bool wasIdle = !m_running;
    if (next->settings.hasVideo() && !m_compositor && !m_external) {
        m_compositor = std::make_unique<Compositor>(); // GUI thread (QOffscreenSurface)
    }
    next->status = Status::Rendering;
    next->progress = {};
    m_runningId = next->id;
    m_running = std::make_shared<ExportJob>(next->settings, next->timeline, next->media);
    m_running->setCompositor(m_external ? m_external : m_compositor.get());
    m_running->setProxies(next->proxies);

    const quint64 id = next->id;
    std::shared_ptr<ExportJob> job = m_running;
    QPointer<RenderQueue> self(this);
    const bool background = m_background;
    m_thread = QThread::create([job, id, self, background] {
        if (background)
            yieldToOtherThreads(10);
        QElapsedTimer throttle;
        throttle.start();
        bool lastLow = false;
        const ExportResult result = job->run([&](const ExportProgress& p) {
            // Throttled hand-over to the GUI thread (queued call; dropped if the queue died).
            if (throttle.elapsed() < kProgressIntervalMs && p.framesDone < p.totalFrames
                && p.memoryLow == lastLow) {
                return;
            }
            lastLow = p.memoryLow;
            throttle.restart();
            QMetaObject::invokeMethod(
                self,
                [self, id, p] {
                    if (self) {
                        self->onProgress(id, p);
                    }
                },
                Qt::QueuedConnection);
        });
        QMetaObject::invokeMethod(
            self,
            [self, id, result] {
                if (self) {
                    self->onFinished(id, result);
                }
            },
            Qt::QueuedConnection);
    });
    m_thread->setObjectName(QStringLiteral("kk-export"));
    m_thread->start(QThread::LowPriority); // the editor stays responsive
    Q_EMIT jobChanged(id);
    if (wasIdle) {
        Q_EMIT runningChanged(true);
    }
}

void RenderQueue::onProgress(quint64 id, const ExportProgress& progress)
{
    if (Job* job = findJob(id); job && job->status == Status::Rendering) {
        job->progress = progress;
        Q_EMIT jobChanged(id);
    }
}

void RenderQueue::onFinished(quint64 id, const ExportResult& result)
{
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
    m_running.reset();
    m_runningId = 0;
    qCInfo(lcExport).noquote() << "memory after the render"
                               << (result.ok                 ? "(finished):"
                                          : result.cancelled ? "(cancelled):"
                                                             : "(failed):")
                               << memreport::oneLine()
                               << Compositor::liveStats().join(QLatin1Char(';'));
    if (Job* job = findJob(id)) {
        job->result = result;
        job->status = result.ok ? Status::Done
            : result.cancelled  ? Status::Cancelled
                                : Status::Failed;
        if (result.ok) {
            job->progress.framesDone = job->progress.totalFrames = result.frames;
            job->progress.etaSeconds = 0;
        }
        Q_EMIT jobChanged(id);
    }
    Q_EMIT jobFinished(id, result.ok);
    if (m_processAll) {
        startNext();
    } else {
        m_onlyJob = 0;
        Q_EMIT runningChanged(false);
    }
}

QString RenderQueue::statusText(const Job& job)
{
    auto tr = [](const char* text) { return QCoreApplication::translate("kk::RenderQueue", text); };
    switch (job.status) {
    case Status::Queued:
        return tr("Queued");
    case Status::Rendering: {
        if (job.progress.memoryLow)
            return tr("Paused: the computer is nearly out of memory. Close other programs; "
                      "the render goes on by itself.");
        QString text = tr("Rendering %1%").arg(int(std::floor(job.progress.fraction() * 100)));
        if (job.progress.fps > 0) {
            text += QStringLiteral(" · %1 fps").arg(job.progress.fps, 0, 'f', 1);
        }
        if (job.progress.etaSeconds >= 0) {
            const int eta = int(std::ceil(job.progress.etaSeconds));
            text += tr(" · %1:%2 left").arg(eta / 60).arg(eta % 60, 2, 10, QLatin1Char('0'));
        }
        return text;
    }
    case Status::Done: {
        const int secs = int(std::round(job.result.seconds));
        return tr("Done in %1:%2 (%3 fps)")
            .arg(secs / 60)
            .arg(secs % 60, 2, 10, QLatin1Char('0'))
            .arg(job.result.averageFps(), 0, 'f', 1);
    }
    case Status::Failed:
        return tr("Failed: %1").arg(job.result.error);
    case Status::Cancelled:
        return tr("Cancelled");
    }
    return {};
}

bool RenderQueue::waitForIdle(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (m_thread || m_processAll) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

} // namespace kk
