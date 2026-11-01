// SPDX-License-Identifier: GPL-3.0-or-later
// AudioRing — single-producer / single-consumer lock-free ring of interleaved stereo floats.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace kk {

/// Lock-free SPSC ring buffer between the mixer thread (producer) and the audio device thread
/// (consumer). Counts are in *sample frames* (one frame = kChannels floats).
///
/// Implementation: monotonically increasing 64-bit read/write counters (never wrap in
/// practice); the slot index is `counter & mask` with a power-of-two capacity, so `full`
/// and `empty` need no extra flag. The producer publishes with a release store of the write
/// counter after copying, the consumer reads with an acquire load (and vice versa), which is
/// all the synchronisation the two threads need. No allocation after construction.
///
/// clear() may only be called while neither side is running (the engine stops the device and
/// the feeder first).
class AudioRing
{
public:
    static constexpr int kChannels = 2;

    /// Capacity is rounded up to a power of two sample frames.
    explicit AudioRing(size_t minFrames)
    {
        size_t cap = 1;
        while (cap < minFrames) {
            cap <<= 1;
        }
        m_mask = cap - 1;
        m_data.assign(cap * kChannels, 0.0f);
    }

    /// Capacity in sample frames.
    [[nodiscard]] size_t capacity() const { return m_mask + 1; }
    /// Frames ready to read (consumer side; a lower bound when called by the producer).
    [[nodiscard]] size_t available() const
    {
        return size_t(
            m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire));
    }
    /// Free frames (producer side).
    [[nodiscard]] size_t space() const { return capacity() - available(); }

    /// Producer: copies up to @p frames frames from @p src; returns the number written.
    size_t write(const float* src, size_t frames)
    {
        const uint64_t w = m_write.load(std::memory_order_relaxed);
        const uint64_t r = m_read.load(std::memory_order_acquire);
        frames = std::min(frames, capacity() - size_t(w - r));
        copyIn(w, src, frames);
        m_write.store(w + frames, std::memory_order_release);
        return frames;
    }

    /// Consumer: copies up to @p frames frames into @p dst; returns the number read.
    size_t read(float* dst, size_t frames)
    {
        const uint64_t r = m_read.load(std::memory_order_relaxed);
        const uint64_t w = m_write.load(std::memory_order_acquire);
        frames = std::min(frames, size_t(w - r));
        copyOut(r, dst, frames);
        m_read.store(r + frames, std::memory_order_release);
        return frames;
    }

    /// Total frames consumed since construction / clear() (any thread).
    [[nodiscard]] uint64_t totalRead() const { return m_read.load(std::memory_order_acquire); }

    /// Empties the ring. Only while producer and consumer are both idle.
    void clear()
    {
        m_read.store(0, std::memory_order_relaxed);
        m_write.store(0, std::memory_order_release);
    }

private:
    // Copy with at most one wrap-around split (two memcpy calls).
    void copyIn(uint64_t pos, const float* src, size_t frames)
    {
        const size_t start = size_t(pos & m_mask);
        const size_t first = std::min(frames, capacity() - start);
        std::memcpy(&m_data[start * kChannels], src, first * kChannels * sizeof(float));
        if (frames > first) {
            std::memcpy(m_data.data(), src + first * kChannels,
                (frames - first) * kChannels * sizeof(float));
        }
    }
    void copyOut(uint64_t pos, float* dst, size_t frames) const
    {
        const size_t start = size_t(pos & m_mask);
        const size_t first = std::min(frames, capacity() - start);
        std::memcpy(dst, &m_data[start * kChannels], first * kChannels * sizeof(float));
        if (frames > first) {
            std::memcpy(dst + first * kChannels, m_data.data(),
                (frames - first) * kChannels * sizeof(float));
        }
    }

    std::vector<float> m_data;
    size_t m_mask = 0;
    // Separate cache lines: the two threads each write one counter.
    alignas(64) std::atomic<uint64_t> m_write {0};
    alignas(64) std::atomic<uint64_t> m_read {0};
};

} // namespace kk
