// SPDX-License-Identifier: GPL-3.0-or-later
// AudioMixer — mixes the audio tracks of a timeline into 48 kHz interleaved stereo float.
#pragma once

#include "core/TimelineData.h"
#include "render/MixLevels.h"
#include "render/RenderTypes.h"

#include <QElapsedTimer>
#include <QString>

#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace kk::media {
class AudioDecoder;
}
namespace kk::dsp {
class ChannelProcessor;
}

namespace kk {

/// Mixes a TimelineData's audio tracks (PLAN §5.4):
///
/// * per clip a media::AudioDecoder (pooled by clip id, so contiguous playback never seeks;
///   closed after `idleCloseMs` without use);
/// * source position from clip start / sourceInUs and the retime curve or constant speed
///   (core/Retime.h). Retimed clips with ClipData::pitchCorrection are time stretched
///   (render/TimeStretch.h, WSOLA: tempo follows the curve, pitch stays); without it they are
///   resampled along the curve (pitch follows the speed). Freeze and reverse parts (speed
///   below 2 %) are muted;
/// * clip audio effects (ClipData::effects of an audio clip, render/AudioClipEffects.h): the
///   clip's source samples run through the enabled effects in list order, before volume, pan,
///   fades and transitions, so everything after sees the processed sound;
/// * volume in dB (ClipData::audio keys::VolumeDb, keyframed; interpolated between frames
///   and ramped linearly over kBlockFrames-sample blocks, so automation never clicks);
/// * constant-power pan: θ = (pan + 1)·π/4, gains √2·cos θ / √2·sin θ (unity at centre,
///   +3 dB on the remaining side at the extremes; applied as a balance to stereo sources);
/// * track mute/solo (any solo → only solo tracks play);
/// * transitions: an audio clip's own transitionIn/Out, or — for clips linked to a video clip
///   (same non-zero linkGroup) — the video clip's transition, centred on the cut, using source
///   handles; fades from/to silence at track ends. The gain curve follows the audio
///   transition type (core/AudioTransitions.h: 0 dB linear, -3 dB, +3 dB equal power,
///   exponential, logarithmic, S-curve); a video transition (the linked clip's, or a video id
///   stored on an audio clip by older projects) is the +3 dB equal-power crossfade (sin/cos);
/// * Nested clips (compound clips, nested timelines): the nested timeline's own mix (all its
///   audio tracks, recursively, before the master clipper) is the clip's source, read through
///   a child mixer per clip so contiguous playback never re-opens anything;
/// * fade handles (ClipData::fadeInFrames / fadeOutFrames): equal-power gain ramps
///   (audioFadeGain() in core/ClipAttributes.h), times the volume and transitions;
/// * Clip Attributes channel routing (ClipData::attributes, applied by the decoder:
///   media::AudioDecoder::setChannelMapping());
/// * the mixer graph (Audio page, core/MixerData.h; render/AudioMixerGraph.cpp): every
///   audio track's clips are summed into a track buffer, which runs through the track's
///   channel strip in this order: channel format (mono = (L + R) / 2) -> 6-band EQ ->
///   compressor -> limiter -> pan (the constant-power law below, from the track's pan
///   automation or static pan) -> fader (automation or static) -> mute / solo; then it is
///   added to every bus the track is routed to. Each bus: sum -> channel format -> EQ ->
///   compressor -> limiter -> fader -> mute. The output bus (MixerData::outputBus, default
///   Bus 1) is what mix() returns, so playback and export hear the same thing. Gains glide
///   (~5 ms) and EQ parameters glide (~10 ms), so nothing clicks or zippers. A mix call that
///   does not continue the previous one (seek, loop jump, new export) clears the filter and
///   envelope memory first, so a range always mixes the same way from the same start;
/// * a soft clipper on the master (transparent below −1 dBFS, asymptotic to ±1).
///
/// Performance: allocation-free in steady state (scratch buffers grow only; decoders and
/// channel processors are only created when a clip starts playing or a track / bus appears;
/// tracks without sound and without a filter tail skip their processing). Opening a decoder costs a
/// few ms, so call mix() from a thread that buffers ahead of the audio device, not from a hard
/// real-time callback.
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

    /// Meter readings: when set, every mix() of this (master) mixer clears @p levels and
    /// fills it with the post-fader peaks of every audio track and bus and of the output
    /// (not owned; nullptr = no metering).
    void setLevels(MixLevels* levels) { m_levels = levels; }

