// SPDX-License-Identifier: GPL-3.0-or-later
// AudioMixer — per-clip decode, gain/pan automation, crossfades, mute/solo, soft clip.
#include "render/AudioMixer.h"

#include "core/AudioTransitions.h"
#include "core/ParamKeys.h"
#include "core/Retime.h"
#include "media/AudioDecoder.h"
#include "render/AudioClipEffects.h"
#include "render/Equalizer.h"
#include "render/MixerChannel.h"
#include "render/RenderLog.h"
#include "render/TimeStretch.h"
#include "render/TimelineEval.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace kk {

namespace {

/// Gain at or below this level is treated as silence.
constexpr double kSilenceDb = -96.0;
/// Upper bound of half a transition's length: how far a clip can play into its handles.
constexpr double kMaxHandleSeconds = 60.0;
/// Don't retry opening a failed file for this long.
constexpr qint64 kRetryFailedMs = 5000;
/// Deepest nesting mixed (guards against damaged data only; see compound::canNest()).
constexpr int kMaxNestingDepth = 16;
/// Retimed audio below this speed (freeze, reverse, near-stops of a ramp) is muted, as in
/// Resolve: there is no meaningful sound to play.
constexpr double kMinAudibleSpeed = 0.02;
/// Resampled (no pitch correction) reads of more source frames than this per call are muted
/// (beyond ~1000 % the material is noise anyway; bounds the scratch buffer).
constexpr int64_t kMaxVarispeedRead = int64_t(1) << 20;
/// Samples between two evaluations of the retime curve (positions are interpolated linearly
/// in between; the curve is smooth at this scale).
constexpr int kCurveStep = 64;

/// The clip linked to @p clip on a video track (same non-zero linkGroup, overlapping).
/// Gain curve of @p span: the audio transition's own, equal power for a video transition
/// (a linked video clip's, or a video type id stored on an audio clip by older projects).
audiotransitions::Curve curveOf(const render::TransitionSpan& span)
{
    return span.transition ? audiotransitions::curveFor(span.transition->type)
                           : audiotransitions::Curve::EqualPower;
}

const ClipData* linkedVideo(
    const TimelineData& tl, const ClipData& clip, const TrackData** track, int* index)
{
    if (clip.linkGroup == 0)
        return nullptr;
    for (const TrackData& t : tl.videoTracks) {
        for (int i = 0; i < int(t.clips.size()); ++i) {
            const ClipData& c = t.clips[size_t(i)];
            if (c.linkGroup == clip.linkGroup && c.start < clip.end() && clip.start < c.end()) {
                *track = &t;
                *index = i;
                return &c;
            }
        }
    }
    return nullptr;
}

} // namespace

struct AudioMixer::Decoder
{
    std::unique_ptr<media::AudioDecoder> decoder; ///< nullptr after a failed open
    /// Nested clips: the mixer of the nested timeline (owns its own decoders) and the
    /// timeline it mixes (owned by the snapshot passed to mix(), valid during the call).
    std::unique_ptr<AudioMixer> nested;
    const TimelineData* nestedTimeline = nullptr;
    QString path;
    qint64 lastUsedMs = 0;
    qint64 failedAtMs = -1;
    /// Retimed clips: recently read source samples and the time stretch state, valid for the
    /// retime state in `stamp` (start, source in, speed, curve).
    render::SourceCache cache;
    render::TimeStretch stretch;
    ClipData stamp;
    bool stamped = false;
    /// The clip's audio effect stack (ClipData::effects) with its filter / delay memory, and
    /// the timeline sample the next block must start at for that memory to be valid.
    dsp::ClipEffectChain effects;
    int64_t effectsNext = std::numeric_limits<int64_t>::min();
    /// The clip's own equalizer (Inspector > Audio) and its continuation sample.
    dsp::Equalizer eq;
    int64_t eqNext = std::numeric_limits<int64_t>::min();
    /// Pitch shift: the stretch ratio the grain chain was built for (0 = none).
    double pitchRatio = 0.0;
    int64_t pitchSourceIn = 0;
};

