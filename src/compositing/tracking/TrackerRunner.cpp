// SPDX-License-Identifier: GPL-3.0-or-later
// TrackerRunner implementation. See the header.
#include "compositing/tracking/TrackerRunner.h"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace kk::tracking {

namespace {

constexpr int kPrefetch = 2; ///< pictures requested ahead of the one being matched

} // namespace

TrackerRunner::TrackerRunner(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<kk::tracking::TrackSample>();
    qRegisterMetaType<kk::tracking::TrackOutcome>();
}

TrackerRunner::~TrackerRunner()
{
    // A step still running holds its own references; it just must not report here any more.
    ++m_generation;
    if (m_watcher)
        m_watcher->disconnect();
}

bool TrackerRunner::start(TrackJob job)
{
    if (m_running)
        stop();
    if (!job.source)
        return false;
    m_samples.clear();
    m_pending.clear();
    m_total = std::llabs(job.lastFrame - job.firstFrame) + 1;
    m_nextToRequest = job.firstFrame;
    m_run = std::make_shared<Run>();
    m_run->job = std::move(job);
    ++m_generation;
    m_running = true;
    emit progress(0, int(m_total));
    for (int i = 0; i < kPrefetch; ++i)
        requestFrame();
    launchStep();
    return true;
}

void TrackerRunner::stop()
{
    if (m_running)
        end(TrackOutcome::Stopped, {});
}

void TrackerRunner::requestFrame()
{
    const TrackJob& job = m_run->job;
    const bool forward = job.lastFrame >= job.firstFrame;
    if (forward ? m_nextToRequest > job.lastFrame : m_nextToRequest < job.lastFrame)
        return;
    const int64_t frame = m_nextToRequest;
    m_nextToRequest += forward ? 1 : -1;
    m_pending.emplace_back(
        frame, std::make_shared<std::future<RenderedFramePtr>>(job.source(frame)));
}

void TrackerRunner::launchStep()
{
    if (m_pending.empty()) {
        end(TrackOutcome::Finished, {});
        return;
    }
    auto [frame, picture] = std::move(m_pending.front());
    m_pending.pop_front();
    requestFrame(); // keep the render queue filled while this one is matched
    const bool first = m_samples.empty();
    const std::shared_ptr<Run> run = m_run;
    if (m_watcher)
        m_watcher->deleteLater();
    m_watcher = new QFutureWatcher<Step>(this);
    const quint64 generation = m_generation;
    connect(m_watcher, &QFutureWatcher<Step>::finished, this,
        [this, generation] { onStepDone(generation); });
    m_watcher->setFuture(QtConcurrent::run([run, picture, frame, first]() -> Step {
        Step step;
        step.sample.frame = frame;
        const RenderedFramePtr rendered = picture->get();
        if (!rendered || !rendered->ok || rendered->image.isNull()) {
            step.outcome = TrackOutcome::Failed;
            step.reason = QObject::tr("The picture could not be rendered.");
            return step;
        }
        const GrayImage gray = grayFromImage(rendered->image);
        const double w = gray.width, h = gray.height;
        if (first) {
            TrackerSettings settings;
            settings.patternSize = std::max(8, int(std::lround(run->job.patternFraction * w)));
            settings.searchSize
                = std::max(settings.patternSize, int(std::lround(run->job.searchFraction * w)));
            settings.minCorrelation = run->job.minCorrelation;
            run->tracker = PointTracker(settings);
            const Vec2 start {run->job.startU * w - 0.5, (1.0 - run->job.startV) * h - 0.5};
            if (!run->tracker.reset(gray, start)) {
                step.outcome = TrackOutcome::Lost;
                step.reason = QObject::tr("The pattern has no detail to follow. Move the box "
                                          "onto a corner or an edge.");
                return step;
            }
            step.sample.u = run->job.startU;
            step.sample.v = run->job.startV;
            step.sample.correlation = 1.0;
            return step;
        }
        const TrackResult r = run->tracker.track(gray);
        step.sample.correlation = r.correlation;
        if (!r.ok) {
            step.outcome = TrackOutcome::Lost;
            step.reason = QObject::tr("The pattern was lost.");
            return step;
        }
        step.sample.u = (r.position.x + 0.5) / w;
        step.sample.v = 1.0 - (r.position.y + 0.5) / h;
        return step;
    }));
}

void TrackerRunner::onStepDone(quint64 generation)
{
    if (generation != m_generation || !m_running || !m_watcher)
        return; // stopped or restarted meanwhile
    const Step step = m_watcher->result();
    if (step.outcome != TrackOutcome::Finished) {
        end(step.outcome, step.reason);
        return;
    }
    m_samples.push_back(step.sample);
    emit sampleReady(step.sample);
    emit progress(int(m_samples.size()), int(m_total));
    if (step.sample.frame == m_run->job.lastFrame)
        end(TrackOutcome::Finished, {});
    else
        launchStep();
}

void TrackerRunner::end(TrackOutcome outcome, const QString& reason)
{
    m_running = false;
    ++m_generation; // late results of the step in flight are dropped
    m_pending.clear();
    if (m_watcher) {
        m_watcher->disconnect(this);
        m_watcher->deleteLater();
        m_watcher = nullptr;
    }
    emit finished(outcome, reason);
}

} // namespace kk::tracking