    /// Number of open decoders (tests, stats).
    [[nodiscard]] int openDecoders() const { return int(m_decoders.size()); }

    // ---- Maths (public for tests) ----

    /// dB → linear gain; ≤ −96 dB is silence (0).
    [[nodiscard]] static float dbToGain(double db);
    /// Constant-power pan gains for @p pan in [−1, 1] (clamped).
    static void panGains(double pan, float& left, float& right);
    /// Soft clipper: identity for |x| ≤ kClipKnee, smooth tanh knee above, never beyond ±1.
    [[nodiscard]] static float softClip(float x);
    /// Linear level (−1 dBFS) above which the soft clipper starts to act.
    static constexpr float kClipKnee = 0.891251f;

private:
    struct Decoder;
    struct Fade;

    struct Channel;

    Decoder* decoderFor(const ClipData& clip, const NestedTimelines& nested);
    /// Adds the mix of @p tl's output bus over [start, start + frames) to @p out (no
    /// clipping): tracks through their strips into the buses, buses through theirs
    /// (AudioMixerGraph.cpp).
    void mixTimeline(const TimelineData& tl, const NestedTimelines& nested, int64_t startSample48k,
        int frames, float* out);
    /// Adds the clips of @p track over [start, start + frames) to @p out (clip volume, pan,
    /// fades and transitions; no track processing). Returns whether any clip played.
    bool mixTrackClips(const TimelineData& tl, const NestedTimelines& nested,
        const TrackData& track, int64_t startSample48k, int frames, float* out);
    /// The processor state of track @p id / bus @p id (created on first use).
    Channel& trackChannel(quint64 id);
    Channel& busChannel(int id);
    /// Clears every channel's filter and envelope memory (a non-contiguous mix).
    void resetChannels();
    /// Reads @p count source samples of clip decoder @p d from source sample @p first.
    void readSource(
        Decoder& d, const NestedTimelines& nested, int64_t first, float* out, int count);
    /// Retimed clip: the source for timeline samples [from, from + n) (time stretched when
    /// the clip has pitch correction, else resampled along the retime curve).
    const float* retimedSource(Decoder& d, const NestedTimelines& nested, const ClipData& clip,
        Rational fps, int64_t clipStart, int64_t sourceIn, int64_t from, int n);
    /// Pitch-shifted source (ratio = frequency factor, same length) for timeline samples
    /// [from, from + n) of a clip that is not retimed.
    const float* pitchedSource(Decoder& d, const NestedTimelines& nested, double ratio,
        int64_t clipStart, int64_t sourceIn, int64_t from, int n);
    /// Adds one clip over timeline samples [from, to) to @p out; false if it has no source.
    bool mixClip(const TimelineData& tl, const NestedTimelines& nested, const ClipData& clip,
        const Fade& head, const Fade& tail, int64_t from, int64_t to, float* out);
    void collectIdle();

    std::shared_ptr<const MediaPathMap> m_media;
    int m_depth = 0; ///< nesting depth of this mixer (0 = the master mixer)
    std::unordered_map<quint64, std::unique_ptr<Decoder>> m_decoders; ///< by clip id
    std::vector<float> m_source; ///< decoded source samples (scratch)
    std::vector<float> m_resampled; ///< speed-adjusted samples (scratch)
    std::vector<float> m_pitchZ; ///< a pitched clip's time-stretched source (scratch)
    std::vector<float> m_effectBuffer; ///< a clip's samples after its audio effects (scratch)
    std::vector<double> m_positions; ///< retime curve source positions per block (scratch)
    QElapsedTimer m_clock;
    qint64 m_idleCloseMs = 10000;
    // ---- Mixer graph (AudioMixerGraph.cpp) ----
    std::unordered_map<quint64, std::unique_ptr<Channel>> m_trackChannels; ///< by track id
    std::unordered_map<int, std::unique_ptr<Channel>> m_busChannels; ///< by bus id
    std::vector<float> m_trackBuffer; ///< one track's signal (scratch)
    std::vector<std::vector<float>> m_busBuffers; ///< per MixerData::buses index (scratch)
    int64_t m_nextSample = std::numeric_limits<int64_t>::min(); ///< contiguity check
    MixLevels* m_levels = nullptr;
};

} // namespace kk
