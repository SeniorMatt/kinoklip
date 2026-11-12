// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/FrameRateConvert.h"

#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/ParamKeys.h"

#include <QSet>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

/// Rounds num / den (den > 0) to the nearest integer, halves away from zero.
int64_t roundDiv(__int128 num, __int128 den)
{
    const bool negative = num < 0;
    const __int128 a = negative ? -num : num;
    const __int128 q = (2 * a + den) / (2 * den);
    return int64_t(negative ? -q : q);
}

/// The conversion of one timeline: a frame ratio and what to do with the structures.
struct Converter
{
    Rational from;
    Rational to;
    __int128 rn = 1; ///< ratio to / from as a fraction rn / rd
    __int128 rd = 1;
    double ratio = 1.0;

    Converter(Rational f, Rational t)
        : from(f)
        , to(t)
    {
        rn = __int128(to.num) * from.den;
        rd = __int128(to.den) * from.num;
        ratio = double(rn) / double(rd);
    }

    int64_t map(int64_t frame) const { return roundDiv(__int128(frame) * rn, rd); }
    /// A length that exists (>= 1 frame) stays at least one frame.
    int64_t length(int64_t frames) const
    {
        return frames <= 0 ? frames : std::max<int64_t>(1, map(frames));
    }

    /// Converts the keys of @p v. @p scaleValues: the values are frames too (retime curves).
    /// @p pinFrom / @p pinTo: a key at old frame pinFrom goes to pinTo (clip end of a retime).
    void animated(
        AnimatedValue& v, bool scaleValues = false, int64_t pinFrom = -1, int64_t pinTo = -1) const
    {
        if (scaleValues)
            v.base *= ratio;
        QVector<Keyframe> out;
        out.reserve(v.keys.size());
        double lastError = 0.0;
        for (Keyframe k : std::as_const(v.keys)) {
            const int64_t oldFrame = k.frame;
            k.frame = (pinFrom >= 0 && oldFrame == pinFrom) ? pinTo : map(oldFrame);
            const double error = std::abs(double(oldFrame) * ratio - double(k.frame));
            for (KeyHandle* h : {&k.in, &k.out}) {
                h->dtFrames *= ratio;
                if (scaleValues)
                    h->dv *= ratio;
            }
            if (scaleValues)
                k.value *= ratio;
            if (!out.isEmpty() && out.last().frame == k.frame) {
                if (error < lastError) {
                    out.last() = k;
                    lastError = error;
                }
                continue;
            }
            out.append(k);
            lastError = error;
        }
        v.keys = std::move(out);
    }

    void params(ParamSet& set) const
    {
        for (auto it = set.values.begin(); it != set.values.end(); ++it)
            animated(it.value());
    }

    void path(AnimatedPath& p) const
    {
        animated(p.timing);
        p.normalize(); // drops the shapes of keys that merged
    }

    /// Settings counted in frames (unit "fr": which neighbouring frames an effect reads).
    void frameSettings(const QString& descriptorId, ParamSet& set) const
    {
        const EffectDescriptor* d = EffectRegistry::instance().find(descriptorId);
        if (!d)
            return;
        for (const ParamDescriptor& p : d->params) {
            if (p.unit != QLatin1String("fr") || p.type != ParamType::Int)
                continue;
            auto scaled = [&](double v) {
                return std::clamp(double(map(int64_t(std::llround(v)))), p.min, p.max);
            };
            if (set.statics.contains(p.key))
                set.statics[p.key] = int(scaled(set.statics.value(p.key).toDouble()));
            else if (set.values.contains(p.key))
                set.values[p.key].base = scaled(set.values.value(p.key).base);
        }
    }

    void marker(Marker& m, bool singleStaysSingle = true) const
    {
        const int64_t end = m.end();
        m.frame = map(m.frame);
        if (m.duration > 1 || !singleStaysSingle)
            m.duration = std::max<int64_t>(1, map(end) - m.frame);
    }

    void markers(QVector<Marker>& list) const
    {
        for (Marker& m : list)
            marker(m);
        // One marker per frame: a marker landing on a taken frame moves to the next free one.
        for (qsizetype i = 1; i < list.size(); ++i) {
            if (list[i].frame <= list[i - 1].frame)
                list[i].frame = list[i - 1].frame + 1;
        }
    }