/// A gain ramp over [start, end) timeline samples: fade-in (gainIn) or fade-out (gainOut) of
/// the transition's curve (core/AudioTransitions.h; equal power for video transition ids).
struct AudioMixer::Fade
{
    int64_t start = 0;
    int64_t end = 0; ///< start == end: no fade
    audiotransitions::Curve curve = audiotransitions::Curve::EqualPower;
    [[nodiscard]] bool active() const { return end > start; }
};

AudioMixer::AudioMixer()
{
    m_clock.start();
}

AudioMixer::~AudioMixer() = default;

void AudioMixer::setMediaPaths(std::shared_ptr<const MediaPathMap> media)
{
    m_media = std::move(media);
}

void AudioMixer::reset()
{
    m_decoders.clear();
    m_trackChannels.clear();
    m_busChannels.clear();
    m_nextSample = std::numeric_limits<int64_t>::min();
}

float AudioMixer::dbToGain(double db)
{
    return db <= kSilenceDb ? 0.0f : float(std::pow(10.0, db / 20.0));
}

void AudioMixer::panGains(double pan, float& left, float& right)
{
    const double theta = (std::clamp(pan, -1.0, 1.0) + 1.0) * std::numbers::pi / 4.0;
    left = float(std::numbers::sqrt2 * std::cos(theta));
    right = float(std::numbers::sqrt2 * std::sin(theta));
}

float AudioMixer::softClip(float x)
{
    // y = knee + (1 − knee)·tanh((|x| − knee) / (1 − knee)): continuous, slope 1 at the knee,
    // approaches ±1 asymptotically.
    const float a = std::abs(x);
    if (a <= kClipKnee)
        return x;
    const float range = 1.0f - kClipKnee;
    const float y = kClipKnee + range * std::tanh((a - kClipKnee) / range);
    return x < 0 ? -y : y;
}

AudioMixer::Decoder* AudioMixer::decoderFor(const ClipData& clip, const NestedTimelines& nested)
{
    if (clip.kind == ClipKind::Nested) {
        const TimelineData* n = nested.findClip(clip);
        if (!n || m_depth >= kMaxNestingDepth)
            return nullptr;
        auto& slot = m_decoders[clip.id];
        if (!slot)
            slot = std::make_unique<Decoder>();
        if (!slot->nested) {
            slot->nested = std::make_unique<AudioMixer>();
            slot->nested->m_depth = m_depth + 1;
            slot->nested->setIdleCloseMs(m_idleCloseMs);
        }
        slot->nested->m_media = m_media;
        if (slot->nestedTimeline != n) // edited (new snapshot): cached samples are stale
            slot->cache.clear();
        slot->nestedTimeline = n;
        slot->lastUsedMs = m_clock.elapsed();
        return slot.get();
    }
    const QString path = m_media ? m_media->value(clip.mediaId) : QString();
    if (path.isEmpty())
        return nullptr;
    const qint64 now = m_clock.elapsed();
    auto& slot = m_decoders[clip.id];
    if (!slot)
        slot = std::make_unique<Decoder>();
    Decoder& d = *slot;
    d.lastUsedMs = now;
    if (d.path != path) { // new clip, or its media was relinked
        d.decoder.reset();
        d.cache.clear();
        d.failedAtMs = -1;
        d.path = path;
    }
    if (!d.decoder) {
        if (d.failedAtMs >= 0 && now - d.failedAtMs < kRetryFailedMs)
            return nullptr;
        auto decoder = std::make_unique<media::AudioDecoder>();
        QString error;
        if (!decoder->open(path, &error)) {
            if (d.failedAtMs < 0)
                qCWarning(lcRender) << "audio: cannot open" << path << ":" << error;
            d.failedAtMs = now;
            return nullptr;
        }
        d.decoder = std::move(decoder);
    }
    return &d;
}

