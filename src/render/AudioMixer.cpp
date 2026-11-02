// SPDX-License-Identifier: GPL-3.0-or-later
// AudioMixer — per-clip decode, gain/pan automation, crossfades, mute/solo, soft clip.
#include "render/AudioMixer.h"

#include "core/ParamKeys.h"
#include "core/Retime.h"
#include "media/AudioDecoder.h"
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
};

/// A gain ramp over [start, end) timeline samples: fade-in (sin) or fade-out (cos).
struct AudioMixer::Fade
{
    int64_t start = 0;
    int64_t end = 0; ///< start == end: no fade
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
}

void AudioMixer::mix(const TimelineData& tl, int64_t startSample48k, int frames, float* out)
{
    if (frames <= 0)
        return;
    std::fill(out, out + size_t(frames) * kChannels, 0.0f);
    if (!tl.fps.isValid())
        return;
    mixTimeline(tl, tl.nested, startSample48k, frames, out);
    for (int i = 0; i < frames * kChannels; ++i)
        out[i] = softClip(out[i]);
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

void AudioMixer::mixTimeline(const TimelineData& tl, const NestedTimelines& nested,
    int64_t startSample48k, int frames, float* out)
{
    const int64_t mixEnd = startSample48k + frames;
    const bool anySolo = std::any_of(
        tl.audioTracks.begin(), tl.audioTracks.end(), [](const TrackData& t) { return t.solo; });
    // Cheap pre-filter in frames: clips farther than kMaxHandleSeconds from the mix window
    // cannot reach it even through a transition handle, so the (linear) linked-clip lookup
    // below only runs for the few clips around the playhead.
    const int64_t margin = int64_t(kMaxHandleSeconds * tl.fps.toDouble()) + 1;
    const int64_t windowFirst
        = usToFrames(startSample48k * 1'000'000 / kSampleRate, tl.fps) - margin;
    const int64_t windowLast = usToFrames(mixEnd * 1'000'000 / kSampleRate, tl.fps) + margin;

    for (const TrackData& track : tl.audioTracks) {
        if (track.muted || (anySolo && !track.solo))
            continue;
        for (int ci = 0; ci < int(track.clips.size()); ++ci) {
            const ClipData& clip = track.clips[size_t(ci)];
            if (!clip.enabled || (clip.kind != ClipKind::Media && clip.kind != ClipKind::Nested)
                || clip.end() < windowFirst || clip.start > windowLast)
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
                headFade = {
                    framesToSamples48k(head->start, tl.fps), framesToSamples48k(head->end, tl.fps)};
            }
            if (tail) {
                playEnd = std::max(playEnd, tail->end);
                tailFade = {
                    framesToSamples48k(tail->start, tl.fps), framesToSamples48k(tail->end, tl.fps)};
            }
            const int64_t s0 = std::max(framesToSamples48k(playStart, tl.fps), startSample48k);
            const int64_t s1 = std::min(framesToSamples48k(playEnd, tl.fps), mixEnd);
            if (s1 <= s0)
                continue;
            mixClip(tl, nested, clip, headFade, tailFade, s0, s1,
                out + (s0 - startSample48k) * kChannels);
        }
    }
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

void AudioMixer::mixClip(const TimelineData& tl, const NestedTimelines& nested,
    const ClipData& clip, const Fade& head, const Fade& tail, int64_t from, int64_t to, float* out)
{
    Decoder* d = decoderFor(clip, nested);
    if (!d)
        return;
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
        constexpr double kHalfPi = std::numbers::pi / 2.0;
        if (head.active() && sample >= head.start && sample < head.end)
            g *= std::sin(double(sample - head.start) / double(head.end - head.start) * kHalfPi);
        if (tail.active() && sample >= tail.start && sample < tail.end)
            g *= std::cos(double(sample - tail.start) / double(tail.end - tail.start) * kHalfPi);
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
}

} // namespace kk
