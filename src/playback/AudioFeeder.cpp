// SPDX-License-Identifier: GPL-3.0-or-later
// AudioFeeder — mixer thread keeping the audio ring full.
#include "playback/AudioFeeder.h"

#include "playback/AudioRing.h"
#include "render/AudioMixer.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace kk {

namespace {
/// Sleep between ring checks while the ring is full (well below the ring's duration).
constexpr auto kIdleWait = std::chrono::milliseconds(4);
} // namespace

AudioFeeder::AudioFeeder(AudioRing* ring)
    : m_ring(ring)
{
    m_thread = std::thread([this] { run(); });
}

AudioFeeder::~AudioFeeder()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
        m_running = false;
    }
    m_wake.notify_all();
    m_thread.join();
}

void AudioFeeder::start(
    std::shared_ptr<const TimelineData> timeline, int64_t startSample, SampleLoop loop)
{
    {
        std::lock_guard lock(m_mutex);
        m_timeline = std::move(timeline);
        m_position = startSample;
        m_loop = loop;
        if (m_loop.active() && (m_position < m_loop.start || m_position >= m_loop.end)) {
            m_position = m_loop.start;
        }
        m_running = true;
    }
    m_wake.notify_all();
}

void AudioFeeder::stop()
{
    std::unique_lock lock(m_mutex);
    m_running = false;
    // Wait for an in-progress chunk so the caller may clear the ring right after.
    m_filled.wait(lock, [this] { return !m_busy; });
}

void AudioFeeder::setTimeline(std::shared_ptr<const TimelineData> timeline)
{
    std::lock_guard lock(m_mutex);
    m_timeline = std::move(timeline);
}

void AudioFeeder::setMediaPaths(std::shared_ptr<const MediaPathMap> media)
{
    std::lock_guard lock(m_mutex);
    m_media = std::move(media);
    m_mediaDirty = true;
}

size_t AudioFeeder::waitForFill(size_t frames, int timeoutMs)
{
    std::unique_lock lock(m_mutex);
    m_filled.wait_for(lock, std::chrono::milliseconds(timeoutMs),
        [this, frames] { return m_ring->available() >= frames || !m_running; });
    return m_ring->available();
}

void AudioFeeder::run()
{
    AudioMixer mixer; // used only on this thread
    std::vector<float> chunk(size_t(kChunkFrames) * AudioRing::kChannels);
    std::unique_lock lock(m_mutex);
    while (!m_quit) {
        if (!m_running || m_ring->space() < size_t(kChunkFrames) || !m_timeline) {
            // Idle or full: sleep until woken (start/quit) or the device made room.
            m_wake.wait_for(lock, kIdleWait);
            continue;
        }
        // Take what the chunk needs, then mix without holding the lock.
        std::shared_ptr<const TimelineData> timeline = m_timeline;
        if (m_mediaDirty) {
            mixer.setMediaPaths(m_media);
            m_mediaDirty = false;
        }
        int64_t position = m_position;
        const SampleLoop loop = m_loop;
        m_busy = true;
        lock.unlock();

        // A chunk never crosses the loop end: split it so the mix jumps exactly there.
        int done = 0;
        while (done < kChunkFrames) {
            int count = kChunkFrames - done;
            if (loop.active()) {
                count = int(std::min<int64_t>(count, loop.end - position));
            }
            mixer.mix(
                *timeline, position, count, chunk.data() + size_t(done) * AudioRing::kChannels);
            done += count;
            position += count;
            if (loop.active() && position >= loop.end) {
                position = loop.start;
            }
        }

        lock.lock();
        m_busy = false;
        if (m_running) {
            // The ring had room for a chunk when we started and only the consumer ran since.
            m_ring->write(chunk.data(), size_t(kChunkFrames));
            m_position = position;
        }
        m_filled.notify_all();
    }
    m_busy = false;
    m_filled.notify_all();
}

} // namespace kk
