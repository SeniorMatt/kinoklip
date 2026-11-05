// SPDX-License-Identifier: GPL-3.0-or-later
// AudioFeeder — worker thread that mixes the timeline (AudioMixer) ahead of the audio device
// into an AudioRing.
#pragma once

#include "core/TimelineData.h"
#include "playback/VarispeedResampler.h"
#include "render/MixLevels.h"
#include "render/RenderTypes.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace kk {

class AudioMixer;
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
/// `startSample + rate * n`, wrapped into the loop range when one is set (the mix jumps from
/// loop.end - 1 to loop.start, so a loop plays seamlessly).
///
/// Varispeed (rate != 1: shuttle, reverse, slow play; spec 1.16): like a tape, the pitch
/// follows the speed. Each chunk mixes the span of timeline samples it covers (|rate| times
/// the chunk plus the kernel reach, read forwards) and resamples it with VarispeedResampler
/// (windowed sinc, low-passed below the output Nyquist frequency at |rate| > 1, so 4x - 8x
/// shuttling does not alias), in reverse order for negative rates. Forwards, the mixed
/// window slides: only the new samples are mixed each chunk, so the mixer runs continuously.
///
/// Scrubbing (startScrub(); spec 23.7): the ring is kept nearly empty (kScrubFillFrames) so
/// what is heard follows the playhead closely; each scrubTo() plays one short snippet
/// (kScrubSnippetFrames, faded in and out) from that position, silence otherwise.
///
/// Metering: every chunk's track / bus / output peaks (AudioMixer::setLevels()) and the
/// output's loudness (dsp::LoudnessMeter, BS.1770) go into a small ring of time-stamped
/// MeterFrame entries; meterFrameAt() picks the one for the sample being heard (the mix runs
/// ~170 ms ahead of the device). The ring is guarded by its own mutex, held only for the copy.
///
/// Thread-safety: all public methods on one controlling thread (GUI). start()/stop() hand
/// over the ring: after stop() returns the feeder no longer writes to it.
class AudioFeeder
{
public:
    static constexpr int kChunkFrames = 1024; ///< mix block (21 ms at 48 kHz)
    /// Scrubbing: frames kept in the ring at most (low latency, ~43 ms at 48 kHz).
    static constexpr size_t kScrubFillFrames = 2048;
    /// Scrubbing: length of the snippet played per scrubTo() (~80 ms at 48 kHz).
    static constexpr int kScrubSnippetFrames = 3840;
    /// remixUnheard(): frames of the old sound still played (~43 ms at 48 kHz).
    static constexpr size_t kRewindKeepFrames = 2048;

    /// @p ring is not owned and must outlive the feeder.
    explicit AudioFeeder(AudioRing* ring);
    ~AudioFeeder();
    AudioFeeder(const AudioFeeder&) = delete;
    AudioFeeder& operator=(const AudioFeeder&) = delete;

    /// Starts filling the (empty) ring from timeline sample @p startSample at speed @p rate
    /// (1 = normal; negative = reverse; see the class comment for varispeed).
    void start(std::shared_ptr<const TimelineData> timeline, int64_t startSample, SampleLoop loop,
        double rate = 1.0);
    /// Starts scrub mode on the (empty) ring: silence until scrubTo().
    void startScrub(std::shared_ptr<const TimelineData> timeline);
    /// Scrub mode: plays a snippet from timeline sample @p sample (replaces one not yet mixed).
    void scrubTo(int64_t sample);
    /// True while in scrub mode (between startScrub() and stop()).
    [[nodiscard]] bool isScrubbing() const;
    /// Stops writing (blocks until the current chunk is done).
    void stop();
    /// Replaces the timeline mixed from the next chunk on (live edits during playback).
    void setTimeline(std::shared_ptr<const TimelineData> timeline);
    /// An edit changed what is about to be heard: the ring's unheard sound is mixed again from
    /// the current timeline, except kRewindKeepFrames (the device needs something to read while
    /// the first new chunk is mixed). No device restart: the position stays exact (spec 48.7).
    void remixUnheard();
    /// Media id -> path table for the mixer.
    void setMediaPaths(std::shared_ptr<const MediaPathMap> media);
    /// Blocks until at least @p frames are in the ring or @p timeoutMs passed (pre-roll).
    /// Returns the frames available.
    size_t waitForFill(size_t frames, int timeoutMs);

    /// Copies the meter frame of the mixed block that contains timeline sample @p sample
    /// (else the latest one before it, at most ~0.5 s older) into @p out. Returns false (and
    /// leaves @p out alone) when there is none.
    bool meterFrameAt(int64_t sample, MeterFrame* out) const;
    /// Restarts the integrated loudness and the maximum true peak (from the next chunk).
    void resetLoudness() { m_resetLoudness = true; }
    /// Forgets the meter frames (after stop, so a later start shows no stale levels).
    void clearMeters();

private:
    void run();
    /// Mixes @p count output frames at m_rate from m_srcPos into @p out (varispeed path).
    void mixVarispeed(AudioMixer& mixer, const TimelineData& timeline, int count, float* out);
    /// Makes m_span hold timeline samples [@p lo, @p hi] (varispeed window; worker thread).
    void ensureWindow(AudioMixer& mixer, const TimelineData& timeline, int64_t lo, int64_t hi);

    AudioRing* m_ring;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_filled;
    bool m_quit = false;
    bool m_running = false;
    bool m_busy = false; ///< the thread is inside a mix (stop() waits for it)
    std::shared_ptr<const TimelineData> m_timeline;
    std::shared_ptr<const MediaPathMap> m_media;
    bool m_mediaDirty = false;
    int64_t m_position = 0; ///< next timeline sample to write (rate 1)
    SampleLoop m_loop;
    double m_rate = 1.0;
    double m_srcPos = 0.0; ///< varispeed: timeline sample of the next output frame
    std::vector<float> m_span; ///< varispeed: mixed source window (worker thread only)
    int64_t m_spanStart = 0; ///< varispeed: timeline sample of m_span's first frame
    int64_t m_spanFrames = 0; ///< varispeed: valid frames in m_span (0 = none)
    const TimelineData* m_spanTimeline = nullptr; ///< timeline the window was mixed from
    VarispeedResampler m_resampler; ///< kernel table (read-only after construction)
    bool m_remixWanted = false; ///< remixUnheard() was called
    bool m_remixWaiting = false; ///< waiting for the device to carry out the ring's rewind
    int m_fadeInLeft = 0; ///< frames of the new sound still to fade in (worker thread only)
    bool m_scrub = false; ///< scrub mode
    int64_t m_scrubTarget = -1; ///< pending snippet start (-1 = none)
    int64_t m_scrubPos = 0; ///< current snippet position
    int m_scrubLeft = 0; ///< frames of the current snippet still to mix

    static constexpr int kMeterFrames = 32; ///< ~680 ms of chunks (the ring holds ~170 ms)
    mutable std::mutex m_meterMutex;
    std::array<MeterFrame, kMeterFrames> m_meters {};
    int m_meterHead = 0;
    std::atomic<bool> m_resetLoudness {false};
};

} // namespace kk
