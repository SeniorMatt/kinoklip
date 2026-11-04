// SPDX-License-Identifier: GPL-3.0-or-later
// RenderFinishNotifier implementation.
#include "ui/deliver/RenderFinishNotifier.h"

#include "core/Preferences.h"
#include "platform/ChildProcessEnv.h"
#include "platform/DesktopNotifications.h"
#include "platform/FileManager.h"

#include <QApplication>
#include <QUrl>

#include <cmath>

namespace kk {

RenderFinishNotifier::RenderFinishNotifier(RenderQueue* queue, QWidget* window, QObject* parent)
    : QObject(parent)
    , m_queue(queue)
    , m_window(window)
    , m_reveal([](const QString& path) { platform::showInFileManager(path); })
    , m_open([](const QString& path) { platform::openUrl(QUrl::fromLocalFile(path)); })
{
    connect(
        queue, &RenderQueue::jobFinished, this, [this](quint64 id, bool) { onJobFinished(id); });
    connect(queue, &RenderQueue::runningChanged, this, &RenderFinishNotifier::onRunningChanged);
}

RenderFinishNotifier::~RenderFinishNotifier() = default;

void RenderFinishNotifier::setNotifier(platform::Notifier* notifier)
{
    m_notifier = notifier;
    m_notifierSet = true;
}

void RenderFinishNotifier::setHandlers(std::function<void(const QString&)> revealInFileManager,
    std::function<void(const QString&)> openFile)
{
    m_reveal = std::move(revealInFileManager);
    m_open = std::move(openFile);
}

platform::Notifier* RenderFinishNotifier::notifier()
{
    if (!m_notifierSet) {
        m_notifierSet = true;
        m_own = platform::createDesktopNotifier();
        m_notifier = m_own.get();
    }
    return m_notifier;
}

QString RenderFinishNotifier::formatDuration(double seconds)
{
    const int s = std::max(0, int(std::lround(seconds)));
    const QLatin1Char zero('0');
    if (s >= 3600)
        return QStringLiteral("%1:%2:%3")
            .arg(s / 3600)
            .arg((s / 60) % 60, 2, 10, zero)
            .arg(s % 60, 2, 10, zero);
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, zero);
}

std::optional<RenderFinishNotifier::Summary> RenderFinishNotifier::summarize(
    const QList<RenderQueue::Job>& jobs)
{
    if (jobs.isEmpty() || jobs.last().status == RenderQueue::Status::Cancelled)
        return std::nullopt; // the user stopped the run: they know
    int done = 0;
    int failed = 0;
    int cancelled = 0;
    double seconds = 0.0;
    QString firstError;
    const RenderQueue::Job* lastDone = nullptr;
    for (const RenderQueue::Job& job : jobs) {
        seconds += job.result.seconds;
        switch (job.status) {
        case RenderQueue::Status::Done:
            ++done;
            lastDone = &job;
            break;
        case RenderQueue::Status::Cancelled:
            ++cancelled;
            break;
        default:
            ++failed;
            if (firstError.isEmpty())
                firstError = job.result.error;
            break;
        }
    }
    Summary s;
    const int total = int(jobs.size());
    if (failed == 0 && cancelled == 0) {
        s.summary = tr("Render complete");
        const QString time = formatDuration(seconds);
        s.body = total == 1 ? tr("%1 rendered in %2").arg(jobs.first().name, time)
                            : tr("%1 jobs rendered in %2").arg(total).arg(time);
    } else {
        s.summary = tr("Render finished with errors");
        QStringList parts;
        if (failed > 0)
            parts << tr("%1 of %2 failed").arg(failed).arg(total);
        if (cancelled > 0)
            parts << (failed > 0 ? tr("%1 cancelled").arg(cancelled)
                                 : tr("%1 of %2 cancelled").arg(cancelled).arg(total));
        s.body = parts.join(QStringLiteral(", "));
        if (failed == 1 && !firstError.isEmpty())
            s.body += QLatin1Char('\n') + firstError;
    }
    if (lastDone) {
        const ExportResult& r = lastDone->result;
        s.revealPath = r.files.isEmpty() ? r.outputPath : r.files.last();
        if (done == 1 && r.files.size() <= 1 && !s.revealPath.isEmpty())
            s.playPath = s.revealPath; // one movie / audio file, not an image sequence
    }
    return s;
}

void RenderFinishNotifier::onJobFinished(quint64 id)
{
    if (const RenderQueue::Job* job = m_queue->job(id)) {
        RenderQueue::Job copy = *job;
        copy.timeline.reset(); // only the name, status and result are needed
        copy.media.reset();
        copy.proxies.reset();
        m_run.append(std::move(copy));
    }
}

void RenderFinishNotifier::onRunningChanged(bool running)
{
    if (!running && !m_run.isEmpty())
        runFinished();
}

void RenderFinishNotifier::runFinished()
{
    const QList<RenderQueue::Job> jobs = std::exchange(m_run, {});
    const std::optional<Summary> s = summarize(jobs);
    if (!s || !prefs::renderNotificationsEnabled())
        return;
    if (m_window && !m_window->window()->isActiveWindow())
        QApplication::alert(m_window->window());
    platform::Notifier* n = notifier();
    if (!n)
        return;
    platform::DesktopNotification note {s->summary, s->body, {}};
    if (!s->revealPath.isEmpty())
        note.actions.append({QLatin1String(kOpenFolderAction), tr("Open Folder")});
    if (!s->playPath.isEmpty())
        note.actions.append({QLatin1String(kPlayAction), tr("Play")});
    n->show(note,
        [reveal = m_reveal, open = m_open, revealPath = s->revealPath, playPath = s->playPath](
            const QString& key) {
            if (key == QLatin1String(kOpenFolderAction) && reveal)
                reveal(revealPath);
            else if (key == QLatin1String(kPlayAction) && open)
                open(playPath);
        });
}

} // namespace kk
