// SPDX-License-Identifier: GPL-3.0-or-later
// DecoderPool — reusable media::VideoDecoder instances per media path, owned by one thread.
#pragma once

#include "media/VideoDecoder.h"

#include <QElapsedTimer>
#include <QHash>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

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
/// Thread-safety: NOT thread-safe; owned and used by one thread (the render thread). Decoders
/// handed out may be used on worker threads in between (parallel decode) as long as the
/// owner does not call acquire()/beginFrame()/collectIdle() meanwhile.
class DecoderPool
{
public:
    /// One pooled decoder. `userData` lets the owner attach per-decoder resources (the GL
    /// renderer keeps its plane textures here so a still image is uploaded only once).
    struct Entry
    {
        std::unique_ptr<media::VideoDecoder> decoder;
        QString path;
        int64_t lastTimeUs = -1; ///< media time of the last request, -1 = none yet
        qint64 lastUsedMs = 0; ///< pool clock of the last acquire()
        bool busy = false; ///< handed out during the current frame
        std::shared_ptr<void> userData;
    };

    /// Called right before an entry is closed/destroyed (on the owning thread).
    using CloseHook = std::function<void(Entry&)>;

    DecoderPool();
    ~DecoderPool();
    DecoderPool(const DecoderPool&) = delete;
    DecoderPool& operator=(const DecoderPool&) = delete;

    /// Sets the hook run before an entry is closed (e.g. to delete GL textures).
    void setCloseHook(CloseHook hook) { m_closeHook = std::move(hook); }
    /// Decoders idle longer than this are closed by collectIdle(). Default 10 s.
    void setIdleCloseMs(qint64 ms) { m_idleCloseMs = ms; }

    /// Marks every decoder idle again; call once at the start of each frame.
    void beginFrame();
    /// A decoder for @p path, positioned as close as possible before @p timeUs, marked busy.
    /// Opens a new one if needed. @return nullptr if the file cannot be opened.
    Entry* acquire(const QString& path, int64_t timeUs);
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

    std::vector<std::unique_ptr<Entry>> m_entries;
    QHash<QString, qint64> m_failedAtMs; ///< path → pool clock of the last failed open()
    QElapsedTimer m_clock;
    qint64 m_idleCloseMs = 10000;
    CloseHook m_closeHook;
};

} // namespace kk::render
