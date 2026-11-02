// SPDX-License-Identifier: GPL-3.0-or-later
// Retime model: evaluation of the retime curve, segments, media limits (see Retime.h).
#include "core/Retime.h"

#include "core/ParamKeys.h"

#include <algorithm>
#include <cmath>

namespace kk::retime {

namespace {

/// Half width (frames) of the central difference used by speedAt(): small against a frame,
/// large enough to stay clear of double rounding at hour-long positions.
constexpr double kSpeedProbeFrames = 1.0 / 64.0;

double chordSlope(const Keyframe& a, const Keyframe& b)
{
    return (b.value - a.value) / double(b.frame - a.frame);
}

bool isBoundedMedia(const ClipData& clip)
{
    return (clip.kind == ClipKind::Media || clip.kind == ClipKind::Nested)
        && clip.sourceDurationUs > 0;
}

} // namespace

const AnimatedValue* curve(const ClipData& clip)
{
    const auto it = clip.retime.values.constFind(keys::RetimeFrame);
    if (it == clip.retime.values.constEnd() || it->keys.size() < 2)
        return nullptr;
    return &*it;
}

bool isRetimed(const ClipData& clip)
{
    return hasCurve(clip) || clip.speed != 1.0;
}

double position(const ClipData& clip, double localFrame)
{
    const AnimatedValue* c = curve(clip);
    if (!c)
        return localFrame * clip.speed;
    const QVector<Keyframe>& k = c->keys;
    const Keyframe& first = k.front();
    const Keyframe& last = k.back();
    // Outside the keys: straight continuation of the edge segment (AnimatedValue itself would
    // hold the end value, which would freeze a clip that is trimmed longer).
    if (localFrame <= double(first.frame))
        return first.value + (localFrame - double(first.frame)) * chordSlope(first, k[1]);
    if (localFrame >= double(last.frame))
        return last.value + (localFrame - double(last.frame)) * chordSlope(k[k.size() - 2], last);
    return c->valueAtTime(localFrame);
}

double speedAt(const ClipData& clip, double localFrame)
{
    if (!hasCurve(clip))
        return clip.speed;
    return (position(clip, localFrame + kSpeedProbeFrames)
               - position(clip, localFrame - kSpeedProbeFrames))
        / (2.0 * kSpeedProbeFrames);
}

double frameUs(Rational fps)
{
    return fps.isValid() ? 1e6 * fps.den / fps.num : 0.0;
}

int64_t sourceOffsetUs(const ClipData& clip, double fromLocal, double toLocal, Rational fps)
{
    if (!fps.isValid())
        return 0;
    if (!hasCurve(clip)) {
        // Same expression (and rounding) as ClipData::framesToSourceUs().
        return int64_t(std::llround((toLocal - fromLocal) * 1e6 * fps.den / fps.num * clip.speed));
    }
    return int64_t(std::llround(
        (position(clip, toLocal) - position(clip, fromLocal)) * 1e6 * fps.den / fps.num));
}

int64_t sourceTimeUs(const ClipData& clip, double localFrame, Rational fps)
{
    return clip.sourceInUs + sourceOffsetUs(clip, 0.0, localFrame, fps);
}

SourceRange shownSourceRange(const ClipData& clip, int64_t fromLocal, int64_t toLocal, Rational fps)
{
    const int64_t first = sourceTimeUs(clip, double(fromLocal), fps);
    SourceRange r {first, first};
    if (toLocal <= fromLocal + 1)
        return r;
    auto add = [&](int64_t x) {
        const int64_t t = sourceTimeUs(clip, double(x), fps);
        r.minUs = std::min(r.minUs, t);
        r.maxUs = std::max(r.maxUs, t);
    };
    add(toLocal - 1);
    // Between keys the curve is monotone (non-free handles never overshoot), so the extremes
    // sit at the range ends or at keys.
    if (const AnimatedValue* c = curve(clip)) {
        for (const Keyframe& k : c->keys) {
            if (k.frame > fromLocal && k.frame < toLocal - 1)
                add(k.frame);
        }
    }
    return r;
}

int64_t validRun(const ClipData& clip, int64_t firstLocal, int step, Rational fps, int64_t cap)
{
    if (!isBoundedMedia(clip))
        return cap;
    const int64_t lastUs = clip.sourceDurationUs - 1;
    auto ok = [&](int64_t x) {
        const int64_t t = sourceTimeUs(clip, double(x), fps);
        return t >= 0 && t <= lastUs;
    };
    if (!ok(firstLocal))
        return 0;
    auto count = [&](int64_t x) { return std::min(cap, (x - firstLocal) * step + 1); };

    // Walk the monotone pieces between keys in the direction of travel; inside a piece that
    // leaves the media, binary search the last valid frame.
    int64_t cur = firstLocal;
    if (const AnimatedValue* c = curve(clip)) {
        QVector<int64_t> breaks;
        for (const Keyframe& k : c->keys) {
            if ((k.frame - cur) * step > 0)
                breaks.append(k.frame);
        }
        if (step < 0)
            std::reverse(breaks.begin(), breaks.end());
        const int64_t capFrame = firstLocal + (cap - 1) * step;
        for (int64_t key : breaks) {
            // Never look past the cap: a piece reaching it only needs its frame at the cap.
            const bool atCap = count(key) >= cap;
            const int64_t b = atCap ? capFrame : key;
            if (ok(b)) {
                if (atCap)
                    return cap;
                cur = b;
                continue;
            }
            int64_t lo = cur, hi = b; // lo valid, hi not
            while ((hi - lo) * step > 1) {
                const int64_t mid = lo + (hi - lo) / 2;
                (ok(mid) ? lo : hi) = mid;
            }
            return count(lo);
        }
    }
    // Linear from cur on.
    const double slopeUs = (position(clip, double(cur + step)) - position(clip, double(cur)))
        * frameUs(fps); // µs per step, signed
    if (slopeUs == 0.0)
        return cap;
    const int64_t t = sourceTimeUs(clip, double(cur), fps);
    const double room = slopeUs > 0 ? double(lastUs - t) : double(t);
    int64_t n = int64_t(std::floor(room / std::abs(slopeUs)));
    if (count(cur + n * step) >= cap)
        return cap;
    // The closed form can be one off after rounding to µs: settle it exactly.
    while (n > 0 && !ok(cur + n * step))
        --n;
    while (count(cur + (n + 1) * step) < cap && ok(cur + (n + 1) * step))
        ++n;
    return count(cur + n * step);
}

// ---- Segments ----

QVector<Segment> segments(const ClipData& clip)
{
    QVector<Segment> out;
    const int64_t duration = std::max<int64_t>(1, clip.duration);
    const AnimatedValue* c = curve(clip);
    if (!c) {
        out.append(Segment {0, duration, clip.speed, false, false, -1, -1});
        return out;
    }
    const QVector<Keyframe>& keys = c->keys;
    auto keyAt = [&](int64_t frame) {
        const qsizetype i = c->indexOf(frame);
        return int(i);
    };
    int64_t a = 0;
    auto close = [&](int64_t b) {
        Segment s;
        s.startLocal = a;
        s.endLocal = b;
        s.speed = (position(clip, double(b)) - position(clip, double(a))) / double(b - a);
        // Exact zero for freeze segments whose ends have equal values (no rounding noise).
        if (std::abs(s.speed) < 1e-12)
            s.speed = 0.0;
        s.startKey = keyAt(a);
        s.endKey = keyAt(b);
        s.rampIn = s.startKey >= 0 && keys[s.startKey].out.mode != KeyHandle::Mode::Linear;
        s.rampOut = s.endKey >= 0 && keys[s.endKey].in.mode != KeyHandle::Mode::Linear;
        out.append(s);
        a = b;
    };
    for (const Keyframe& k : keys) {
        if (k.frame > 0 && k.frame < duration)
            close(k.frame);
    }
    close(duration);
    return out;
}

int segmentAt(const QVector<Segment>& segs, int64_t localFrame)
{
    if (segs.isEmpty())
        return -1;
    for (int i = 0; i < segs.size(); ++i) {
        if (localFrame < segs[i].endLocal)
            return i;
    }
    return int(segs.size()) - 1;
}

QVector<int64_t> speedPoints(const ClipData& clip)
{
    QVector<int64_t> out;
    if (const AnimatedValue* c = curve(clip)) {
        for (const Keyframe& k : c->keys) {
            if (k.frame > 0 && k.frame < clip.duration)
                out.append(k.frame);
        }
    }
    return out;
}

// ---- Process ----

RetimeProcess effectiveProcess(const ClipData& clip, const TimelineData& timeline)
{
    if (!isRetimed(clip))
        return RetimeProcess::Nearest;
    RetimeProcess p = clip.retimeProcess;
    if (p == RetimeProcess::ProjectDefault)
        p = timeline.retimeProcess;
    return p == RetimeProcess::ProjectDefault ? RetimeProcess::Nearest : p;
}

QString processToString(RetimeProcess process)
{
    switch (process) {
    case RetimeProcess::ProjectDefault:
        return QStringLiteral("default");
    case RetimeProcess::Nearest:
        return QStringLiteral("nearest");
    case RetimeProcess::FrameBlend:
        return QStringLiteral("frameBlend");
    case RetimeProcess::OpticalFlow:
        return QStringLiteral("opticalFlow");
    }
    return QStringLiteral("default");
}

RetimeProcess processFromString(const QString& text, RetimeProcess fallback)
{
    for (RetimeProcess p : {RetimeProcess::ProjectDefault, RetimeProcess::Nearest,
             RetimeProcess::FrameBlend, RetimeProcess::OpticalFlow}) {
        if (text == processToString(p))
            return p;
    }
    return fallback;
}

QString formatPercent(double speed)
{
    const double percent = std::round(speed * 1000.0) / 10.0;
    if (percent == 0.0)
        return QStringLiteral("0%");
    if (percent == std::round(percent))
        return QString::number(qint64(percent)) + QLatin1Char('%');
    return QString::number(percent, 'f', 1) + QLatin1Char('%');
}

} // namespace kk::retime
