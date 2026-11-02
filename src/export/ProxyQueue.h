// SPDX-License-Identifier: GPL-3.0-or-later
// ProxyQueue — background generation of proxy / optimized media (Media Pool > Generate
// Proxy Media / Generate Optimized Media): one job at a time on a low-priority worker thread,
// progress per media item, cancel, linking when a job completes.
#pragma once

#include "export/ProxyMedia.h"
#include "export/RenderQueue.h"

#include <QHash>
#include <QObject>

namespace kk {

class Compositor;
class MediaLinks;

/// Queue of proxy / optimized media jobs (ProxyJobSpec) over a private RenderQueue.
///
/// A job renders into ProxyJobSpec::partialPath; when it succeeds the file replaces
/// outputPath (atomic rename), a sidecar is written (MediaLinks::writeSidecar(), lets other
/// projects link it automatically) and the link is set in the MediaLinks. Failed or
/// cancelled jobs leave nothing behind. Adding a job for an item that already has one of
/// the same kind queued or running is ignored.
///
/// Thread-safety: GUI thread only.
class ProxyQueue : public QObject
{
    Q_OBJECT
public:
    /// Links finished jobs into @p links (must outlive the queue).
    explicit ProxyQueue(MediaLinks* links, QObject* parent = nullptr);
    ~ProxyQueue() override;

    /// Renders with @p compositor (shared background render thread) instead of an own one.
    void setCompositor(Compositor* compositor) { m_queue.setCompositor(compositor); }

    /// Queues @p spec and starts processing. @return false if one is already pending.
    bool enqueue(const ProxyJobSpec& spec);
    /// Cancels the job of item @p mediaId / @p kind (queued or running).
    void cancel(const QString& mediaId, LinkKind kind);
    /// Cancels everything.
    void cancelAll();

    /// True while a job for @p mediaId / @p kind is queued or running.
    [[nodiscard]] bool isPending(const QString& mediaId, LinkKind kind) const;
    /// 0..1 progress of that job; -1 when none is pending; 0 while queued.
    [[nodiscard]] double progress(const QString& mediaId, LinkKind kind) const;
    /// Number of queued + running jobs.
    [[nodiscard]] int pendingCount() const { return int(m_specs.size()); }
    /// Blocks (processing events) until nothing is pending; tests. @return false on timeout.
    bool waitForIdle(int timeoutMs);

    /// Deletes the linked file of @p mediaId / @p kind (and its sidecar) and removes the
    /// link (Media Pool > Delete Proxy Media). Files outside @p managedFolder are only
    /// unlinked, never deleted (a proxy linked by hand belongs to the user).
    static void deleteLinked(
        MediaLinks* links, const QString& mediaId, LinkKind kind, const QString& managedFolder);

Q_SIGNALS:
    /// A job of item @p mediaId was queued, progressed, finished or was cancelled.
    void changed(const QString& mediaId);
    /// A job finished; @p ok = linked. @p error is empty when cancelled.
    void finished(const QString& mediaId, LinkKind kind, bool ok, const QString& error);
    /// Something is (not) being generated.
    void runningChanged(bool running);

private:
    void onJobChanged(quint64 id);
    void onJobFinished(quint64 id, bool ok);
    [[nodiscard]] quint64 jobOf(const QString& mediaId, LinkKind kind) const;

    MediaLinks* m_links;
    RenderQueue m_queue;
    QHash<quint64, ProxyJobSpec> m_specs; ///< RenderQueue job id -> spec
};

} // namespace kk
