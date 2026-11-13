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
#include <optional>
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
    /// Compositing page: every frame is one node of one clip's composition (RenderRequest::
    /// compositionNode) instead of the timeline picture; `frame - compStart` (clamped to
    /// [0, compLength)) is the comp frame. Playback, look-ahead and frame dropping are the same.
    std::optional<CompositionNodeView> compNode = std::nullopt;
    int64_t compStart = 0;
    int64_t compLength = 0;
};

/// One frame the playback engine wants: `frame` is rendered, `key` orders the frames in play
/// time. They differ only when looping: the key keeps increasing (unwrapped position) while
/// the frame jumps back to the loop start, so "older/newer than due" stays well defined.
struct FrameSlot
{
    int64_t key = 0;
    int64_t frame = 0;
    bool catchUp = false; ///< RenderRequest::catchUp
};

/// What the playback look-ahead needs to choose the frames to ask for.
struct LookAhead
{
    double unwrapped = 0.0; ///< clock position in timeline frames (unwrapped when looping)
    int64_t dueKey = 0; ///< the frame due now
    double rate = 1.0; ///< play rate (negative: reverse)
    int lead = 0; ///< frames to start ahead of the clock (rendering slower than the interval)
    int count = 5; ///< number of intervals to cover
    bool sequential = true; ///< allow the sequential mode below
    bool haveNewest = false; ///< a frame was asked for or shown
    int64_t newest = 0; ///< the newest key asked for or shown (in play direction)
    int64_t maxLateFrames = 6; ///< how far behind the clock the sequential mode goes
};

/// The keys of the frames to ask for, in display order (the engine still skips what is on
/// screen, outside the play range or already requested).
///
/// Normally the frames due at the next display intervals. At 1x forward: decoding cannot skip,
/// a request for a frame decodes every frame since the last one. If the look-ahead always starts
/// at the clock, a decoder nearly as slow as the clock makes every gap longer than the one
/// before (the gap is the time the last request took) and the shown rate collapses far below
/// the decoder's throughput (4K60 HEVC that decodes at 77 fps showed 13 fps). So the next
/// request is the frame after the newest one asked for as long as that is not more than
/// 2 x maxLateFrames behind the clock: it is shown late, which costs a little lip sync but keeps
/// the shown rate at the throughput, like mpv and VLC, which show every decoded frame and drop
/// only when far behind. Beyond that, or with nothing asked for yet, it starts at the clock
/// (plus lead).
///
/// @p catchUp is set while the frames are more than half of maxLateFrames behind the clock: those
/// requests discard the frames nothing refers to (RenderRequest::catchUp), which costs about half
/// the decoding, until the lag is gone, like mpv's framedrop. Beyond twice maxLateFrames the start
/// jumps to the clock.
[[nodiscard]] std::vector<int64_t> lookAheadKeys(const LookAhead& in, bool* catchUp = nullptr);

/// How many frames behind the clock the picture is presented (0 .. maxFrames). A decoder that
/// delivers in bursts finishes several frames at once after a stall; presented at the clock only
/// the newest would be shown. Raised by one (at most every 100 ms) when a frame was owed and
/// missing, lowered by one after a second without a miss.
class DisplayLag
{
public:
    [[nodiscard]] int frames() const { return m_frames; }
    void reset() { m_frames = 0; }
    /// @p missed: a frame was owed at the display time and had not finished.
    void update(bool missed, int64_t nowUs, int maxFrames)
    {
        if (missed) {
            if (m_frames < maxFrames && nowUs - m_stepUs >= 100'000) {
                ++m_frames;
                m_stepUs = nowUs;
            }
            m_missUs = nowUs;
        } else if (m_frames > 0 && nowUs - std::max(m_stepUs, m_missUs) >= 1'000'000) {
            --m_frames;
            m_stepUs = nowUs;
        }
    }

private:
    int m_frames = 0;
    int64_t m_stepUs = 0, m_missUs = 0;
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

    /// Playback: asks the renderer to prepare the decoders of the clips coming into view up to
    /// slot @p lastKey (the look-ahead past the render requests, so that the first frame after a
    /// cut is ready when due; RenderRequest::prepare). Every slot goes out once: a call only
    /// covers the slots not asked for since the last clear() or change of content. @p dueKey is
    /// the slot due now, @p frameMs the wall time per slot; @p frameOfKey maps a slot to its
    /// frame (loops). @return the requests issued (not counted in issuedCount() or inFlight();
    /// they complete at once and nothing waits for them).
    ///
    /// @p extraDueMs is added to every due time: a look-ahead made while paused, for a play
    /// that may start any time later (the prepared decoders stay reserved that much longer).
    int prepareAhead(int64_t dueKey, int64_t lastKey, double frameMs,
        const std::function<int64_t(int64_t)>& frameOfKey, double extraDueMs = 0.0);

    /// Scrub/still: wants frame @p frame shown as soon as possible (coalesced, see above).
    /// @p force re-renders even if @p frame is the frame last shown (timeline edit).
    void requestStill(int64_t frame, bool force = false);
    /// Scrub/still: the finished still, or nullptr. Issues the pending one if any.
    RenderedFramePtr pollStill();

    /// Forgets all playback and still requests and cancels the unfinished ones (they keep
    /// counting until the render thread has skipped or finished them).
    /// @p keepPrepare: the look-ahead in progress stays (a play that starts where the paused
    /// look-ahead was made, see PlaybackEngine::prepareParked()).
    void clear(bool keepPrepare = false);
    /// Stops the look-ahead in progress; the next prepareAhead() starts over.
    void cancelPrepare();
    /// True while a look-ahead is in progress (prepareAhead() ran since the last cancelPrepare()).
    [[nodiscard]] bool prepareActive() const { return m_hasPrepareEdge; }
    /// Playback: the newest key (in direction @p direction) among the requests not yet presented
    /// or dropped. @return false if there is none.
    [[nodiscard]] bool newestRequestedKey(int direction, int64_t* key) const;
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

    std::future<RenderedFramePtr> issue(int64_t frame, bool catchUp);
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
    std::shared_ptr<std::atomic<bool>> m_prepareCancel; ///< flag of the current look-ahead
    bool m_hasPrepareEdge = false;
    int64_t m_prepareEdge = 0; ///< last slot the look-ahead has covered
};

} // namespace kk
