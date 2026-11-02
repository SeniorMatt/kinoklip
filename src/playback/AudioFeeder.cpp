// SPDX-License-Identifier: GPL-3.0-or-later
// AudioFeeder — mixer thread keeping the audio ring full.
#include "playback/AudioFeeder.h"

#include "playback/AudioRing.h"
#include "render/AudioMixer.h"
#include "render/LoudnessMeter.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace kk {

namespace {
/// Sleep between ring checks while the ring is full (well below the ring's duration).
constexpr auto kIdleWait = std::chrono::milliseconds(4);
/// meterFrameAt() accepts a frame up to this many samples before the asked one (0.5 s).
constexpr int64_t kMeterMaxAge = 24000;
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

bool AudioFeeder::meterFrameAt(int64_t sample, MeterFrame* out) const
{
    std::lock_guard lock(m_meterMutex);
    const MeterFrame* best = nullptr;
    for (const MeterFrame& f : m_meters) {
        if (f.sample < 0 || f.sample > sample || sample - f.sample > kMeterMaxAge)
            continue;
        if (!best || f.sample > best->sample)
            best = &f;
    }
    if (!best)
        return false;
    *out = *best;
    return true;
}

void AudioFeeder::clearMeters()
{
    std::lock_guard lock(m_meterMutex);
    for (MeterFrame& f : m_meters)
        f.sample = -1;
}

void AudioFeeder::run()
{
    AudioMixer mixer; // used only on this thread
    std::vector<float> chunk(size_t(kChunkFrames) * AudioRing::kChannels);
    // Metering state of this thread (allocated once).
    auto loudness = std::make_unique<dsp::LoudnessMeter>();
    auto levels = std::make_unique<MixLevels>();
    auto chunkLevels = std::make_unique<MixLevels>();
    auto frame = std::make_unique<MeterFrame>();
    mixer.setLevels(levels.get());
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
        const int64_t chunkStart = position;
        chunkLevels->clear();
        int done = 0;
        while (done < kChunkFrames) {
            int count = kChunkFrames - done;
            if (loop.active()) {
                count = int(std::min<int64_t>(count, loop.end - position));
            }
            mixer.mix(
                *timeline, position, count, chunk.data() + size_t(done) * AudioRing::kChannels);
            chunkLevels->merge(*levels);
            done += count;
            position += count;
            if (loop.active() && position >= loop.end) {
                position = loop.start;
            }
        }

        // Meters: loudness of the output, then publish the time-stamped frame.
        if (m_resetLoudness.exchange(false))
            loudness->reset();
        loudness->process(chunk.data(), kChunkFrames);
        frame->sample = chunkStart;
        frame->levels = *chunkLevels;
        frame->momentaryLufs = loudness->momentary();
        frame->shortTermLufs = loudness->shortTerm();
        frame->integratedLufs = loudness->integrated();
        frame->truePeakDb = loudness->truePeakRecent();
        frame->truePeakMaxDb = loudness->truePeakMax();
        {
            std::lock_guard meterLock(m_meterMutex);
            m_meters[size_t(m_meterHead)] = *frame;
            m_meterHead = (m_meterHead + 1) % kMeterFrames;
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
