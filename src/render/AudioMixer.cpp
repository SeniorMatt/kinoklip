// SPDX-License-Identifier: GPL-3.0-or-later
// AudioMixer — per-clip decode, gain/pan automation, crossfades, mute/solo, soft clip.
#include "render/AudioMixer.h"

#include "core/ParamKeys.h"
#include "media/AudioDecoder.h"
#include "render/RenderLog.h"
#include "render/TimelineEval.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk {

namespace {

/// Gain at or below this level is treated as silence.
constexpr double kSilenceDb = -96.0;
/// Upper bound of half a transition's length: how far a clip can play into its handles.
constexpr double kMaxHandleSeconds = 60.0;
/// Don't retry opening a failed file for this long.
constexpr qint64 kRetryFailedMs = 5000;

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
    QString path;
    qint64 lastUsedMs = 0;
    qint64 failedAtMs = -1;
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

AudioMixer::Decoder* AudioMixer::decoderFor(const ClipData& clip)
{
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
            if (!clip.enabled || clip.kind != ClipKind::Media || clip.end() < windowFirst
                || clip.start > windowLast)
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
            mixClip(tl, clip, headFade, tailFade, s0, s1, out + (s0 - startSample48k) * kChannels);
        }
    }

    for (int i = 0; i < frames * kChannels; ++i)
        out[i] = softClip(out[i]);
    collectIdle();
}

void AudioMixer::mixClip(const TimelineData& tl, const ClipData& clip, const Fade& head,
    const Fade& tail, int64_t from, int64_t to, float* out)
{
    Decoder* d = decoderFor(clip);
    if (!d)
        return;
    const int n = int(to - from);
    const int64_t clipStart = framesToSamples48k(clip.start, tl.fps);
    const int64_t sourceIn = std::llround(double(clip.sourceInUs) * kSampleRate / 1e6);
    const double speed = clip.speed > 0 ? clip.speed : 1.0;

    // ---- Source samples for timeline samples [from, to) ----
    const float* src = nullptr;
    if (speed == 1.0) {
        if (m_source.size() < size_t(n) * kChannels)
            m_source.resize(size_t(n) * kChannels);
        d->decoder->read(sourceIn + (from - clipStart), m_source.data(), n);
        src = m_source.data();
    } else {
        // Linear interpolation between the source samples around each output position.
        const double pos0 = double(sourceIn) + double(from - clipStart) * speed;
        const int64_t first = int64_t(std::floor(pos0));
        const int count = int(std::ceil((n - 1) * speed + (pos0 - first))) + 2;
        if (m_source.size() < size_t(count) * kChannels)
            m_source.resize(size_t(count) * kChannels);
        if (m_resampled.size() < size_t(n) * kChannels)
            m_resampled.resize(size_t(n) * kChannels);
        d->decoder->read(first, m_source.data(), count);
        for (int i = 0; i < n; ++i) {
            const double p = pos0 - double(first) + i * speed;
            const int k = std::min(int(p), count - 2);
            const float t = float(p - k);
            for (int c = 0; c < kChannels; ++c) {
                const float a = m_source[size_t(k) * kChannels + size_t(c)];
                const float b = m_source[size_t(k + 1) * kChannels + size_t(c)];
                m_resampled[size_t(i) * kChannels + size_t(c)] = a + (b - a) * t;
            }
        }
        src = m_resampled.data();
    }

    // ---- Gain envelope: volume automation × transition fades, evaluated at block edges ----
    const double framesPerSample = double(tl.fps.num) / (double(kSampleRate) * tl.fps.den);
    const AnimatedValue* volume = nullptr;
    if (const auto it = clip.audio.values.constFind(keys::VolumeDb);
        it != clip.audio.values.constEnd())
        volume = &*it;
    auto gainAt = [&](int64_t sample) {
        double db = 0.0;
        if (volume) {
            // Keyframes are per frame; interpolate between the two frames around the sample.
            const double lf = double(sample - clipStart) * framesPerSample;
            const double f0 = std::floor(lf);
            const double v0 = volume->valueAt(int64_t(f0));
            const double v1 = volume->isAnimated() ? volume->valueAt(int64_t(f0) + 1) : v0;
            db = v0 + (v1 - v0) * (lf - f0);
        }
        double g = dbToGain(db);
        constexpr double kHalfPi = std::numbers::pi / 2.0;
        if (head.active() && sample >= head.start && sample < head.end)
            g *= std::sin(double(sample - head.start) / double(head.end - head.start) * kHalfPi);
        if (tail.active() && sample >= tail.start && sample < tail.end)
            g *= std::cos(double(sample - tail.start) / double(tail.end - tail.start) * kHalfPi);
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
