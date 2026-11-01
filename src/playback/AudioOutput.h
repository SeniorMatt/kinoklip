// SPDX-License-Identifier: GPL-3.0-or-later
// AudioOutput — the sound device behind the playback engine (interface + QAudioSink
// implementation). Tests substitute a fake that consumes at a simulated rate.
#pragma once

#include <QString>

#include <cstdint>
#include <memory>

namespace kk {

class AudioRing;

/// Monotonic wall clock in microseconds (std::chrono::steady_clock); the common time base of
/// the playback clock, the audio device position stamps and the engine's tick.
[[nodiscard]] int64_t monotonicUs();

/// A sound output pulling 48 kHz interleaved stereo float frames from an AudioRing.
///
/// Contract:
/// * start() begins pulling from the ring (the device may start a few ms later); stop() stops
///   and returns only once the device no longer touches the ring.
/// * On underrun the device plays silence and later *skips* the same number of ring frames,
///   so "device frame n" always carries timeline sample `startSample + n` (A/V sync survives
///   hiccups; the audio jumps instead of lagging behind).
/// * playedFrames() reports how many frames since start() have actually been *heard*
///   (device latency compensated) and when that count was measured.
///
/// Thread-safety: start/stop/setVolume on the GUI thread; playedFrames() from any thread.
class AudioOutput
{
public:
    virtual ~AudioOutput() = default;

    /// True if a real device exists (false: the engine uses its free-running clock).
    [[nodiscard]] virtual bool isAvailable() const = 0;
    /// Starts pulling from @p ring (not owned; must outlive stop()). False if it failed.
    virtual bool start(AudioRing* ring) = 0;
    /// Stops pulling; afterwards the ring may be cleared.
    virtual void stop() = 0;
    /// Linear output volume 0..1.
    virtual void setVolume(float volume) = 0;
    /// Frames heard since start() (>= 0) and the monotonicUs() time of that measurement.
    virtual void playedFrames(int64_t* frames, int64_t* atUs) const = 0;
    /// Device description for logs ("Built-in Audio Analog Stereo (float)").
    [[nodiscard]] virtual QString description() const = 0;
};

/// The default output device through QAudioSink in pull mode, on its own thread
/// (48 kHz stereo float, or int16 when the device rejects float). The environment variable
/// KK_NO_AUDIO=1 makes it report no device (tests never make a sound).
[[nodiscard]] std::unique_ptr<AudioOutput> createDefaultAudioOutput();

} // namespace kk
