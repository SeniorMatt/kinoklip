// SPDX-License-Identifier: GPL-3.0-or-later
// AudioMixer — mixes the audio tracks of a timeline into 48 kHz interleaved stereo float.
#pragma once

#include "core/TimelineData.h"
#include "render/RenderTypes.h"

#include <QElapsedTimer>
#include <QString>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace kk::media {
class AudioDecoder;
}

namespace kk {

/// Mixes a TimelineData's audio tracks (PLAN §5.4):
///
/// * per clip a media::AudioDecoder (pooled by clip id, so contiguous playback never seeks;
///   closed after `idleCloseMs` without use);
/// * source position from clip start / sourceInUs / speed (speed ≠ 1 is resampled linearly,
///   i.e. pitch changes with speed — v0.1 has no time stretching);
/// * volume in dB (ClipData::audio keys::VolumeDb, keyframed; interpolated between frames
///   and ramped linearly over kBlockFrames-sample blocks, so automation never clicks);
/// * constant-power pan: θ = (pan + 1)·π/4, gains √2·cos θ / √2·sin θ (unity at centre,
///   +3 dB on the remaining side at the extremes; applied as a balance to stereo sources);
/// * track mute/solo (any solo → only solo tracks play);
/// * transitions: an audio clip's own transitionIn/Out, or — for clips linked to a video clip
///   (same non-zero linkGroup) — the video clip's transition: equal-power crossfade (sin/cos)
///   centred on the cut, using source handles; fades from/to silence at track ends;
/// * a soft clipper on the master (transparent below −1 dBFS, asymptotic to ±1).
///
/// Performance: allocation-free in steady state (scratch buffers grow only; decoders are only
/// created when a clip starts playing). Opening a decoder costs a few ms, so call mix() from
/// a thread that buffers ahead of the audio device, not from a hard real-time callback.
///
/// Thread-safety: NOT thread-safe; one mixer per consumer thread (playback, export).
class AudioMixer
{
public:
    static constexpr int kSampleRate = 48000; ///< output rate (Hz)
    static constexpr int kChannels = 2; ///< interleaved L, R
    static constexpr int kBlockFrames = 64; ///< gain ramp granularity in samples

    AudioMixer();
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    /// Media id → path table used to open clips (replaces the previous one).
    void setMediaPaths(std::shared_ptr<const MediaPathMap> media);
    /// Decoders unused for this long are closed (default 10 s).
    void setIdleCloseMs(qint64 ms) { m_idleCloseMs = ms; }

    /// Writes @p frames stereo sample frames (2·frames floats) starting at timeline sample
    /// @p startSample48k (= framesToSamples48k(timeline frame)) into @p out. Silence where
    /// nothing plays. Always writes every sample.
    void mix(const TimelineData& timeline, int64_t startSample48k, int frames, float* out);

    /// Closes every decoder (e.g. after a project change).
    void reset();

    /// Number of open decoders (tests, stats).
    [[nodiscard]] int openDecoders() const { return int(m_decoders.size()); }

    // ---- Maths (public for tests) ----

    /// dB → linear gain; ≤ −96 dB is silence (0).
    [[nodiscard]] static float dbToGain(double db);
    /// Constant-power pan gains for @p pan in [−1, 1] (clamped).
    static void panGains(double pan, float& left, float& right);
    /// Soft clipper: identity for |x| ≤ kClipKnee, smooth tanh knee above, |y| < 1.
    [[nodiscard]] static float softClip(float x);
    /// Linear level (−1 dBFS) above which the soft clipper starts to act.
    static constexpr float kClipKnee = 0.891251f;

private:
    struct Decoder;
    struct Fade;

    Decoder* decoderFor(const ClipData& clip);
    void mixClip(const TimelineData& tl, const ClipData& clip, const Fade& head, const Fade& tail,
        int64_t from, int64_t to, float* out);
    void collectIdle();

    std::shared_ptr<const MediaPathMap> m_media;
    std::unordered_map<quint64, std::unique_ptr<Decoder>> m_decoders; ///< by clip id
    std::vector<float> m_source; ///< decoded source samples (scratch)
    std::vector<float> m_resampled; ///< speed-adjusted samples (scratch)
    QElapsedTimer m_clock;
    qint64 m_idleCloseMs = 10000;
};

} // namespace kk