void AudioMixer::collectIdle()
{
    const qint64 now = m_clock.elapsed();
    for (auto it = m_decoders.begin(); it != m_decoders.end();) {
        if (now - it->second->lastUsedMs > m_idleCloseMs)
            it = m_decoders.erase(it);
        else
            ++it;
    }
    // Channels of deleted tracks / buses (live ones are touched by every mix call).
    std::erase_if(m_trackChannels,
        [&](const auto& entry) { return now - entry.second->lastUsedMs > m_idleCloseMs; });
    std::erase_if(m_busChannels,
        [&](const auto& entry) { return now - entry.second->lastUsedMs > m_idleCloseMs; });
}

void AudioMixer::mix(const TimelineData& tl, int64_t startSample48k, int frames, float* out)
{
    if (frames <= 0)
        return;
    std::fill(out, out + size_t(frames) * kChannels, 0.0f);
    if (m_levels)
        m_levels->clear();
    if (!tl.fps.isValid())
        return;
    mixTimeline(tl, tl.nested, startSample48k, frames, out);
    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < frames; ++i) {
        out[size_t(i) * 2] = softClip(out[size_t(i) * 2]);
        out[size_t(i) * 2 + 1] = softClip(out[size_t(i) * 2 + 1]);
        peakL = std::max(peakL, std::abs(out[size_t(i) * 2]));
        peakR = std::max(peakR, std::abs(out[size_t(i) * 2 + 1]));
    }
    if (m_levels) {
        m_levels->output[0] = peakL;
        m_levels->output[1] = peakR;
    }
    collectIdle();
}

void AudioMixer::readSource(
    Decoder& d, const NestedTimelines& nested, int64_t first, float* out, int count)
{
    if (d.nested) {
        // Nested timeline time 0 = source sample 0. Silence before it (handles of a
        // transition at the very start) and wherever nothing plays inside.
        std::fill(out, out + size_t(count) * kChannels, 0.0f);
        int64_t from = first;
        int n = count;
        if (from < 0) {
            const int skip = int(std::min<int64_t>(-from, count));
            from += skip;
            n -= skip;
            out += size_t(skip) * kChannels;
        }
        if (n > 0 && d.nestedTimeline->fps.isValid())
            d.nested->mixTimeline(*d.nestedTimeline, nested, from, n, out);
        d.nested->collectIdle();
        return;
    }
    d.decoder->read(first, out, count);
}

bool AudioMixer::mixTrackClips(const TimelineData& tl, const NestedTimelines& nested,
    const TrackData& track, int64_t startSample48k, int frames, float* out)
{
    const int64_t mixEnd = startSample48k + frames;
    // Cheap pre-filter in frames: clips farther than kMaxHandleSeconds from the mix window
    // cannot reach it even through a transition handle, so the (linear) linked-clip lookup
    // below only runs for the few clips around the playhead.
    const int64_t margin = int64_t(kMaxHandleSeconds * tl.fps.toDouble()) + 1;
    const int64_t windowFirst
        = usToFrames(startSample48k * 1'000'000 / kSampleRate, tl.fps) - margin;
    const int64_t windowLast = usToFrames(mixEnd * 1'000'000 / kSampleRate, tl.fps) + margin;

    // First clip that can reach the window (clips are sorted and disjoint).
    const auto firstIt = std::lower_bound(track.clips.begin(), track.clips.end(), windowFirst,
        [](const ClipData& c, int64_t f) { return c.end() < f; });
    bool any = false;
    for (int ci = int(firstIt - track.clips.begin()); ci < int(track.clips.size()); ++ci) {
        const ClipData& clip = track.clips[size_t(ci)];
        if (clip.start > windowLast)
            break;
        if (!clip.enabled || (clip.kind != ClipKind::Media && clip.kind != ClipKind::Nested))
            continue;

        // Transitions: the audio track's own, else the linked video clip's.
        std::optional<render::TransitionSpan> head = render::headTransition(track, ci);
        std::optional<render::TransitionSpan> tail = render::tailTransition(track, ci);
        if (!head || !tail) {
            const TrackData* vTrack = nullptr;
            int vIndex = -1;
            if (linkedVideo(tl, clip, &vTrack, &vIndex)) {
                if (!head)
                    head = render::headTransition(*vTrack, vIndex);
                if (!tail)
                    tail = render::tailTransition(*vTrack, vIndex);
            }
        }
        // A centred transition extends the clip into its handles.
        int64_t playStart = clip.start;
        int64_t playEnd = clip.end();
        Fade headFade, tailFade;
        if (head) {
            playStart = std::min(playStart, head->start);
            headFade = {framesToSamples48k(head->start, tl.fps),
                framesToSamples48k(head->end, tl.fps), curveOf(*head)};
        }
        if (tail) {
            playEnd = std::max(playEnd, tail->end);
            tailFade = {framesToSamples48k(tail->start, tl.fps),
                framesToSamples48k(tail->end, tl.fps), curveOf(*tail)};
        }
        const int64_t s0 = std::max(framesToSamples48k(playStart, tl.fps), startSample48k);
        const int64_t s1 = std::min(framesToSamples48k(playEnd, tl.fps), mixEnd);
        if (s1 <= s0)
            continue;
        any |= mixClip(
            tl, nested, clip, headFade, tailFade, s0, s1, out + (s0 - startSample48k) * kChannels);
    }
    return any;
}

