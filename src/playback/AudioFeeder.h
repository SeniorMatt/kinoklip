// SPDX-License-Identifier: GPL-3.0-or-later
// AudioFeeder — worker thread that mixes the timeline (AudioMixer) ahead of the audio device
// into an AudioRing.
#pragma once

#include "core/TimelineData.h"
#include "render/RenderTypes.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace kk {

class AudioRing;

/// Loop range in 48 kHz timeline samples, [start, end); inactive when end <= start.
struct SampleLoop
{
    int64_t start = 0;
    int64_t end = 0;
    [[nodiscard]] bool active() const { return end > start; }
};

/// Keeps an AudioRing full with mixed timeline audio while running.
///
/// Owns a thread (started in the constructor, sleeping when idle) and the AudioMixer, which
/// is used only on that thread (the mixer is not thread-safe). Mixing happens in kChunkFrames
/// blocks whenever the ring has room; the thread then sleeps for a few ms. Opening a decoder
/// can take several ms, which the ring (~170 ms) absorbs.
///
/// Sample mapping: the n-th frame written after start() carries timeline sample
/// `startSample + n`, wrapped into the loop range when one is set (the mix jumps from
/// loop.end - 1 to loop.start, so a loop plays seamlessly).
///
/// Thread-safety: all public methods on one controlling thread (GUI). start()/stop() hand
/// over the ring: after stop() returns the feeder no longer writes to it.
class AudioFeeder
{
public:
    static constexpr int kChunkFrames = 1024; ///< mix block (21 ms at 48 kHz)

    /// @p ring is not owned and must outlive the feeder.
    explicit AudioFeeder(AudioRing* ring);
    ~AudioFeeder();
    AudioFeeder(const AudioFeeder&) = delete;
    AudioFeeder& operator=(const AudioFeeder&) = delete;

    /// Starts filling the (empty) ring from timeline sample @p startSample.
    void start(std::shared_ptr<const TimelineData> timeline, int64_t startSample, SampleLoop loop);
    /// Stops writing (blocks until the current chunk is done).
    void stop();
    /// Replaces the timeline mixed from the next chunk on (live edits during playback).
    void setTimeline(std::shared_ptr<const TimelineData> timeline);
    /// Media id -> path table for the mixer.
    void setMediaPaths(std::shared_ptr<const MediaPathMap> media);
    /// Blocks until at least @p frames are in the ring or @p timeoutMs passed (pre-roll).
    /// Returns the frames available.
    size_t waitForFill(size_t frames, int timeoutMs);

private:
    void run();

    AudioRing* m_ring;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_filled;
    bool m_quit = false;
    bool m_running = false;
    bool m_busy = false; ///< the thread is inside a mix (stop() waits for it)
    std::shared_ptr<const TimelineData> m_timeline;
    std::shared_ptr<const MediaPathMap> m_media;
    bool m_mediaDirty = false;
    int64_t m_position = 0; ///< next timeline sample to write
    SampleLoop m_loop;
};

} // namespace kk
