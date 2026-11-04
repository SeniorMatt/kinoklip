// SPDX-License-Identifier: GPL-3.0-or-later
// FrameScheduler — decides which frames to ask the compositor for and which finished frame to
// present (playback look-ahead, frame dropping, scrub coalescing). Pure logic around futures;
// the render call is injected so tests drive it with hand-made promises.
#pragma once

#include "render/RenderTypes.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <vector>

namespace kk {

/// Everything of a RenderRequest except the frame (what changes per request "generation").
struct RenderParams
{
    std::shared_ptr<const TimelineData> timeline;
    std::shared_ptr<const MediaPathMap> media;
    QSize outSize;
    Quality quality = Quality::Full;
    unsigned outputs = OutputTexture;
    std::shared_ptr<const ProxyMap> proxies; ///< proxy / optimized media (viewers only)
    std::shared_ptr<const RenderCacheView> renderCache; ///< cached chunks of `timeline`
    bool bypassGrades = false; ///< Color page "bypass all grades" (viewer only)
    bool dynamicZoom = true; ///< false: without Dynamic Zoom (on-screen controls, viewer only)
};

/// One frame the playback engine wants: `frame` is rendered, `key` orders the frames in play
/// time. They differ only when looping: the key keeps increasing (unwrapped position) while
/// the frame jumps back to the loop start, so "older/newer than due" stays well defined.
struct FrameSlot
{
    int64_t key = 0;
    int64_t frame = 0;
};

/// Request bookkeeping between the playback engine and the Compositor.
///
/// Playback: the engine calls prefetch() with the frames it will want next (in display order)
/// and poll() with the frame due *now*. poll() returns the newest finished frame that is not
/// in the future (in play direction) and forgets everything older — those frames are dropped,
/// never shown late, so video cannot drift behind the clock. At most maxInFlight() requests
/// are unfinished at any time (2-3 keep the GPU busy while bounding latency). Requests that
/// became useless (clear() on a seek, a content change) are cancelled: the render thread skips
/// them if it has not started them yet (RenderRequest::cancelled); until they complete they
/// still count as in flight.
///
/// Scrubbing / stills: requestStill() keeps at most ONE still request in flight; further calls
/// only replace the pending frame number. When the in-flight one finishes, poll() returns it
/// and the latest pending frame is requested — a fast scrub renders only the frames it can
/// keep up with, and always ends on the last requested one.
///
/// Thread-safety: none (GUI thread). Futures are only polled with wait_for(0), never waited on.
class FrameScheduler
{
public:
    using RenderFn = std::function<std::future<RenderedFramePtr>(RenderRequest)>;

    /// @p render queues a request (normally Compositor::renderAsync).
    explicit FrameScheduler(RenderFn render);

    /// Upper bound of unfinished requests (default 3).
    void setMaxInFlight(int count) { m_maxInFlight = std::max(1, count); }
    [[nodiscard]] int maxInFlight() const { return m_maxInFlight; }

    /// New request parameters. With @p discardPending (content changed: timeline or media)
    /// outstanding playback requests are discarded; otherwise (size / quality only) they are
    /// still presented — same picture, other resolution — so a quality switch drops nothing.
    void setParams(const RenderParams& params, bool discardPending = true);
    [[nodiscard]] const RenderParams& params() const { return m_params; }

    /// Playback: requests the slots of @p wanted (display order) whose key is not requested yet,
    /// as long as fewer than maxInFlight() are unfinished. Returns the number issued.
    int prefetch(const std::vector<FrameSlot>& wanted);
    /// Playback: the frame to present when the slot with key @p dueKey is due, playing in
    /// direction @p direction (+1 / -1), or nullptr if nothing new is ready. Slots older than
    /// the returned one are discarded and counted in droppedCount(). @p shownKey receives the
    /// key of the returned frame.
    RenderedFramePtr poll(int64_t dueKey, int direction, int64_t* shownKey = nullptr);

    /// Scrub/still: wants frame @p frame shown as soon as possible (coalesced, see above).
    /// @p force re-renders even if @p frame is the frame last shown (timeline edit).
    void requestStill(int64_t frame, bool force = false);
    /// Scrub/still: the finished still, or nullptr. Issues the pending one if any.
    RenderedFramePtr pollStill();

    /// Forgets all playback and still requests and cancels the unfinished ones (they keep
    /// counting until the render thread has skipped or finished them).
    void clear();
    /// True if any request (playback, still or discarded) is unfinished.
    [[nodiscard]] bool busy();
    /// Unfinished requests (including discarded ones).
    [[nodiscard]] int inFlight();
    /// Frames that finished but were skipped by poll() because a newer one was due.
    [[nodiscard]] int64_t droppedCount() const { return m_dropped; }
    /// Total requests issued (tests: coalescing).
    [[nodiscard]] int64_t issuedCount() const { return m_issued; }
    /// Moving average of RenderedFrame::renderMs over presented frames.
    [[nodiscard]] double averageRenderMs() const { return m_avgRenderMs; }
    /// Restarts the moving average (after a quality change).
    void resetAverage() { m_avgRenderMs = 0.0; }

private:
    struct Entry
    {
        int64_t key = 0;
        int64_t frame = 0;
        std::future<RenderedFramePtr> future;
        RenderedFramePtr result; ///< filled once the future is ready
        bool ready = false;
    };

    std::future<RenderedFramePtr> issue(int64_t frame);
    /// Moves a ready future's value into the entry; true if ready.
    static bool check(Entry& e);
    void reapZombies();
    void noteRendered(const RenderedFramePtr& frame);
    /// Sets the cancellation flag of every request issued so far.
    void cancelOutstanding();

    RenderFn m_render;
    RenderParams m_params;
    int m_maxInFlight = 3;
    std::deque<Entry> m_entries; ///< playback requests in issue order
    std::vector<std::future<RenderedFramePtr>> m_zombies; ///< discarded, still rendering
    bool m_stillActive = false;
    Entry m_still;
    bool m_hasPendingStill = false;
    int64_t m_pendingStill = 0;
    int64_t m_lastStillFrame = -1;
    int64_t m_dropped = 0;
    int64_t m_issued = 0;
    double m_avgRenderMs = 0.0;
    std::shared_ptr<std::atomic<bool>> m_cancel; ///< flag of the current request generation
};

} // namespace kk
