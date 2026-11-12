// SPDX-License-Identifier: GPL-3.0-or-later
// DecoderPool — reusable media::VideoDecoder instances per media path, owned by one thread.
#pragma once

#include "media/VideoDecoder.h"

#include <QElapsedTimer>
#include <QHash>
#include <QString>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <vector>

class QThreadPool;

namespace kk::render {

/// Pool of open video decoders, keyed by media path, several per path.
///
/// Why several per path: two clips cut from the same file are often visible at the same time
/// (a cross dissolve between two takes of one recording, picture-in-picture of the same
/// source). One decoder would seek back and forth every frame; one decoder per *active use*
/// keeps every use on the decoder's fast sequential path.
///
/// acquire() picks, among the idle decoders of a path, the one whose last position is closest
/// before the requested time (so playback keeps decoding forward), else any idle one, else
/// opens a new decoder. A decoder handed out is "busy" until the next beginFrame().
/// Decoders unused for `idleCloseMs` are closed by collectIdle(). Files that fail to open are
/// remembered for kRetryFailedMs so offline media does not cost an open() per frame.
///
/// Look-ahead: prepare() gets a decoder ready for a request that comes later (the first frame
/// after a cut) on a worker thread: opened, seeked and decoded forward to that frame. Until
/// then the entry is *reserved*: acquire() hands it out for its own position only. A request
/// that arrives before the preparation is done waits for the rest of it. At most kMaxReserved
/// decoders are reserved and kMaxPrepareRunning preparing at a time, which is what bounds the
/// memory (each holds its decoded frames and its reference pictures); further wishes wait in a
/// short list until their due time has passed. A reservation ends when its request arrives, when
/// the request of a later reservation arrives (the frames between were skipped), kPrepareGraceMs
/// after its due time, or when its `cancelled` flag is set (a seek, an edit). When cuts come faster
/// than decoders can get ready, a wish that cannot be nearly done in time is not started (see the
/// last preparation's cost); its clip decodes at the cut, as if there were no look-ahead.
///
/// Thread-safety: NOT thread-safe; owned and used by one thread (the render thread). Decoders
/// handed out may be used on worker threads in between (parallel decode) as long as the
/// owner does not call acquire()/beginFrame()/collectIdle() meanwhile. prepareStats() may be
/// read from any thread.
class DecoderPool
{
public:
    /// Counters of the look-ahead since the pool was created.
    struct PrepareStats
    {
        int lastCostMs = 0; ///< wall time of the last finished preparation (open + seek + decode)
        int64_t requested = 0; ///< prepare() calls
        int64_t started = 0; ///< preparations started on a worker thread
        int64_t reused = 0; ///< ... of those on an idle open decoder instead of a new one
        int64_t ready = 0; ///< ... finished (the frame is decoded)
        int64_t hitsReady = 0; ///< acquire() got a prepared decoder that was already ready
        int64_t hitsWaited = 0; ///< ... that was still preparing: it waited for the rest
        int64_t cancelled = 0; ///< reservations ended by the `cancelled` flag
        int64_t expired = 0; ///< reservations unused until after their due time
        int peakReserved = 0; ///< most decoders reserved at one time
        int peakRunning = 0; ///< most preparations running at one time
    };

    /// Decoders reserved by the look-ahead at most.
    /// Frames decoded after the first one of a preparation (VideoDecoder::prepareFrame()): the
    /// first frames of a cut are ready too, not only the cut frame (spec 72.7).
    static constexpr int kReadAheadFrames = 3;
    static constexpr int kMaxReserved = 3;
    /// Open decoders at most (idle ones are closed, least recently used first, to open a new
    /// one; decoders in use or reserved are never closed, so a frame needing more still works).
    static constexpr int kMaxOpen = 8;
    /// Preparations running at the same time at most (leave threads for the visible clips).
    static constexpr int kMaxPrepareRunning = 2;
    /// Wishes waiting for a free reservation at most.
    static constexpr int kMaxPrepareWaiting = 8;
    /// A reservation or wish stays this long after its due time (the frame may be late).
    static constexpr qint64 kPrepareGraceMs = 250;
    /// An open decoder unused for this long may be re-positioned for a later clip.
    static constexpr qint64 kRecycleIdleMs = 300;

    /// State shared between a reservation and its worker.
    struct Reservation
    {
        std::atomic<bool> stop {false};
        std::shared_ptr<const std::atomic<bool>> cancelled;
        qint64 dueMs = 0; ///< pool clock
        [[nodiscard]] bool stale() const
        {
            return stop.load(std::memory_order_relaxed)
                || (cancelled && cancelled->load(std::memory_order_relaxed));
        }
    };

    /// One pooled decoder. `userData` lets the owner attach per-decoder resources (the GL
    /// renderer keeps its plane textures here so a still image is uploaded only once).
    struct Entry
    {
        std::unique_ptr<media::VideoDecoder> decoder;
        QString path;
        int64_t lastTimeUs = -1; ///< media time of the last request, -1 = none yet
        int64_t previousTimeUs = -1; ///< media time of the request before that
        /// Decode-ahead running on a worker thread (see GlRenderer); the decoder must not be
        /// used before waitAhead().
        std::future<void> ahead;
        qint64 lastUsedMs = 0; ///< pool clock of the last acquire()
        bool busy = false; ///< handed out during the current frame
        bool skipNonReference = false; ///< what the decoder was last told (setSkipNonReference())
        /// Set while the look-ahead holds this decoder (see the class comment). `decoder` must
        /// not be touched before waitAhead(): it may still be opening on a worker thread.
        std::shared_ptr<Reservation> reservation;
        std::shared_ptr<void> userData;

        /// Waits for a decode-ahead in flight (no-op without one).
        void waitAhead()
        {
            if (ahead.valid())
                ahead.wait();
            ahead = {};
        }
    };

