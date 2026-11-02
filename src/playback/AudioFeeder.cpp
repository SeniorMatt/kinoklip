// SPDX-License-Identifier: GPL-3.0-or-later
// AudioFeeder — mixer thread keeping the audio ring full.
#include "playback/AudioFeeder.h"

#include "playback/AudioRing.h"
#include "render/AudioMixer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace kk {

namespace {
/// Sleep between ring checks while the ring is full (well below the ring's duration).
constexpr auto kIdleWait = std::chrono::milliseconds(4);
/// Scrub snippets fade in and out over this many frames (no clicks at the cut points).
constexpr int kScrubFadeFrames = 96;

/// Wraps varispeed position @p pos into @p loop (no-op when inactive).
double wrapIntoLoop(double pos, const SampleLoop& loop)
{
    if (!loop.active())
        return pos;
    const double len = double(loop.end - loop.start);
    while (pos >= double(loop.end))
        pos -= len;
    while (pos < double(loop.start))
        pos += len;
    return pos;
}
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
    std::shared_ptr<const TimelineData> timeline, int64_t startSample, SampleLoop loop, double rate)
{
    {
        std::lock_guard lock(m_mutex);
        m_timeline = std::move(timeline);
        m_position = startSample;
        m_loop = loop;
        if (m_loop.active() && (m_position < m_loop.start || m_position >= m_loop.end)) {
            m_position = rate < 0 ? m_loop.end - 1 : m_loop.start;
        }
        m_rate = rate;
        m_srcPos = double(m_position);
        m_scrub = false;
        m_running = true;
    }
    m_wake.notify_all();
}

void AudioFeeder::startScrub(std::shared_ptr<const TimelineData> timeline)
{
    {
        std::lock_guard lock(m_mutex);
        m_timeline = std::move(timeline);
        m_loop = SampleLoop {};
        m_rate = 1.0;
        m_scrub = true;
        m_scrubTarget = -1;
        m_scrubLeft = 0;
        m_running = true;
    }
    m_wake.notify_all();
}

void AudioFeeder::scrubTo(int64_t sample)
{
    {
        std::lock_guard lock(m_mutex);
        m_scrubTarget = std::max<int64_t>(0, sample);
    }
    m_wake.notify_all();
}

bool AudioFeeder::isScrubbing() const
{
    std::lock_guard lock(m_mutex);
    return m_running && m_scrub;
}

void AudioFeeder::mixVarispeed(
    AudioMixer& mixer, const TimelineData& timeline, int count, float* out)
{
    constexpr int ch = AudioRing::kChannels;
    // Output frame k carries timeline sample m_srcPos + rate * k. The span read covers every
    // position the chunk touches plus one sample for the interpolation.
    const double last = m_srcPos + m_rate * double(count - 1);
    const int64_t lo = int64_t(std::floor(std::min(m_srcPos, last)));
    const int64_t hi = int64_t(std::floor(std::max(m_srcPos, last))) + 1;
    const int span = int(hi - lo + 1);
    m_span.resize(size_t(span) * ch); // grows to the largest span once, then reused
    std::fill(m_span.begin(), m_span.end(), 0.0f);
    // Nothing exists before the timeline start: mix only from sample 0 on.
    const int64_t from = std::max<int64_t>(lo, 0);
    if (hi >= from)
        mixer.mix(timeline, from, int(hi - from + 1), m_span.data() + size_t(from - lo) * ch);
    for (int k = 0; k < count; ++k) {
        const double pos = m_srcPos + m_rate * double(k) - double(lo);
        const int i = std::clamp(int(pos), 0, span - 2); // span >= 2 by construction
        const float frac = float(std::clamp(pos - double(i), 0.0, 1.0));
        for (int c = 0; c < ch; ++c) {
            const float a = m_span[size_t(i) * ch + c];
            const float b = m_span[size_t(std::min(i + 1, span - 1)) * ch + c];
            out[size_t(k) * ch + c] = a + (b - a) * frac;
        }
    }
    m_srcPos += m_rate * double(count);
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
    constexpr int ch = AudioRing::kChannels;
    std::vector<float> chunk(size_t(kChunkFrames) * ch);
    std::unique_lock lock(m_mutex);
    while (!m_quit) {
        // Scrubbing keeps the ring nearly empty so the sound follows the playhead closely.
        const bool full = m_scrub ? m_ring->available() >= kScrubFillFrames
                                  : m_ring->space() < size_t(kChunkFrames);
        if (!m_running || full || m_ring->space() < size_t(kChunkFrames) || !m_timeline) {
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
        const double rate = m_rate;
        const bool scrub = m_scrub;
        if (scrub && m_scrubTarget >= 0) {
            // A new scrub position replaces the snippet being played.
            m_scrubPos = m_scrubTarget;
            m_scrubLeft = kScrubSnippetFrames;
            m_scrubTarget = -1;
        }
        const int64_t scrubPos = m_scrubPos;
        const int scrubLeft = m_scrubLeft;
        m_busy = true;
        lock.unlock();

        if (scrub) {
            // One chunk of the snippet (faded at both ends), silence after it.
            std::fill(chunk.begin(), chunk.end(), 0.0f);
            const int count = std::min(kChunkFrames, scrubLeft);
            if (count > 0) {
                mixer.mix(*timeline, scrubPos, count, chunk.data());
                const int done = kScrubSnippetFrames - scrubLeft;
                for (int k = 0; k < count; ++k) {
                    const int fromStart = done + k;
                    const int toEnd = kScrubSnippetFrames - 1 - fromStart;
                    const float g = float(std::min({fromStart, toEnd, kScrubFadeFrames}))
                        / float(kScrubFadeFrames);
                    for (int c = 0; c < ch; ++c)
                        chunk[size_t(k) * ch + c] *= std::min(1.0f, g);
                }
            }
            lock.lock();
            m_busy = false;
            if (m_running && m_scrub) {
                m_ring->write(chunk.data(), size_t(kChunkFrames));
                if (m_scrubTarget < 0 && m_scrubPos == scrubPos) {
                    m_scrubPos += count;
                    m_scrubLeft = scrubLeft - count;
                }
            }
            m_filled.notify_all();
            continue;
        }

        if (rate != 1.0) {
            // Varispeed: a chunk never crosses the loop boundary in its playing direction.
            int done = 0;
            while (done < kChunkFrames) {
                int count = kChunkFrames - done;
                if (loop.active()) {
                    const double room = rate > 0 ? (double(loop.end) - m_srcPos) / rate
                                                 : (m_srcPos - double(loop.start)) / -rate;
                    count = std::clamp(int(std::ceil(room)), 1, count);
                }
                mixVarispeed(mixer, *timeline, count, chunk.data() + size_t(done) * ch);
                done += count;
                m_srcPos = wrapIntoLoop(m_srcPos, loop);
            }
            position = int64_t(std::floor(m_srcPos));
        } else {
            // A chunk never crosses the loop end: split it so the mix jumps exactly there.
            int done = 0;
            while (done < kChunkFrames) {
                int count = kChunkFrames - done;
                if (loop.active()) {
                    count = int(std::min<int64_t>(count, loop.end - position));
                }
                mixer.mix(*timeline, position, count, chunk.data() + size_t(done) * ch);
                done += count;
                position += count;
                if (loop.active() && position >= loop.end) {
                    position = loop.start;
                }
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
