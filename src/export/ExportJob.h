// SPDX-License-Identifier: GPL-3.0-or-later
// ExportJob — renders a timeline snapshot to a file: frames from the Compositor (2-3 requests
// in flight, PBO readback), audio from an AudioMixer, encoding with libavcodec and muxing with
// libavformat. Blocking run() for a worker thread; progress callback; cancel from any thread.
#pragma once

#include "export/ExportSettings.h"
#include "render/RenderTypes.h"

#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace kk {

class Compositor;
struct TimelineData;

/// Progress of a running job.
struct ExportProgress
{
    int64_t framesDone = 0; ///< frames encoded so far
    int64_t totalFrames = 0; ///< frames in the render range
    double fps = 0.0; ///< current speed (frames per second, sliding window of ~1 s)
    double elapsedSeconds = 0.0;
    double etaSeconds = -1.0; ///< estimated remaining time; < 0 while unknown

    /// 0..1 completion.
    [[nodiscard]] double fraction() const
    {
        return totalFrames > 0 ? double(framesDone) / double(totalFrames) : 0.0;
    }
};

/// Outcome of ExportJob::run().
struct ExportResult
{
    bool ok = false;
    bool cancelled = false; ///< cancel() was called; partial output was removed
    QString error; ///< user-readable reason when !ok && !cancelled
    QString outputPath; ///< the file (or the sequence pattern) that was written
    QStringList files; ///< every file written (one for movies, N for sequences)
    QString videoEncoder; ///< libavcodec encoder used ("h264_vaapi", ...), empty if none
    EncoderBackend videoBackend = EncoderBackend::Software;
    int64_t frames = 0; ///< frames written
    double seconds = 0.0; ///< wall-clock time of the job
    /// Average speed in frames per second (frames / seconds).
    [[nodiscard]] double averageFps() const { return seconds > 0 ? double(frames) / seconds : 0.0; }
};

/// One render job (docs/archive/plan.md §5.6).
///
/// Pipeline: run() keeps up to kFramesInFlight Compositor::renderAsync() requests queued (the
/// render thread renders frame N+1 while frame N is read back and encoded here), converts
/// each RGBA frame to the encoder's pixel format (VideoEncoder), mixes the audio that belongs
/// to the frame (AudioMixer, 48 kHz) and hands both to the interleaving muxer (MediaWriter).
/// Video pts = frame index from the range start (time base 1/fps, exactly the timeline
/// rate, no retiming); audio pts = sample index at the output rate, starting at the same
/// instant, so A/V sync is exact by construction.
///
/// Hardware encoders: EncoderCatalog::candidates() lists working encoders; if opening one at
/// the real size fails (e.g. resolution limits), the next one (finally software) is used.
///
/// Thread-safety: run() blocks; call it from a worker thread (or the main thread of a CLI
/// tool) — never from the Compositor's render thread. cancel() and isCancelled() may be
/// called from any thread. The Compositor must outlive run().
class ExportJob
{
public:
    /// Requests kept queued in the compositor (2-3 lets readback overlap rendering).
    static constexpr int kFramesInFlight = 3;

    using ProgressCallback = std::function<void(const ExportProgress&)>;

    /// @p timeline and @p media are immutable snapshots taken when the job was queued.
    ExportJob(ExportSettings settings, std::shared_ptr<const TimelineData> timeline,
        std::shared_ptr<const MediaPathMap> media);
    ~ExportJob();
    ExportJob(const ExportJob&) = delete;
    ExportJob& operator=(const ExportJob&) = delete;

    /// The compositor used for video (required when settings().hasVideo()).
    void setCompositor(Compositor* compositor) { m_compositor = compositor; }
    /// Renders with proxy / optimized media instead of the camera originals (Deliver's "Use
    /// proxy media" option; the default, nullptr, always renders the originals).
    void setProxies(std::shared_ptr<const ProxyMap> proxies) { m_proxies = std::move(proxies); }

    /// Renders the whole range. @p progress is called on the calling thread after every
    /// frame (keep it cheap; throttle UI updates yourself). On error or cancel every file
    /// written by this job is deleted.
    [[nodiscard]] ExportResult run(const ProgressCallback& progress = {});

    /// Requests cancellation; run() returns soon after with cancelled = true.
    void cancel() { m_cancel.store(true, std::memory_order_relaxed); }
    /// True once cancel() was called.
    [[nodiscard]] bool isCancelled() const { return m_cancel.load(std::memory_order_relaxed); }

    /// The job's settings.
    [[nodiscard]] const ExportSettings& settings() const { return m_settings; }
    /// The timeline snapshot.
    [[nodiscard]] const std::shared_ptr<const TimelineData>& timeline() const { return m_timeline; }

private:
    ExportResult runStill();
    ExportResult runStreams(const ProgressCallback& progress);

    ExportSettings m_settings;
    std::shared_ptr<const TimelineData> m_timeline;
    std::shared_ptr<const MediaPathMap> m_media;
    std::shared_ptr<const ProxyMap> m_proxies;
    Compositor* m_compositor = nullptr;
    std::atomic<bool> m_cancel {false};
};

} // namespace kk