const float* AudioMixer::pitchedSource(Decoder& d, const NestedTimelines& nested, double ratio,
    int64_t clipStart, int64_t sourceIn, int64_t from, int n)
{
    // Pitch shift = time stretch by `ratio` (WSOLA keeps the pitch, so the stretched signal Z
    // shows source position u / ratio at its sample u), read back at `ratio` samples per
    // output sample: the length stays, the pitch is multiplied by `ratio`. The stretch is
    // random access, so playback and export give the same samples.
    if (d.pitchRatio != ratio || d.pitchSourceIn != sourceIn || d.stamped) {
        d.stretch.invalidate();
        d.pitchRatio = ratio;
        d.pitchSourceIn = sourceIn;
        d.stamped = false; // the retime path restarts its own chain
    }
    const int64_t t0 = from - clipStart; // clip-local output samples [t0, t0 + n)
    // Above an octave up the read skips samples: a windowed-sinc low-pass at the new Nyquist
    // (kTaps - 1 = 2 * margin taps) keeps what is above it from folding back.
    constexpr int kMargin = 16;
    const int margin = ratio > 2.0 ? kMargin : 0;
    const int64_t i0 = int64_t(std::floor(double(t0) * ratio)) - 1 - margin;
    const int64_t i1 = int64_t(std::floor(double(t0 + n - 1) * ratio)) + 2 + margin;
    const int zn = int(i1 - i0 + 1);
    if (m_pitchZ.size() < size_t(zn) * kChannels)
        m_pitchZ.resize(size_t(zn) * kChannels);
    const render::SourceCache::Reader reader
        = [this, &d, &nested](
              int64_t first, float* dst, int count) { readSource(d, nested, first, dst, count); };
    d.stretch.render(
        i0, zn, [&](int64_t u) { return double(sourceIn) + double(u) / ratio; }, d.cache, reader,
        m_pitchZ.data());
    if (m_resampled.size() < size_t(n) * kChannels)
        m_resampled.resize(size_t(n) * kChannels);
    const float* z = m_pitchZ.data();
    if (margin > 0) {
        double taps[2 * kMargin + 1];
        const double fc = 0.45 / ratio; // cycles per sample of Z
        double sum = 0.0;
        for (int k = -kMargin; k <= kMargin; ++k) {
            const double x = 2.0 * std::numbers::pi * fc * k;
            const double sinc = k == 0 ? 2.0 * fc : std::sin(x) / (std::numbers::pi * k);
            const double w = 0.5 + 0.5 * std::cos(std::numbers::pi * k / (kMargin + 1));
            taps[k + kMargin] = sinc * w;
            sum += taps[k + kMargin];
        }
        for (double& t : taps)
            t /= sum; // unity at DC
        if (m_pitchF.size() < size_t(zn) * kChannels)
            m_pitchF.resize(size_t(zn) * kChannels);
        std::copy(m_pitchZ.begin(), m_pitchZ.begin() + size_t(zn) * kChannels, m_pitchF.begin());
        for (int j = kMargin; j < zn - kMargin; ++j) {
            for (int ch = 0; ch < kChannels; ++ch) {
                double acc = 0.0;
                for (int k = -kMargin; k <= kMargin; ++k)
                    acc += taps[k + kMargin] * double(m_pitchZ[size_t(j + k) * kChannels + ch]);
                m_pitchF[size_t(j) * kChannels + ch] = float(acc);
            }
        }
        z = m_pitchF.data();
    }
    for (int i = 0; i < n; ++i) {
        const double x = double(t0 + i) * ratio;
        const int64_t k = int64_t(std::floor(x));
        const float f = float(x - double(k));
        const size_t base = size_t(k - i0);
        for (int ch = 0; ch < kChannels; ++ch) {
            // Catmull-Rom between the samples k and k + 1.
            const float p0 = z[(base - 1) * kChannels + ch], p1 = z[base * kChannels + ch];
            const float p2 = z[(base + 1) * kChannels + ch], p3 = z[(base + 2) * kChannels + ch];
            m_resampled[size_t(i) * kChannels + ch] = p1
                + 0.5f * f
                    * (p2 - p0
                        + f
                            * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3
                                + f * (3.0f * (p1 - p2) + p3 - p0)));
        }
    }
    return m_resampled.data();
}

