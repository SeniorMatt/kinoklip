// SPDX-License-Identifier: GPL-3.0-or-later
// StabilizeJob — the Stabilize button's work for one media clip: plans which source frames the
// clip shows, decodes them (VideoDecoder, the same decoder as the rest of the editor) and runs the
// Stabilizer on them, off the UI thread when run through StabilizeRunner. The command layer and the
// tests call the synchronous runStabilizeJob() directly.
#pragma once

#include "compositing/tracking/Stabilizer.h"

#include <QFutureWatcher>
#include <QObject>
#include <QString>

#include <atomic>
#include <optional>

namespace kk {

struct ClipData;
struct MediaItem;

namespace tracking {

/// Width the pictures are shrunk to for measuring (never enlarged).
inline constexpr int kStabilizeWidth = 640;

/// What to decode and measure for one clip.
struct StabilizeJob
{
    QString path; ///< the media file (or image-sequence path)
    StabilizeRequest request;
    int maxWidth = kStabilizeWidth;
};

/// The job for @p clip (a Media clip of @p item on a timeline of @p timelineFps): the source frames
/// the clip shows, whatever its trim, speed, reverse or retime curve. nullopt with a sentence in
/// @p why when the clip cannot be stabilized (not a video file, a still, a freeze frame, no
/// frame rate).
[[nodiscard]] std::optional<StabilizeJob> planStabilizeJob(const ClipData& clip,
    const MediaItem& item, Rational timelineFps, stabilize::Mode mode, QString* why);

/// Decodes and measures (blocking). @p cancel may be null.
[[nodiscard]] StabilizeOutcome runStabilizeJob(const StabilizeJob& job,
    const StabilizeProgressFn& progress = {}, const std::atomic<bool>* cancel = nullptr);

/// Switches stabilization on for @p clip with @p settings and the freshly measured @p data.
void applyStabilization(ClipData& clip, const stabilize::Settings& settings,
    std::shared_ptr<const stabilize::Data> data);

/// Runs one StabilizeJob at a time on a worker thread. Thread-safety: GUI thread only (signals
/// are delivered there).
class StabilizeRunner : public QObject
{
    Q_OBJECT
public:
    explicit StabilizeRunner(QObject* parent = nullptr);
    ~StabilizeRunner() override;

    /// Starts @p job for clip @p clipId. False when a run is in progress.
    bool start(quint64 clipId, StabilizeJob job);
    /// Asks the run to end; finished() follows with cancelled set.
    void cancel();
    [[nodiscard]] bool isRunning() const { return m_running; }
    [[nodiscard]] quint64 clipId() const { return m_clipId; }
    [[nodiscard]] int done() const { return m_done; }
    [[nodiscard]] int total() const { return m_total; }
    /// The latest run's result (valid from finished() on).
    [[nodiscard]] const StabilizeOutcome& outcome() const { return m_outcome; }

signals:
    void progress(int done, int total);
    /// The run ended: look at outcome().
    void finished(quint64 clipId);

private:
    QFutureWatcher<StabilizeOutcome> m_watcher;
    StabilizeOutcome m_outcome;
    std::atomic<bool> m_cancel {false};
    quint64 m_clipId = 0;
    int m_done = 0;
    int m_total = 0;
    bool m_running = false;
};

} // namespace tracking
} // namespace kk
