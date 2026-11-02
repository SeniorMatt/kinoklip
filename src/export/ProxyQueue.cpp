// SPDX-License-Identifier: GPL-3.0-or-later
// ProxyQueue — proxy / optimized media jobs, linking on completion.
#include "export/ProxyQueue.h"

#include "core/MediaLinks.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

namespace kk {

ProxyQueue::ProxyQueue(MediaLinks* links, QObject* parent)
    : QObject(parent)
    , m_links(links)
{
    connect(&m_queue, &RenderQueue::jobChanged, this, &ProxyQueue::onJobChanged);
    connect(&m_queue, &RenderQueue::jobFinished, this, &ProxyQueue::onJobFinished);
    connect(&m_queue, &RenderQueue::runningChanged, this, &ProxyQueue::runningChanged);
}

ProxyQueue::~ProxyQueue()
{
    cancelAll();
}

quint64 ProxyQueue::jobOf(const QString& mediaId, LinkKind kind) const
{
    for (auto it = m_specs.cbegin(); it != m_specs.cend(); ++it) {
        if (it->mediaId == mediaId && it->kind == kind)
            return it.key();
    }
    return 0;
}

bool ProxyQueue::enqueue(const ProxyJobSpec& spec)
{
    if (jobOf(spec.mediaId, spec.kind) != 0)
        return false;
    QDir().mkpath(QFileInfo(spec.outputPath).absolutePath());
    const quint64 id = m_queue.addJob(
        QFileInfo(spec.outputPath).fileName(), spec.settings, spec.timeline, spec.media);
    m_specs.insert(id, spec);
    m_queue.startAll();
    emit changed(spec.mediaId);
    return true;
}

void ProxyQueue::cancel(const QString& mediaId, LinkKind kind)
{
    if (const quint64 id = jobOf(mediaId, kind))
        m_queue.cancel(id); // queued: Cancelled at once; running: finishes soon
}

void ProxyQueue::cancelAll()
{
    const QList<quint64> ids = m_specs.keys();
    for (const quint64 id : ids)
        m_queue.cancel(id);
}

bool ProxyQueue::isPending(const QString& mediaId, LinkKind kind) const
{
    return jobOf(mediaId, kind) != 0;
}

double ProxyQueue::progress(const QString& mediaId, LinkKind kind) const
{
    const quint64 id = jobOf(mediaId, kind);
    if (id == 0)
        return -1.0;
    const RenderQueue::Job* job = m_queue.job(id);
    return job ? job->progress.fraction() : 0.0;
}

bool ProxyQueue::waitForIdle(int timeoutMs)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!m_specs.isEmpty()) {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

void ProxyQueue::onJobChanged(quint64 id)
{
    const auto it = m_specs.constFind(id);
    if (it == m_specs.constEnd())
        return;
    const RenderQueue::Job* job = m_queue.job(id);
    if (job && job->status == RenderQueue::Status::Cancelled) {
        // A queued job cancelled before it ran never gets jobFinished().
        onJobFinished(id, false);
        return;
    }
    emit changed(it->mediaId);
}

void ProxyQueue::onJobFinished(quint64 id, bool ok)
{
    const auto it = m_specs.find(id);
    if (it == m_specs.end())
        return;
    const ProxyJobSpec spec = *it;
    m_specs.erase(it);
    const RenderQueue::Job* job = m_queue.job(id);
    QString error
        = job && !ok && job->status == RenderQueue::Status::Failed ? job->result.error : QString();
    if (ok) {
        // Replace the previous file atomically: rename() within one folder.
        QFile::remove(spec.outputPath);
        if (QFile::rename(spec.partialPath, spec.outputPath)) {
            MediaLinks::writeSidecar(spec.outputPath, spec.originalPath, spec.link);
            m_links->setLink(spec.mediaId, spec.kind, spec.link);
        } else {
            ok = false;
            error = tr("Cannot move %1 into place.").arg(spec.outputPath);
        }
    }
    QFile::remove(spec.partialPath); // a failed / cancelled job's leftovers, if any
    m_queue.removeJob(id);
    emit finished(spec.mediaId, spec.kind, ok, error);
    emit changed(spec.mediaId);
}

void ProxyQueue::deleteLinked(
    MediaLinks* links, const QString& mediaId, LinkKind kind, const QString& managedFolder)
{
    const MediaLink link = links->link(mediaId, kind);
    if (link.isEmpty())
        return;
    const QString folder = QDir(managedFolder).absolutePath();
    if (!folder.isEmpty() && QFileInfo(link.path).absolutePath() == folder) {
        QFile::remove(link.path);
        QFile::remove(link.path + QStringLiteral(".json"));
    }
    links->clearLink(mediaId, kind);
}

} // namespace kk
