// SPDX-License-Identifier: GPL-3.0-or-later
// FrameScheduler — look-ahead requests, frame dropping and scrub coalescing.
#include "playback/FrameScheduler.h"

#include <algorithm>
#include <chrono>

namespace kk {

namespace {
/// Weight of the newest sample in the render-time moving average (~ last 10 frames).
constexpr double kRenderMsSmoothing = 0.1;
/// Samples above this multiple of the average are clamped to it.
constexpr double kOutlierFactor = 3.0;
} // namespace

FrameScheduler::FrameScheduler(RenderFn render)
    : m_render(std::move(render))
    , m_cancel(std::make_shared<std::atomic<bool>>(false))
{ }

void FrameScheduler::cancelOutstanding()
{
    // Requests still queued on the render thread are skipped; a new flag for new requests.
    m_cancel->store(true, std::memory_order_relaxed);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
}

void FrameScheduler::setParams(const RenderParams& params, bool discardPending)
{
    m_params = params;
    if (!discardPending) {
        return;
    }
    // Playback frames in flight were made for the old parameters: discard them. (A still in
    // flight is kept: pollStill() re-issues the pending wish with the new parameters.)
    bool cancel = false;
    for (Entry& e : m_entries) {
        if (!check(e)) {
            m_zombies.push_back(std::move(e.future));
            cancel = true;
        }
    }
    m_entries.clear();
    if (cancel && !m_stillActive) {
        cancelOutstanding();
    }
}

std::future<RenderedFramePtr> FrameScheduler::issue(int64_t frame)
{
    RenderRequest request;
    request.timeline = m_params.timeline;
    request.media = m_params.media;
    request.frame = frame;
    request.outSize = m_params.outSize;
    request.quality = m_params.quality;
    request.outputs = m_params.outputs;
    request.cancelled = m_cancel;
    ++m_issued;
    return m_render(std::move(request));
}

bool FrameScheduler::check(Entry& e)
{
    if (e.ready) {
        return true;
    }
    if (!e.future.valid()
        || e.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return false;
    }
    e.result = e.future.get();
    e.ready = true;
    return true;
}

void FrameScheduler::reapZombies()
{
    m_zombies.erase(std::remove_if(m_zombies.begin(), m_zombies.end(),
                        [](std::future<RenderedFramePtr>& f) {
                            return !f.valid()
                                || f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                        }),
        m_zombies.end());
}

int FrameScheduler::inFlight()
{
    reapZombies();
    int count = int(m_zombies.size());
    for (Entry& e : m_entries) {
        count += check(e) ? 0 : 1;
    }
    if (m_stillActive && !check(m_still)) {
        ++count;
    }
    return count;
}

bool FrameScheduler::busy()
{
    return inFlight() > 0 || m_hasPendingStill || (m_stillActive && m_still.ready);
}

int FrameScheduler::prefetch(const std::vector<FrameSlot>& wanted)
{
    int issued = 0;
    int unfinished = inFlight();
    for (const FrameSlot& slot : wanted) {
        if (unfinished >= m_maxInFlight) {
            break;
        }
        const int64_t key = slot.key;
        const bool known = std::any_of(
            m_entries.begin(), m_entries.end(), [key](const Entry& e) { return e.key == key; });
        if (known || slot.frame < 0) {
            continue;
        }
        Entry e;
        e.key = slot.key;
        e.frame = slot.frame;
        e.future = issue(slot.frame);
        m_entries.push_back(std::move(e));
        ++unfinished;
        ++issued;
    }
    return issued;
}

RenderedFramePtr FrameScheduler::poll(int64_t due, int direction, int64_t* shownKey)
{
    const int dir = direction >= 0 ? 1 : -1;
    // Newest finished frame that is not in the future (in play direction).
    int best = -1;
    for (int i = 0; i < int(m_entries.size()); ++i) {
        Entry& e = m_entries[size_t(i)];
        if (!check(e) || (e.key - due) * dir > 0) {
            continue;
        }
        if (best < 0 || (e.key - m_entries[size_t(best)].key) * dir > 0) {
            best = i;
        }
    }
    if (best < 0) {
        return {};
    }
    RenderedFramePtr result = m_entries[size_t(best)].result;
    const int64_t shown = m_entries[size_t(best)].key;
    if (shownKey) {
        *shownKey = shown;
    }
    // Everything at or before the presented slot is obsolete.
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if ((it->key - shown) * dir > 0) {
            ++it;
            continue;
        }
        if (it->key != shown) {
            if (it->ready) {
                ++m_dropped;
            } else {
                m_zombies.push_back(std::move(it->future));
            }
        }
        it = m_entries.erase(it);
    }
    noteRendered(result);
    return result;
}

void FrameScheduler::requestStill(int64_t frame, bool force)
{
    if (m_stillActive) {
        // One in flight: remember only the newest wish.
        m_hasPendingStill = true;
        m_pendingStill = frame;
        return;
    }
    if (!force && frame == m_lastStillFrame) {
        return;
    }
    m_stillActive = true;
    m_still = Entry {};
    m_still.frame = frame;
    m_still.future = issue(frame);
    m_hasPendingStill = false;
}

RenderedFramePtr FrameScheduler::pollStill()
{
    if (!m_stillActive || !check(m_still)) {
        return {};
    }
    RenderedFramePtr result = std::move(m_still.result);
    m_lastStillFrame = m_still.frame;
    m_stillActive = false;
    m_still = Entry {};
    if (m_hasPendingStill) {
        // Re-issue even for the same frame: the pending wish may carry new parameters (an
        // edit while the previous render was in flight).
        m_hasPendingStill = false;
        requestStill(m_pendingStill, true);
    }
    noteRendered(result);
    return result;
}

void FrameScheduler::clear()
{
    for (Entry& e : m_entries) {
        if (!check(e)) {
            m_zombies.push_back(std::move(e.future));
        }
    }
    m_entries.clear();
    m_hasPendingStill = false;
    m_lastStillFrame = -1;
    if (m_stillActive && !check(m_still)) {
        m_zombies.push_back(std::move(m_still.future));
    }
    m_stillActive = false;
    m_still = Entry {};
    cancelOutstanding();
}

void FrameScheduler::noteRendered(const RenderedFramePtr& frame)
{
    if (!frame || !frame->ok) {
        return;
    }
    // Outliers (a decoder opening or seeking) are clamped so one slow frame does not look
    // like a renderer that cannot keep up.
    const double sample = m_avgRenderMs > 0.0
        ? std::min(frame->renderMs, kOutlierFactor * m_avgRenderMs)
        : frame->renderMs;
    m_avgRenderMs = m_avgRenderMs <= 0.0
        ? sample
        : m_avgRenderMs + kRenderMsSmoothing * (sample - m_avgRenderMs);
}

} // namespace kk