    void grade(ClipGrade& g) const
    {
        for (GradeNode& n : g.nodes) {
            params(n.params);
            for (GradeWindow& w : n.windows)
                params(w.params);
        }
    }

    void composition(Composition& c) const
    {
        for (CompNode& n : c.nodes) {
            params(n.params);
            path(n.path);
            if (n.type.startsWith(QLatin1String("fx:")))
                frameSettings(n.type.mid(3), n.params);
        }
    }

    void clipInternals(ClipData& c, int64_t oldDuration, int64_t newDuration) const
    {
        params(c.transform);
        params(c.audio);
        params(c.title);
        for (TitleLayer& layer : c.titleLayers)
            params(layer.params);
        for (EffectInstance& fx : c.effects) {
            params(fx.params);
            frameSettings(fx.type, fx.params);
        }
        for (MaskData& m : c.masks) {
            params(m.params);
            path(m.path);
        }
        if (c.transitionIn) {
            c.transitionIn->duration = length(c.transitionIn->duration);
            params(c.transitionIn->params);
        }
        if (c.transitionOut) {
            c.transitionOut->duration = length(c.transitionOut->duration);
            params(c.transitionOut->params);
        }
        markers(c.markers);
        // Retime curve: keys are clip-local frames, values source frames at the timeline rate;
        // the key at the clip end stays at the clip end.
        if (auto it = c.retime.values.find(keys::RetimeFrame); it != c.retime.values.end())
            animated(it.value(), true, oldDuration, newDuration);
        for (auto it = c.retime.values.begin(); it != c.retime.values.end(); ++it) {
            if (it.key() != keys::RetimeFrame)
                animated(it.value());
        }
        c.fadeInFrames = length(c.fadeInFrames);
        c.fadeOutFrames = length(c.fadeOutFrames);
        if (c.fadeInFrames + c.fadeOutFrames > newDuration)
            c.fadeOutFrames = std::max<int64_t>(0, newDuration - c.fadeInFrames);
        if (c.composition)
            composition(*c.composition);
        grade(c.grade);
        if (c.kind == ClipKind::Generator && c.title.statics.contains(generators::StartFrame)) {
            c.title.statics[generators::StartFrame] = int(convertTimecode(
                c.title.statics.value(generators::StartFrame).toLongLong(), from, to));
        }
    }
};

} // namespace

int64_t convertFrames(int64_t frames, Rational from, Rational to)
{
    if (!from.isValid() || !to.isValid())
        return frames;
    return Converter(from, to).map(frames);
}

void convertTimelineFrameRate(TimelineData& data, Rational newFps)
{
    if (!newFps.isValid() || !data.fps.isValid() || newFps == data.fps)
        return;
    const Converter conv(data.fps, newFps);

    for (TrackType type : {TrackType::Video, TrackType::Audio, TrackType::Subtitle}) {
        for (TrackData& track : data.tracks(type)) {
            int64_t previousEnd = 0;
            bool first = true;
            for (ClipData& clip : track.clips) {
                const int64_t oldDuration = clip.duration;
                int64_t start = conv.map(clip.start);
                int64_t end = conv.map(clip.end());
                if (!first && start < previousEnd)
                    start = previousEnd;
                if (end <= start)
                    end = start + 1;
                clip.start = start;
                clip.duration = end - start;
                conv.clipInternals(clip, oldDuration, clip.duration);
                previousEnd = end;
                first = false;
            }
            conv.params(track.subtitleStyle);
            conv.params(track.mix.automationKeys); // timeline frames
        }
    }

    conv.grade(data.grade);
    conv.markers(data.markers);
    if (data.markIn >= 0)
        data.markIn = conv.map(data.markIn);
    if (data.markOut >= 0) {
        data.markOut = conv.map(data.markOut);
        if (data.markIn >= 0 && data.markOut <= data.markIn)
            data.markOut = data.markIn + 1;
    }
    data.startTimecode = convertTimecode(data.startTimecode, data.fps, newFps);
    data.fps = newFps;
}

} // namespace kk
