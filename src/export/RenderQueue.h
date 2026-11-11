// SPDX-License-Identifier: GPL-3.0-or-later
// RenderQueue — the Deliver page's render queue: jobs (settings + timeline snapshot) run one
// after another on a background thread while the user keeps editing.
#pragma once

#include "export/ExportJob.h"

#include <QList>
#include <QObject>
#include <QString>

#include <memory>

class QThread;

namespace kk {

class Compositor;

/// Sequential background render queue.
///
/// Jobs snapshot the timeline and media table when they are added, so later edits do not
/// change a queued render. "Render All" (startAll()) runs every queued job in order on one
/// worker thread; progress arrives through jobChanged() at most every kProgressIntervalMs.
/// The queue owns one Compositor for exporting (created on first use, on the GUI thread),
/// separate from any viewer so previews do not wait behind export frames.
///
/// Thread-safety: GUI thread only (signals are emitted on the GUI thread).
class RenderQueue : public QObject
{
    Q_OBJECT
public:
    /// Throttle of progress notifications.
    static constexpr int kProgressIntervalMs = 100;

    /// Life cycle of a job.
    enum class Status {
        Queued,
        Rendering,
        Done,
        Failed,
        Cancelled
    };

    /// One job as shown in the queue.
    struct Job
    {
        quint64 id = 0;
        QString name; ///< "Timeline 1 — clip.mp4"
        ExportSettings settings;
        std::shared_ptr<const TimelineData> timeline;
        std::shared_ptr<const MediaPathMap> media;
        /// Proxy / optimized media to render with (Deliver "Use proxy media"); nullptr =
        /// camera originals (the default).
        std::shared_ptr<const ProxyMap> proxies;
        Status status = Status::Queued;
        ExportProgress progress;
        ExportResult result; ///< valid once finished
    };

    explicit RenderQueue(QObject* parent = nullptr);
    /// Cancels a running job and waits for its thread.
    ~RenderQueue() override;

    /// Adds a job (Queued). @return its id.
    quint64 addJob(const QString& name, const ExportSettings& settings,
        std::shared_ptr<const TimelineData> timeline, std::shared_ptr<const MediaPathMap> media,
        std::shared_ptr<const ProxyMap> proxies = nullptr);
    /// Uses @p compositor (owned by the caller, must outlive the queue) instead of creating
    /// one; lets background queues (proxies, render cache) share a single render thread.
    void setCompositor(Compositor* compositor) { m_external = compositor; }
    /// Background queue (proxies, render cache): jobs run at a lower nice value, so playback
    /// and the editor keep their cores while a job runs; an idle machine renders at full speed.
    void setBackground(bool background) { m_background = background; }
    /// Replaces the name, settings, timeline snapshot, media and proxies of job @p id (Resolve's
    /// "Update Job" after editing a job); a finished job is queued again. @return false if the
    /// job renders or is unknown.
    bool updateJob(quint64 id, const QString& name, const ExportSettings& settings,
        std::shared_ptr<const TimelineData> timeline, std::shared_ptr<const MediaPathMap> media,
        std::shared_ptr<const ProxyMap> proxies = nullptr);
    /// Removes a job that is not rendering. @return false if it is rendering or unknown.
    bool removeJob(quint64 id);
    /// Starts rendering every queued job, one after another.
    void startAll();
    /// Renders just job @p id (if queued), then stops.
    void start(quint64 id);
    /// Cancels job @p id: a running job stops (partial output removed), a queued one is
    /// marked Cancelled.
    void cancel(quint64 id);
    /// Cancels the running job and stops processing the queue.
    void stop();
    /// Removes every Done/Failed/Cancelled job.
    void clearFinished();

    /// All jobs in queue order.
    [[nodiscard]] const QList<Job>& jobs() const { return m_jobs; }
    /// Job by id, or nullptr.
    [[nodiscard]] const Job* job(quint64 id) const;
    /// True while a job renders.
    [[nodiscard]] bool isRunning() const { return m_thread != nullptr; }
    /// Human-readable status ("Queued", "Rendering 42%", ...).
    [[nodiscard]] static QString statusText(const Job& job);

    /// Blocks (processing events) until no job runs; for tests and the CLI. @return false on
    /// timeout.
    bool waitForIdle(int timeoutMs);

Q_SIGNALS:
    void jobAdded(quint64 id);
    void jobRemoved(quint64 id);
    /// Status or progress of a job changed.
    void jobChanged(quint64 id);
    /// A job finished (ok, failed or cancelled).
    void jobFinished(quint64 id, bool ok);
    /// The queue started or stopped rendering.
    void runningChanged(bool running);

private:
    Job* findJob(quint64 id);
    void startNext();
    void onProgress(quint64 id, const ExportProgress& progress);
    void onFinished(quint64 id, const ExportResult& result);

    QList<Job> m_jobs;
    quint64 m_nextId = 1;
    bool m_processAll = false; ///< continue with the next queued job after one finishes
    quint64 m_onlyJob = 0; ///< start(id): render only this one
    QThread* m_thread = nullptr; ///< worker of the running job
    std::shared_ptr<ExportJob> m_running;
    quint64 m_runningId = 0;
    std::unique_ptr<Compositor> m_compositor;
    Compositor* m_external = nullptr; ///< setCompositor()
    bool m_background = false; ///< setBackground()
};

} // namespace kk