const float* AudioMixer::retimedSource(Decoder& d, const NestedTimelines& nested,
    const ClipData& clip, Rational fps, int64_t clipStart, int64_t sourceIn, int64_t from, int n)
{
    if (m_resampled.size() < size_t(n) * kChannels)
        m_resampled.resize(size_t(n) * kChannels);
    float* out = m_resampled.data();
    // A new retime state (an edit) invalidates the grain chain; the cached source stays.
    if (!d.stamped || d.stamp.start != clip.start || d.stamp.sourceInUs != clip.sourceInUs
        || d.stamp.speed != clip.speed || d.stamp.retime != clip.retime) {
        d.stamp.start = clip.start;
        d.stamp.sourceInUs = clip.sourceInUs;
        d.stamp.speed = clip.speed;
        d.stamp.retime = clip.retime;
        d.stamped = true;
        d.stretch.invalidate();
    }
    // Source sample (fractional) at timeline sample s: sourceIn + R-offset in samples.
    const double framesPerSample = double(fps.num) / (double(kSampleRate) * fps.den);
    const double samplesPerFrame = double(kSampleRate) * fps.den / double(fps.num);
    const double r0 = retime::position(clip, 0.0);
    auto sourceAt = [&](int64_t sample) {
        const double x = double(sample - clipStart) * framesPerSample;
        return double(sourceIn) + (retime::position(clip, x) - r0) * samplesPerFrame;
    };
    const render::SourceCache::Reader reader
        = [this, &d, &nested](
              int64_t first, float* dst, int count) { readSource(d, nested, first, dst, count); };

    if (clip.pitchCorrection) {
        // WSOLA: grains on a grid anchored at the clip start, so every render of a range
        // gives the same samples.
        d.stretch.render(
            from - clipStart, n, [&](int64_t outSample) { return sourceAt(clipStart + outSample); },
            d.cache, reader, out);
        return out;
    }

    // Varispeed (pitch follows the speed): source positions at every kCurveStep-th sample,
    // linear in between, then linear interpolation between the two source samples around each.
    const int edges = n / kCurveStep + 2;
    m_positions.resize(size_t(edges));
    double lo = std::numeric_limits<double>::max();
    double hi = std::numeric_limits<double>::lowest();
    for (int e = 0; e < edges; ++e) {
        const double p = sourceAt(from + std::min<int64_t>(int64_t(e) * kCurveStep, n));
        m_positions[size_t(e)] = p;
        lo = std::min(lo, p);
        hi = std::max(hi, p);
    }
    const int64_t first = int64_t(std::floor(lo));
    const int64_t count = int64_t(std::ceil(hi)) - first + 2;
    if (count > kMaxVarispeedRead) {
        std::fill(out, out + size_t(n) * kChannels, 0.0f);
        return out;
    }
    const float* s = d.cache.fetch(first, int(count), reader);
    for (int i = 0; i < n; ++i) {
        const int e = i / kCurveStep;
        const int segStart = e * kCurveStep;
        const int segEnd = std::min(segStart + kCurveStep, n); // > i, so > segStart
        const double t = double(i - segStart) / double(segEnd - segStart);
        const double p0 = m_positions[size_t(e)];
        const double p = p0 + (m_positions[size_t(e) + 1] - p0) * t - double(first);
        const int k = std::clamp(int(p), 0, int(count) - 2);
        const float f = float(p - k);
        for (int c = 0; c < kChannels; ++c) {
            const float a = s[size_t(k) * kChannels + size_t(c)];
            const float b = s[size_t(k + 1) * kChannels + size_t(c)];
            out[size_t(i) * kChannels + size_t(c)] = a + (b - a) * f;
        }
    }
    return out;
}