    /// Called right before an entry is closed/destroyed (on the owning thread).
    using CloseHook = std::function<void(Entry&)>;

    DecoderPool();
    ~DecoderPool();
    DecoderPool(const DecoderPool&) = delete;
    DecoderPool& operator=(const DecoderPool&) = delete;

    /// Sets the hook run before an entry is closed (e.g. to delete GL textures).
    /// Test hook: every open() of a decoder takes this much longer (a hardware decoder's
    /// session setup can take hundreds of ms). Process-wide; 0 = off.
    static void setOpenDelayMs(int ms);
    /// Test hook: frames decoded after the first one of a preparation (default
    /// kReadAheadFrames; 0 = the cut frame only, as before 72.7). Process-wide.
    static void setReadAheadFrames(int frames);

    void setCloseHook(CloseHook hook) { m_closeHook = std::move(hook); }
    /// Zero-copy decoding (media::VideoDecoder::setZeroCopy()): decoders opened from now on
    /// hand VAAPI frames over as DMA-BUF handles while @p gate reads true. The owner (the GL
    /// renderer, which can import them) clears the gate when it cannot. Without a gate the
    /// decoders download as before. Call before the first acquire().
    void setZeroCopyGate(std::shared_ptr<const std::atomic<bool>> gate)
    {
        m_zeroCopyGate = std::move(gate);
    }
    /// Decoders idle longer than this are closed by collectIdle(). Default 10 s.
    void setIdleCloseMs(qint64 ms) { m_idleCloseMs = ms; }

    /// Worker threads of the look-ahead (not owned, must outlive the pool). Without them
    /// prepare() does nothing.
    void setPrepareThreads(QThreadPool* threads) { m_prepareThreads = threads; }
    /// Catch-up mode (RenderRequest::catchUp): decoders handed out from now on discard the
    /// frames nothing refers to, see media::VideoDecoder::setSkipNonReference().
    void setCatchUp(bool catchUp) { m_catchUp = catchUp; }

    /// Marks every decoder idle again, ends stale reservations and starts waiting
    /// preparations; call once at the start of each frame.
    void beginFrame();
    /// A decoder for @p path, positioned as close as possible before @p timeUs, marked busy.
    /// Opens a new one if needed. The decoder is open and not in use by a worker, except for
    /// a decode-ahead (Entry::ahead). @return nullptr if the file cannot be opened.
    Entry* acquire(const QString& path, int64_t timeUs);
    /// Look-ahead: get a decoder ready for acquire(@p path, @p timeUs) in about @p dueInMs.
    /// Never blocks. Stale when @p cancelled (may be null) becomes true.
    void prepare(const QString& path, int64_t timeUs, double dueInMs,
        std::shared_ptr<const std::atomic<bool>> cancelled);
    /// Decoders currently reserved by the look-ahead.
    [[nodiscard]] int reservedCount() const;
    /// Wishes waiting for a free reservation.
    [[nodiscard]] int waitingCount() const { return int(m_waiting.size()); }
    /// Snapshot of the counters; any thread.
    [[nodiscard]] PrepareStats prepareStats() const;
    /// Tells every preparation to give up (before waiting for the worker threads at shutdown).
    void stopPreparing();
    /// Closes decoders unused for longer than the idle limit. Cheap; call once per frame.
    void collectIdle();
    /// Closes everything.
    void clear();
    /// Number of open decoders (for tests and stats).
    [[nodiscard]] int openCount() const { return int(m_entries.size()); }

private:
    /// Media time window in which an idle decoder counts as "positioned for" a request:
    /// targets up to this far ahead are reached by decoding forward instead of seeking.
    static constexpr int64_t kForwardWindowUs = 2'000'000;
    /// Do not retry opening a failed file for this long.
    static constexpr qint64 kRetryFailedMs = 5000;
    /// Hard limit of decoders per path (beyond it the closest idle one is reused anyway).
    static constexpr int kMaxPerPath = 6;

    /// Closes idle decoders until a new one fits under kMaxOpen.
    void makeRoom();

    /// A prepare() that found no free reservation yet.
    struct Wish
    {
        QString path;
        int64_t timeUs = 0;
        qint64 dueMs = 0;
        std::shared_ptr<const std::atomic<bool>> cancelled;
    };

    /// Ends the reservation of an entry whose worker is done; false if the entry must go too
    /// (its file never opened).
    bool endReservation(Entry& entry);
    /// Removes the entry at @p index, closing its decoder.
    void closeEntry(size_t index);
    /// Ends stale and overdue reservations.
    void reapReservations(qint64 now);
    /// Starts waiting wishes while there is room.
    void pumpWishes(qint64 now);
    void startPreparation(const Wish& wish, qint64 now);

    std::vector<std::unique_ptr<Entry>> m_entries;
    std::deque<Wish> m_waiting;
    QThreadPool* m_prepareThreads = nullptr;
    bool m_catchUp = false;
    /// Counters; written by the owning thread and (ready) by workers, read from anywhere.
    struct AtomicStats
    {
        std::atomic<int64_t> requested {0}, started {0}, reused {0}, ready {0}, hitsReady {0},
            hitsWaited {0}, cancelled {0}, expired {0};
        std::atomic<int> peakReserved {0}, peakRunning {0};
        std::atomic<int> lastCostMs {0}; ///< wall time of the last finished preparation
    };
    AtomicStats m_stats;
    QHash<QString, qint64> m_failedAtMs; ///< path → pool clock of the last failed open()
    QElapsedTimer m_clock;
    qint64 m_idleCloseMs = 10000;
    CloseHook m_closeHook;
    std::shared_ptr<const std::atomic<bool>> m_zeroCopyGate;
};

} // namespace kk::render
