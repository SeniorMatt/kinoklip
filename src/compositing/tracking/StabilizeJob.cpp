// SPDX-License-Identifier: GPL-3.0-or-later
// StabilizeJob implementation. See the header.
#include "compositing/tracking/StabilizeJob.h"

#include "core/MediaItem.h"
#include "core/TimelineData.h"
#include "media/VideoDecoder.h"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace kk::tracking {

std::optional<StabilizeJob> planStabilizeJob(const ClipData& clip, const MediaItem& item,
    Rational timelineFps, stabilize::Mode mode, QString* why)
{
    auto refuse = [&](const QString& reason) -> std::optional<StabilizeJob> {
        if (why)
            *why = reason;
        return std::nullopt;
    };
    if (clip.kind != ClipKind::Media || item.isTimeline())
        return refuse(QStringLiteral("only clips of video files can be stabilized"));
    if (!item.info.hasVideo || item.info.isStillImage)
        return refuse(QStringLiteral("a still picture has no camera movement to measure"));
    const Rational fps = item.info.frameRate;
    if (!fps.isValid() || !timelineFps.isValid())
        return refuse(QStringLiteral("the frame rate of the media is unknown"));

    // The source frames the clip shows, whatever its speed, direction or retime curve.
    int64_t first = INT64_MAX, last = INT64_MIN;
    for (int64_t f = 0; f < clip.duration; ++f) {
        const int64_t k
            = stabilize::sourceFrameAt(clip.sourceTimeUsAt(clip.start + f, timelineFps), fps);
        first = std::min(first, k);
        last = std::max(last, k);
    }
    first = std::max<int64_t>(0, first);
    if (item.info.durationUs > 0) {
        const int64_t count = stabilize::sourceFrameAt(item.info.durationUs, fps);
        last = std::min(last, std::max<int64_t>(first, count - 1));
    }
    if (last <= first)
        return refuse(QStringLiteral("the clip shows a single source frame (a freeze frame)"));

    StabilizeJob job;
    job.path = item.path;
    job.request.mode = mode;
    job.request.fps = fps;
    job.request.firstFrame = first;
    job.request.lastFrame = last;
    return job;
}

StabilizeOutcome runStabilizeJob(
    const StabilizeJob& job, const StabilizeProgressFn& progress, const std::atomic<bool>* cancel)
{
    media::VideoDecoder decoder;
    decoder.setThreadCount(1);
    QString error;
    if (!decoder.open(job.path, &error)) {
        StabilizeOutcome failed;
        failed.error = QStringLiteral("cannot open the media: %1").arg(error);
        return failed;
    }
    const Rational fps = job.request.fps;
    const int width = std::max(16,
        std::min(job.maxWidth,
            decoder.info().displayWidth > 0 ? decoder.info().displayWidth : job.maxWidth));
    auto frameAt = [&](int64_t frame) {
        // The middle of the frame's interval: robust against the rounding of frame times.
        const int64_t us = int64_t(std::llround((double(frame) + 0.5) * 1e6 * fps.den / fps.num));
        return grayFromImage(decoder.frameAt(us, QSize(width, 0)));
    };
    return stabilizeAnalyse(job.request, frameAt, progress, cancel);
}

void applyStabilization(ClipData& clip, const stabilize::Settings& settings,
    std::shared_ptr<const stabilize::Data> data)
{
    clip.stabilization.settings = stabilize::clamped(settings);
    clip.stabilization.settings.enabled = true;
    clip.stabilization.data = std::move(data);
}

StabilizeRunner::StabilizeRunner(QObject* parent)
    : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<StabilizeOutcome>::finished, this, [this] {
        m_outcome = m_watcher.result();
        m_running = false;
        emit finished(m_clipId);
    });
}

StabilizeRunner::~StabilizeRunner()
{
    m_cancel = true;
    m_watcher.waitForFinished();
}

bool StabilizeRunner::start(quint64 clipId, StabilizeJob job)
{
    if (m_running)
        return false;
    m_running = true;
    m_cancel = false;
    m_clipId = clipId;
    m_done = 0;
    m_total = int(job.request.lastFrame - job.request.firstFrame + 1);
    m_outcome = {};
    auto report = [this](int done, int total) {
        QMetaObject::invokeMethod(
            this,
            [this, done, total] {
                m_done = done;
                m_total = total;
                emit progress(done, total);
            },
            Qt::QueuedConnection);
    };
    m_watcher.setFuture(QtConcurrent::run(
        [job = std::move(job), report, this] { return runStabilizeJob(job, report, &m_cancel); }));
    return true;
}

void StabilizeRunner::cancel()
{
    m_cancel = true;
}

} // namespace kk::tracking
