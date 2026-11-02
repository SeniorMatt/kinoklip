// SPDX-License-Identifier: GPL-3.0-or-later
// PlaybackClock — maps wall time (and, at 1x, the audio device's play position) to a fractional
// timeline frame. Pure logic: callers pass the time in, so it is unit-tested without devices.
#pragma once

#include "core/Rational.h"

#include <cstdint>

namespace kk {

/// The playback position source.
///
/// * **Free-running** (muted, no audio device): position = start + rate * elapsed * fps. The
///   caller measures elapsed time with a QElapsedTimer.
/// * **Audio master** (any rate with sound; shuttle, reverse and slow play are varispeed, see
///   AudioFeeder): the audio device reports how many sample frames were *heard*
///   (latency-compensated, see AudioOutput::playedFrames()); position = start + rate *
///   played / 48000 * fps. Device positions arrive in steps (one audio period, 5-40 ms) and with
///   jitter, which would make video frames hop. So the clock runs on the wall clock and is
///   *slewed* towards the audio position: errors below kSnapFrames are corrected by
///   a fraction per device report (a first-order PLL tracking the reports' upper envelope), larger
///   ones (device restart, underrun) snap. The result is smooth, follows the device's real rate
///   (sound card crystals drift ~0.01 %), and stays within a fraction of a frame of what is heard.
///
/// In both modes the reported position never goes backwards (forwards play) / forwards
/// (reverse play), which keeps the presented frame sequence monotonic.
///
/// Thread-safety: none (GUI thread object).
class PlaybackClock
{
public:
    /// Audio sample rate the played-frame counts refer to (AudioMixer output rate).
    static constexpr int kSampleRate = 48000;
    /// Fraction of the audio/wall-clock error removed per new device report in audio mode,
    /// when the device is ahead of / behind the clock (asymmetric: reports are lower bounds).
    static constexpr double kSlewGainAhead = 0.3;
    static constexpr double kSlewGainBehind = 0.03;
    /// Errors beyond this many timeline frames are not slewed but corrected at once.
    static constexpr double kSnapFrames = 3.0;

    /// Timeline frame rate used to convert seconds to frames.
    void setFps(Rational fps) { m_fps = fps.isValid() ? fps.toDouble() : 25.0; }

    /// Starts a free-running clock at @p startFrame with speed @p rate (frames of timeline time
    /// per frame of real time; negative = reverse) at wall time @p nowUs.
    void startFree(double startFrame, double rate, int64_t nowUs);
    /// Starts an audio-slaved clock at @p startFrame (the frame whose first sample is the
    /// first sample sent to the device) running at @p rate (each device frame carries @p rate
    /// timeline samples). Until the device reports played samples the position holds at
    /// @p startFrame (device start-up latency).
    void startAudio(double startFrame, int64_t nowUs, double rate = 1.0);
    /// Freezes the clock at its current position (pause).
    void stop() { m_running = false; }

    /// Current fractional frame. @p audioPlayedFrames: sample frames heard since startAudio()
    /// (only used in audio mode; < 0 = unknown). @p audioSampleTimeUs: wall time at which that
    /// count was valid (lets the clock extrapolate between device updates).
    double position(int64_t nowUs, int64_t audioPlayedFrames = -1, int64_t audioSampleTimeUs = -1);

    /// True if running.
    [[nodiscard]] bool isRunning() const { return m_running; }
    /// True if slaved to the audio device.
    [[nodiscard]] bool isAudioMaster() const { return m_audio; }
    /// Speed (timeline frames per real-time frame).
    [[nodiscard]] double rate() const { return m_rate; }
    /// Last position returned by position() (or the start / pause position).
    [[nodiscard]] double lastPosition() const { return m_last; }
    /// Last measured audio - clock error in frames (diagnostics, A/V offset tests).
    [[nodiscard]] double lastAudioError() const { return m_lastError; }

private:
    double m_fps = 25.0;
    double m_rate = 0.0;
    bool m_running = false;
    bool m_audio = false;
    bool m_audioStarted = false; ///< device reported its first played samples
    double m_startFrame = 0.0;
    double m_baseFrame = 0.0; ///< position at wall time m_baseUs (slewed in audio mode)
    int64_t m_baseUs = 0;
    int64_t m_lastReportUs = -2; ///< stamp of the last device report used for slewing
    double m_last = 0.0;
    double m_lastError = 0.0;
};

} // namespace kk