bool AudioMixer::mixClip(const TimelineData& tl, const NestedTimelines& nested,
    const ClipData& clip, const Fade& head, const Fade& tail, int64_t from, int64_t to, float* out)
{
    Decoder* d = decoderFor(clip, nested);
    if (!d)
        return false;
    if (d->decoder) {
        // Clip Attributes channel routing (a no-op compare unless it changed).
        if (!(d->decoder->channelMapping().channelFormat == clip.attributes.channelFormat
                && d->decoder->channelMapping().left == clip.attributes.left
                && d->decoder->channelMapping().right == clip.attributes.right))
            d->cache.clear(); // cached source samples used the old routing
        d->decoder->setChannelMapping(clip.attributes);
    }
    const int n = int(to - from);
    const int64_t clipStart = framesToSamples48k(clip.start, tl.fps);
    const int64_t sourceIn = std::llround(double(clip.sourceInUs) * kSampleRate / 1e6);
    const double framesPerSample = double(tl.fps.num) / (double(kSampleRate) * tl.fps.den);
    const bool retimed = retime::isRetimed(clip);

    // ---- Source samples for timeline samples [from, to) ----
    const float* src = nullptr;
    if (!retimed) {
        if (m_source.size() < size_t(n) * kChannels)
            m_source.resize(size_t(n) * kChannels);
        readSource(*d, nested, sourceIn + (from - clipStart), m_source.data(), n);
        src = m_source.data();
    } else {
        src = retimedSource(*d, nested, clip, tl.fps, clipStart, sourceIn, from, n);
        d->pitchRatio = 0.0;
    }

    // ---- Pitch (Inspector > Audio): same length, other pitch; not on retimed clips ----
    if (!retimed) {
        const double semitones = clip.audio.value(keys::PitchSemitones, 0)
            + clip.audio.value(keys::PitchCents, 0) / 100.0;
        if (std::abs(semitones) > 1e-6) {
            src = pitchedSource(
                *d, nested, std::exp2(semitones / 12.0), clipStart, sourceIn, from, n);
        } else {
            d->pitchRatio = 0.0;
        }
    }

    // ---- Clip equalizer ----
    if (clip.audio.staticValue(keys::EqEnabled, false).toBool()) {
        EqSettings s;
        s.enabled = true;
        for (EqBand& b : s.bands)
            b.enabled = false;
        static const double defaultHz[4] = {100, 500, 2000, 8000};
        static const double defaultQ[4] = {0.7071, 1, 1, 0.7071};
        static const EqBandType types[4]
            = {EqBandType::LowShelf, EqBandType::Bell, EqBandType::Bell, EqBandType::HighShelf};
        for (int b = 0; b < 4; ++b) {
            s.bands[size_t(b + 1)] = {types[b], true,
                std::clamp(clip.audio.value(keys::EqBandFreq[b], 0, defaultHz[b]),
                    kEqMinFrequencyHz, kEqMaxFrequencyHz),
                std::clamp(
                    clip.audio.value(keys::EqBandGain[b], 0, 0.0), -kEqMaxGainDb, kEqMaxGainDb),
                std::clamp(clip.audio.value(keys::EqBandQ[b], 0, defaultQ[b]), kEqMinQ, kEqMaxQ)};
        }
        d->eq.setTarget(s);
        if (from != d->eqNext)
            d->eq.reset(); // after the target: a new start jumps to the settings
        d->eqNext = to;
        if (m_effectBuffer.size() < size_t(n) * kChannels)
            m_effectBuffer.resize(size_t(n) * kChannels);
        std::copy(src, src + size_t(n) * kChannels, m_effectBuffer.begin());
        d->eq.process(m_effectBuffer.data(), n);
        src = m_effectBuffer.data();
    } else {
        d->eqNext = std::numeric_limits<int64_t>::min();
    }

    // ---- Clip audio effects: on the clip's own source, before volume / pan / fades ----
    // (the clip stage; the track's EQ, dynamics, pan and fader come after, in the track
    // channel strip). The effects keep memory between blocks, so a block that does not
    // continue the previous one (seek, loop, a new export) starts them clean.
    if (dsp::ClipEffectChain::hasActive(clip.effects)) {
        if (from != d->effectsNext)
            d->effects.reset();
        d->effectsNext = to;
        if (m_effectBuffer.size() < size_t(n) * kChannels)
            m_effectBuffer.resize(size_t(n) * kChannels);
        std::copy(src, src + size_t(n) * kChannels, m_effectBuffer.begin());
        d->effects.process(clip.effects, m_effectBuffer.data(), n);
        src = m_effectBuffer.data();
    } else {
        d->effectsNext = std::numeric_limits<int64_t>::min();
    }

    // ---- Gain envelope: volume automation × transition fades, evaluated at block edges ----
    const AnimatedValue* volume = nullptr;
    if (const auto it = clip.audio.values.constFind(keys::VolumeDb);
        it != clip.audio.values.constEnd())
        volume = &*it;
    auto gainAt = [&](int64_t sample) {
        double db = 0.0;
        if (volume) {
            // The keyframe curve evaluated at the sample's exact (fractional) clip-local
            // frame: Bezier eases stay smooth between frames and a static (hold) key steps
            // exactly at the next key, as on the video side.
            const double lf = double(sample - clipStart) * framesPerSample;
            db = volume->valueAtTime(lf);
        }
        double g = dbToGain(db);
        if (clip.fadeInFrames > 0 || clip.fadeOutFrames > 0) // fade handles (spec 31)
            g *= audioFadeGain(double(sample - clipStart) * framesPerSample, clip.duration,
                clip.fadeInFrames, clip.fadeOutFrames, clip.fadeInCurve, clip.fadeOutCurve);
        if (head.active() && sample >= head.start && sample < head.end)
            g *= audiotransitions::gainIn(
                head.curve, double(sample - head.start) / double(head.end - head.start));
        if (tail.active() && sample >= tail.start && sample < tail.end)
            g *= audiotransitions::gainOut(
                tail.curve, double(sample - tail.start) / double(tail.end - tail.start));
        // Freeze / reverse: silent (ramped over one block like any gain change).
        if (retimed
            && retime::speedAt(clip, double(sample - clipStart) * framesPerSample)
                < kMinAudibleSpeed)
            g = 0.0;
        return float(g);
    };

    for (int b0 = 0; b0 < n; b0 += kBlockFrames) {
        const int b1 = std::min(b0 + kBlockFrames, n);
        const float g0 = gainAt(from + b0);
        const float g1 = gainAt(from + b1);
        if (g0 == 0.0f && g1 == 0.0f)
            continue;
        float panL = 1.0f, panR = 1.0f;
        const int64_t mid = from + (b0 + b1) / 2;
        panGains(clip.audio.value(keys::Pan, int64_t(double(mid - clipStart) * framesPerSample)),
            panL, panR);
        const float step = (g1 - g0) / float(b1 - b0);
        float g = g0;
        for (int i = b0; i < b1; ++i, g += step) {
            out[size_t(i) * 2] += src[size_t(i) * 2] * g * panL;
            out[size_t(i) * 2 + 1] += src[size_t(i) * 2 + 1] * g * panR;
        }
    }
    return true;
}

} // namespace kk
