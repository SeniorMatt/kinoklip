// SPDX-License-Identifier: GPL-3.0-or-later
// DecoderPool — video decoder reuse by path and position.
#include "render/DecoderPool.h"

#include "render/RenderLog.h"

#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <thread>

namespace kk::render {

namespace {

/// Queue priority of a preparation: work for the frame being rendered goes first.
constexpr int kPreparePriority = -1;
/// Due times this close belong to one frame (clips cutting together on two tracks).
constexpr qint64 kSameFrameMs = 5;

std::atomic<int> s_openDelayMs {0};
std::atomic<int> s_readAheadFrames {DecoderPool::kReadAheadFrames};

/// open() plus the test hook's delay.
bool openDecoder(media::VideoDecoder& decoder, const QString& path, QString* error,
    const std::shared_ptr<const std::atomic<bool>>& zeroCopyGate)
{
    if (zeroCopyGate) {
        decoder.setZeroCopyGate(zeroCopyGate);
        decoder.setZeroCopy(true); // before open(): the surface pool is sized for it
    }
    if (const int ms = s_openDelayMs.load(std::memory_order_relaxed); ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return decoder.open(path, error);
}

bool workerRunning(const DecoderPool::Entry& entry)
{
    return entry.ahead.valid()
        && entry.ahead.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

} // namespace

void DecoderPool::setReadAheadFrames(int frames)
{
    s_readAheadFrames.store(frames, std::memory_order_relaxed);
}

void DecoderPool::setOpenDelayMs(int ms)
{
    s_openDelayMs.store(ms, std::memory_order_relaxed);
}

DecoderPool::DecoderPool()
{
    m_clock.start();
}

DecoderPool::~DecoderPool()
{
    clear();
}

void DecoderPool::beginFrame()
{
    for (auto& e : m_entries)
        e->busy = false;
    if (m_prepareThreads) {
        const qint64 now = m_clock.elapsed();
        reapReservations(now);
        pumpWishes(now);
    }
}

DecoderPool::Entry* DecoderPool::acquire(const QString& path, int64_t timeUs)
{
    const qint64 now = m_clock.elapsed();
    Entry* forward = nullptr; // idle, target reachable by decoding forward (best: closest)
    int64_t forwardDistance = std::numeric_limits<int64_t>::max();
    Entry* anyIdle = nullptr; // idle, needs a seek (best: closest)
    int64_t idleDistance = std::numeric_limits<int64_t>::max();
    Entry* reservedIdle = nullptr; // held for another position: taken only as the last resort
    int perPath = 0;
    for (auto& e : m_entries) {
        if (e->path != path)
            continue;
        if (e->reservation && e->reservation->stale())
            continue; // on its way out (it may not even be open); beginFrame() ends it
        ++perPath;
        if (e->busy)
            continue;
        const int64_t delta = timeUs - e->lastTimeUs;
        if (e->lastTimeUs >= 0 && delta >= 0 && delta <= kForwardWindowUs) {
            if (delta < forwardDistance) {
                forward = e.get();
                forwardDistance = delta;
            }
        } else if (e->reservation) {
            reservedIdle = e.get();
        } else {
            const int64_t distance = delta < 0 ? -delta : delta;
            if (distance < idleDistance) {
                anyIdle = e.get();
                idleDistance = distance;
            }
        }
    }

    // A seek on an open decoder is cheaper than open() (which also decodes the first frame),
    // so a new decoder is only opened when every decoder of this path is busy this frame.
    Entry* chosen = forward ? forward : anyIdle;
    if (!chosen && perPath >= kMaxPerPath)
        chosen = reservedIdle;
    if (chosen && chosen->reservation) {
        // A prepared decoder. Waiting here (not later) keeps "open" true for the caller.
        const bool wanted = chosen == forward;
        if (wanted) {
            (workerRunning(*chosen) ? m_stats.hitsWaited : m_stats.hitsReady)
                .fetch_add(1, std::memory_order_relaxed);
            // Reservations due before this one: their frames were skipped, they will not come.
            const qint64 dueMs = chosen->reservation->dueMs;
            for (auto& e : m_entries) {
                if (e->reservation && e->reservation->dueMs + kSameFrameMs < dueMs
                    && !e->reservation->stop.exchange(true, std::memory_order_relaxed)) {
                    m_stats.expired.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else {
            chosen->reservation->stop.store(true, std::memory_order_relaxed);
        }
        if (endReservation(*chosen)) {
            // It moves forward from here on: decode-ahead may start with the next frame.
            chosen->lastTimeUs = timeUs - 1;
        } else {
            // Never opened. For the frame it was prepared for the file is the problem; a
            // reservation given up before its open just makes room.
            const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                [chosen](const std::unique_ptr<Entry>& e) { return e.get() == chosen; });
            closeEntry(size_t(it - m_entries.begin()));
            chosen = nullptr;
            --perPath;
            if (wanted) {
                qCWarning(lcRender) << "cannot open" << path;
                m_failedAtMs.insert(path, now);
                return nullptr;
            }
        }
    }
    if (!chosen) {
        if (perPath >= kMaxPerPath) {
            qCWarning(lcRender) << "more than" << kMaxPerPath << "simultaneous uses of" << path;
            return nullptr;
        }
        const auto failed = m_failedAtMs.constFind(path);
        if (failed != m_failedAtMs.constEnd() && now - *failed < kRetryFailedMs)
            return nullptr;
        auto entry = std::make_unique<Entry>();
        entry->decoder = std::make_unique<media::VideoDecoder>();
        QString error;
        if (!openDecoder(*entry->decoder, path, &error, m_zeroCopyGate)) {
            if (failed == m_failedAtMs.constEnd())
                qCWarning(lcRender) << "cannot open" << path << ":" << error;
            m_failedAtMs.insert(path, now);
            return nullptr;
        }
        m_failedAtMs.remove(path);
        entry->path = path;
        chosen = entry.get();
        m_entries.push_back(std::move(entry));
    }
    chosen->busy = true;
    chosen->previousTimeUs = chosen->lastTimeUs;
    chosen->lastTimeUs = timeUs;
    chosen->lastUsedMs = now;
    return chosen;
}

void DecoderPool::closeEntry(size_t index)
{
    Entry& e = *m_entries[index];
    if (e.reservation)
        e.reservation->stop.store(true, std::memory_order_relaxed);
    e.waitAhead();
    if (m_closeHook)
        m_closeHook(e);
    m_entries.erase(m_entries.begin() + qsizetype(index));
}

void DecoderPool::collectIdle()
{
    const qint64 now = m_clock.elapsed();
    for (size_t i = 0; i < m_entries.size();) {
        Entry& e = *m_entries[i];
        if (!e.busy && !e.reservation && now - e.lastUsedMs > m_idleCloseMs)
            closeEntry(i);
        else
            ++i;
    }
}

void DecoderPool::clear()
{
    stopPreparing();
    m_waiting.clear();
    while (!m_entries.empty())
        closeEntry(m_entries.size() - 1);
    m_failedAtMs.clear();
}

void DecoderPool::stopPreparing()
{
    for (auto& e : m_entries) {
        if (e->reservation)
            e->reservation->stop.store(true, std::memory_order_relaxed);
    }
}

// ---- Look-ahead ----

int DecoderPool::reservedCount() const
{
    return int(std::count_if(m_entries.begin(), m_entries.end(),
        [](const std::unique_ptr<Entry>& e) { return e->reservation != nullptr; }));
}

DecoderPool::PrepareStats DecoderPool::prepareStats() const
{
    PrepareStats s;
    s.lastCostMs = m_stats.lastCostMs.load(std::memory_order_relaxed);
    s.requested = m_stats.requested.load(std::memory_order_relaxed);
    s.started = m_stats.started.load(std::memory_order_relaxed);
    s.reused = m_stats.reused.load(std::memory_order_relaxed);
    s.ready = m_stats.ready.load(std::memory_order_relaxed);
    s.hitsReady = m_stats.hitsReady.load(std::memory_order_relaxed);
    s.hitsWaited = m_stats.hitsWaited.load(std::memory_order_relaxed);
    s.cancelled = m_stats.cancelled.load(std::memory_order_relaxed);
    s.expired = m_stats.expired.load(std::memory_order_relaxed);
    s.peakReserved = m_stats.peakReserved.load(std::memory_order_relaxed);
    s.peakRunning = m_stats.peakRunning.load(std::memory_order_relaxed);
    return s;
}

bool DecoderPool::endReservation(Entry& entry)
{
    entry.waitAhead();
    entry.reservation.reset();
    return entry.decoder->isOpen();
}

void DecoderPool::reapReservations(qint64 now)
{
    for (size_t i = 0; i < m_entries.size();) {
        Entry& e = *m_entries[i];
        if (e.reservation && !e.reservation->stale()
            && now > e.reservation->dueMs + kPrepareGraceMs) {
            e.reservation->stop.store(true, std::memory_order_relaxed);
            m_stats.expired.fetch_add(1, std::memory_order_relaxed);
        } else if (e.reservation && e.reservation->cancelled
            && e.reservation->cancelled->load(std::memory_order_relaxed)
            && !e.reservation->stop.exchange(true, std::memory_order_relaxed)) {
            m_stats.cancelled.fetch_add(1, std::memory_order_relaxed);
        }
        // Stopped: the worker gives up within one decoded frame (an open cannot be interrupted).
        // Never wait for it here: the next frame looks again.
        if (e.reservation && e.reservation->stale() && !workerRunning(e) && !endReservation(e)) {
            closeEntry(i);
            continue;
        }
        ++i;
    }
}

void DecoderPool::prepare(const QString& path, int64_t timeUs, double dueInMs,
    std::shared_ptr<const std::atomic<bool>> cancelled)
{
    if (!m_prepareThreads)
        return;
    const qint64 now = m_clock.elapsed();
    const auto failed = m_failedAtMs.constFind(path);
    if (failed != m_failedAtMs.constEnd() && now - *failed < kRetryFailedMs)
        return;
    m_stats.requested.fetch_add(1, std::memory_order_relaxed);
    if (int(m_waiting.size()) >= kMaxPrepareWaiting)
        return; // more cuts ahead than can be prepared in time: those decode at the cut
    m_waiting.push_back({path, timeUs, now + qint64(dueInMs), std::move(cancelled)});
    pumpWishes(now);
}

void DecoderPool::pumpWishes(qint64 now)
{
    while (!m_waiting.empty()) {
        const Wish& wish = m_waiting.front();
        // Hopeless wishes go: past due, cancelled, or less than half their cost left.
        const qint64 halfCost = m_stats.lastCostMs.load(std::memory_order_relaxed) / 2;
        if (now + halfCost > wish.dueMs
            || (wish.cancelled && wish.cancelled->load(std::memory_order_relaxed))) {
            m_waiting.pop_front();
            continue;
        }
        int reserved = 0;
        int running = 0;
        for (const auto& e : m_entries) {
            if (e->reservation) {
                ++reserved;
                running += workerRunning(*e) ? 1 : 0;
            }
        }
        if (reserved >= kMaxReserved || running >= kMaxPrepareRunning)
            return;
        startPreparation(wish, now);
        m_waiting.pop_front();
    }
}

void DecoderPool::startPreparation(const Wish& wish, qint64 now)
{
    // An open decoder of this file that nobody has used for a while saves the open().
    Entry* entry = nullptr;
    int perPath = 0;
    for (auto& e : m_entries) {
        if (e->path != wish.path)
            continue;
        ++perPath;
        if (!entry && !e->reservation && !e->busy && !workerRunning(*e)
            && now - e->lastUsedMs >= kRecycleIdleMs) {
            entry = e.get();
        }
    }
    const bool reuse = entry != nullptr;
    if (!reuse) {
        if (perPath >= kMaxPerPath - 1)
            return; // keep room for the requests that cannot wait
        auto created = std::make_unique<Entry>();
        created->decoder = std::make_unique<media::VideoDecoder>();
        created->path = wish.path;
        entry = created.get();
        m_entries.push_back(std::move(created));
    }
    entry->waitAhead(); // a finished decode-ahead
    auto reservation = std::make_shared<Reservation>();
    reservation->cancelled = wish.cancelled;
    reservation->dueMs = wish.dueMs;
    entry->reservation = reservation;
    entry->previousTimeUs = entry->lastTimeUs;
    entry->lastTimeUs = wish.timeUs;
    entry->lastUsedMs = now;

    auto done = std::make_shared<std::promise<void>>();
    entry->ahead = done->get_future();
    media::VideoDecoder* decoder = entry->decoder.get();
    AtomicStats* stats = &m_stats;
    m_prepareThreads->start(
        [decoder, path = wish.path, timeUs = wish.timeUs, reuse, reservation, done, stats,
            gate = m_zeroCopyGate] {
            const std::function<bool()> stop = [&reservation] { return reservation->stale(); };
            QElapsedTimer timer;
            timer.start();
            QString error;
            if (!stop() && (reuse || openDecoder(*decoder, path, &error, gate)) && !stop()
                && decoder->prepareFrame(
                    timeUs, stop, s_readAheadFrames.load(std::memory_order_relaxed))) {
                stats->ready.fetch_add(1, std::memory_order_relaxed);
                stats->lastCostMs.store(int(timer.elapsed()), std::memory_order_relaxed);
            }
            done->set_value();
        },
        kPreparePriority);

    m_stats.started.fetch_add(1, std::memory_order_relaxed);
    if (reuse)
        m_stats.reused.fetch_add(1, std::memory_order_relaxed);
    int reserved = 0;
    int running = 0;
    for (const auto& e : m_entries) {
        if (e->reservation) {
            ++reserved;
            running += workerRunning(*e) ? 1 : 0;
        }
    }
    int peak = m_stats.peakReserved.load(std::memory_order_relaxed);
    m_stats.peakReserved.store(std::max(peak, reserved), std::memory_order_relaxed);
    peak = m_stats.peakRunning.load(std::memory_order_relaxed);
    m_stats.peakRunning.store(std::max(peak, running), std::memory_order_relaxed);
}

} // namespace kk::render
