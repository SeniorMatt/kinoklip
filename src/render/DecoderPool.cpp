// SPDX-License-Identifier: GPL-3.0-or-later
// DecoderPool — video decoder reuse by path and position.
#include "render/DecoderPool.h"

#include "render/RenderLog.h"

#include <limits>

namespace kk::render {

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
}

DecoderPool::Entry* DecoderPool::acquire(const QString& path, int64_t timeUs)
{
    const qint64 now = m_clock.elapsed();
    Entry* forward = nullptr; // idle, target reachable by decoding forward (best: closest)
    int64_t forwardDistance = std::numeric_limits<int64_t>::max();
    Entry* anyIdle = nullptr; // idle, needs a seek (best: closest)
    int64_t idleDistance = std::numeric_limits<int64_t>::max();
    int perPath = 0;
    for (auto& e : m_entries) {
        if (e->path != path)
            continue;
        ++perPath;
        if (e->busy)
            continue;
        const int64_t delta = timeUs - e->lastTimeUs;
        if (e->lastTimeUs >= 0 && delta >= 0 && delta <= kForwardWindowUs) {
            if (delta < forwardDistance) {
                forward = e.get();
                forwardDistance = delta;
            }
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
        if (!entry->decoder->open(path, &error)) {
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
    chosen->lastTimeUs = timeUs;
    chosen->lastUsedMs = now;
    return chosen;
}

void DecoderPool::collectIdle()
{
    const qint64 now = m_clock.elapsed();
    for (size_t i = 0; i < m_entries.size();) {
        Entry& e = *m_entries[i];
        if (!e.busy && now - e.lastUsedMs > m_idleCloseMs) {
            if (m_closeHook)
                m_closeHook(e);
            m_entries.erase(m_entries.begin() + qsizetype(i));
        } else {
            ++i;
        }
    }
}

void DecoderPool::clear()
{
    for (auto& e : m_entries) {
        if (m_closeHook)
            m_closeHook(*e);
    }
    m_entries.clear();
    m_failedAtMs.clear();
}

} // namespace kk::render
