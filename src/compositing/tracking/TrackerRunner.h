// SPDX-License-Identifier: GPL-3.0-or-later
// TrackerRunner — follows one point through a range of comp frames, off the UI thread, and
// reports every position as it is found. The pictures come from a FrameSource the caller
// supplies (the Compositing page gives it the playback engine's renderCompositionNode(), so
// there is no second decode path); this class knows nothing about nodes or the project.
#pragma once

#include "compositing/tracking/PointTracker.h"
#include "render/RenderTypes.h"

#include <QFutureWatcher>
#include <QObject>

#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <vector>

namespace kk::tracking {

/// One tracked position. Normalised frame coordinates like the node parameters: u from the
/// left, v from the BOTTOM (0.5, 0.5 = the middle of the picture).
struct TrackSample
{
    int64_t frame = 0; ///< comp frame
    double u = 0.5;
    double v = 0.5;
    double correlation = 1.0; ///< quality of the match, -1..1 (1 at the start frame)
};

/// What to track.
struct TrackJob
{
    /// Frames from firstFrame to lastFrame inclusive, in that order (lastFrame < firstFrame
    /// tracks backwards). The point is at (startU, startV) on firstFrame.
    int64_t firstFrame = 0;
    int64_t lastFrame = 0;
    double startU = 0.5;
    double startV = 0.5;
    /// Box sides as a fraction of the picture width (resolution independent).
    double patternFraction = 0.05;
    double searchFraction = 0.15;
    double minCorrelation = 0.6;
    /// The picture of a comp frame. Called on the thread that calls start(); the returned
    /// future may be waited on from any thread. A frame that is not ok ends the run (Failed).
    std::function<std::future<RenderedFramePtr>(int64_t frame)> source;
};

enum class TrackOutcome {
    Finished, ///< every frame of the range was tracked
    Stopped, ///< stop() was called
    Lost, ///< the pattern could not be found in a frame (the frames before it were tracked)
    Failed ///< a picture could not be produced
};

/// Runs one TrackJob at a time. Each frame is one background step (wait for the picture,
/// grey conversion, matching); the next picture is already being rendered meanwhile.
/// Thread-safety: GUI thread only (signals are delivered there).
class TrackerRunner : public QObject
{
    Q_OBJECT
public:
    explicit TrackerRunner(QObject* parent = nullptr);
    ~TrackerRunner() override;

    /// Starts @p job (a run in progress is stopped first). False if the job is empty.
    bool start(TrackJob job);
    /// Ends the run after the frame in progress; the samples so far stay available.
    void stop();
    [[nodiscard]] bool isRunning() const { return m_running; }
    /// The positions found by the latest run, in the order they were found.
    [[nodiscard]] const std::vector<TrackSample>& samples() const { return m_samples; }
    /// Number of frames of the latest job.
    [[nodiscard]] int64_t totalFrames() const { return m_total; }

signals:
    /// Frame @p sample.frame was tracked.
    void sampleReady(const kk::tracking::TrackSample& sample);
    /// @p done of @p total frames are tracked.
    void progress(int done, int total);
    /// The run ended. @p reason is a short sentence for the user (empty for Finished/Stopped).
    void finished(kk::tracking::TrackOutcome outcome, const QString& reason);

private:
    /// What one background step returns.
    struct Step
    {
        TrackOutcome outcome = TrackOutcome::Finished;
        QString reason;
        TrackSample sample;
    };
    /// State the background steps share; steps of one run never overlap in time.
    struct Run
    {
        TrackJob job;
        PointTracker tracker;
    };

    void requestFrame();
    void launchStep();
    void onStepDone(quint64 generation);
    void end(TrackOutcome outcome, const QString& reason);

    std::shared_ptr<Run> m_run;
    std::deque<std::pair<int64_t, std::shared_ptr<std::future<RenderedFramePtr>>>> m_pending;
    QFutureWatcher<Step>* m_watcher = nullptr;
    std::vector<TrackSample> m_samples;
    int64_t m_nextToRequest = 0;
    int64_t m_total = 0;
    quint64 m_generation = 0;
    bool m_running = false;
};

} // namespace kk::tracking

Q_DECLARE_METATYPE(kk::tracking::TrackSample)
