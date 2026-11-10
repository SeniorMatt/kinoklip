// SPDX-License-Identifier: GPL-3.0-or-later
// RetimeOps implementation: every edit is a small change of the retime curve's keys (frames =
// timeline length, values = source position) plus a ripple of the clip's track.
#include "core/RetimeOps.h"

#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "core/Retime.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace kk::retimeops {

namespace {

/// Largest speed an edit produces (100000 %): keeps frame counts and µs products finite.
constexpr double kMaxSpeed = 1000.0;
/// Two chord slopes closer than this are "the same speed" (simplifyCurve()).
constexpr double kSameSpeed = 1e-9;

AnimatedValue& curveOf(ClipData& clip)
{
    return clip.retime.animated(keys::RetimeFrame, 0.0);
}

Keyframe linearKey(int64_t frame, double value)
{
    Keyframe k;
    k.frame = frame;
    k.value = value;
    return k; // Linear handles by default
}

bool sameRetime(const ClipData& a, const ClipData& b)
{
    return a.start == b.start && a.duration == b.duration && a.speed == b.speed
        && a.retime == b.retime && a.sourceInUs == b.sourceInUs;
}

/// A clip edit: changes the clip and returns by how many frames the later clips of its track
/// move (its length change when the edit ripples, 0 otherwise; std::nullopt = nothing
/// changed).
using ClipEdit = std::function<std::optional<int64_t>(ClipData&, Rational fps)>;

/// Applies @p edit to the clip and every partner; ripples the later clips of each edited track
/// by the length change. Partners are found before the edit, and the edit must give the same
/// result for each (they have the same retime state).
bool applyToGroup(TimelineData& data, quint64 clipId, const ClipEdit& edit)
{
    const QVector<quint64> ids = partners(data, clipId);
    if (ids.isEmpty())
        return false;
    bool changed = false;
    for (quint64 id : ids) {
        const auto loc = editops::findClip(data, id);
        TrackData& track = *data.track(loc->track);
        if (track.locked)
            continue;
        ClipData& clip = track.clips[size_t(loc->clipIndex)];
        const ClipData before = clip;
        const std::optional<int64_t> delta = edit(clip, data.fps);
        if (!delta) {
            clip = before;
            continue;
        }
        clip.duration = std::max<int64_t>(1, clip.duration);
        editops::clampFades(clip); // a shorter clip must not keep a longer fade (spec 59.13)
        const int64_t applied = *delta;
        for (size_t i = size_t(loc->clipIndex) + 1; i < track.clips.size(); ++i)
            track.clips[i].start += applied;
        changed |= clip != before || applied != 0;
    }
    return changed;
}

/// Index of the curve key at clip-local @p frame, or -1.
int keyIndex(const ClipData& clip, int64_t frame)
{
    const auto it = clip.retime.values.constFind(keys::RetimeFrame);
    return it == clip.retime.values.constEnd() ? -1 : int(it->indexOf(frame));
}

/// Shifts every key after index @p index by @p frames frames and @p value in value.
void shiftAfter(AnimatedValue& c, int index, int64_t frames, double value)
{
    for (qsizetype i = index + 1; i < c.keys.size(); ++i) {
        c.keys[i].frame += frames;
        c.keys[i].value += value;
    }
}

double chord(const AnimatedValue& c, int a, int b)
{
    return (c.keys[b].value - c.keys[a].value) / double(c.keys[b].frame - c.keys[a].frame);
}

/// The key indices bounding segment @p segment after ensureCurve(), or false.
bool segmentKeys(ClipData& clip, int segment, int& a, int& b)
{
    const QVector<retime::Segment> segs = retime::segments(clip);
    if (segment < 0 || segment >= segs.size())
        return false;
    ensureCurve(clip);
    a = keyIndex(clip, segs[segment].startLocal);
    b = keyIndex(clip, segs[segment].endLocal);
    return a >= 0 && b > a;
}

/// The keys after an edited segment moved by @p frames: the clip end (a key, see
/// ensureCurve()) moved with them, so the duration grows by the same amount.
std::optional<int64_t> grow(ClipData& clip, int64_t frames)
{
    clip.duration += frames;
    simplifyCurve(clip);
    return frames;
}

} // namespace

// ---- Curve housekeeping ----

void ensureCurve(ClipData& clip)
{
    if (!retime::hasCurve(clip)) {
        AnimatedValue& c = curveOf(clip);
        c.keys.clear();
        c.keys.append(linearKey(0, 0.0));
        c.keys.append(linearKey(clip.duration, double(clip.duration) * clip.speed));
        return;
    }
    // Keys at both edges (a trim may have left them outside): evaluated before inserting so
    // the curve does not change.
    const double atStart = retime::position(clip, 0.0);
    const double atEnd = retime::position(clip, double(clip.duration));
    AnimatedValue& c = curveOf(clip);
    if (!c.hasKeyAt(0))
        c.putKey(linearKey(0, atStart));
    if (!c.hasKeyAt(clip.duration))
        c.putKey(linearKey(clip.duration, atEnd));
}

void simplifyCurve(ClipData& clip)
{
    const AnimatedValue* c = retime::curve(clip);
    if (!c) {
        // A lone key or an empty value carries no meaning: drop it.
        clip.retime.values.remove(keys::RetimeFrame);
        return;
    }
    const QVector<Keyframe>& k = c->keys;
    // Speed points the user placed stay, even between equal speeds (Resolve keeps them too).
    if (!retime::speedPoints(clip).isEmpty())
        return;
    const double slope = (k[1].value - k[0].value) / double(k[1].frame - k[0].frame);
    for (qsizetype i = 0; i < k.size(); ++i) {
        const Keyframe& key = k[i];
        if (key.hold || key.in.mode != KeyHandle::Mode::Linear
            || key.out.mode != KeyHandle::Mode::Linear)
            return;
        if (i > 0) {
            const double s = (key.value - k[i - 1].value) / double(key.frame - k[i - 1].frame);
            if (std::abs(s - slope) > kSameSpeed)
                return;
        }
    }
    // R(x) = R(0) + slope * x everywhere: the constant speed shows the same source times.
    clip.speed = std::abs(slope) < kSameSpeed ? 0.0 : slope;
    clip.retime.values.remove(keys::RetimeFrame);
}

// ---- Partners ----

QVector<quint64> partners(const TimelineData& data, quint64 clipId)
{
    const ClipData* clip = editops::clipById(data, clipId);
    if (!clip)
        return {};
    QVector<quint64> out {clipId};
    if (clip->linkGroup == 0)
        return out;
    for (TrackType type : {TrackType::Video, TrackType::Audio}) {
        for (const TrackData& t : data.tracks(type)) {
            for (const ClipData& c : t.clips) {
                if (c.id != clipId && c.linkGroup == clip->linkGroup && sameRetime(c, *clip))
                    out.append(c.id);
            }
        }
    }
    return out;
}

QVector<quint64> primaries(const TimelineData& data, const QVector<quint64>& clipIds)
{
    QVector<quint64> out;
    QVector<quint64> covered;
    for (quint64 id : clipIds) {
        if (covered.contains(id))
            continue;
        const QVector<quint64> group = partners(data, id);
        if (group.isEmpty())
            continue;
        out.append(id);
        covered += group;
    }
    return out;
}

// ---- Speed points ----

bool addSpeedPoint(TimelineData& data, quint64 clipId, int64_t frame)
{
    return applyToGroup(data, clipId, [frame](ClipData& clip, Rational) -> std::optional<int64_t> {
        const int64_t local = frame - clip.start;
        if (local <= 0 || local >= clip.duration)
            return std::nullopt;
        ensureCurve(clip);
        if (keyIndex(clip, local) >= 0)
            return std::nullopt;
        const double value = retime::position(clip, double(local));
        curveOf(clip).putKey(linearKey(local, value));
        return 0;
    });
}

bool removeSpeedPoint(TimelineData& data, quint64 clipId, int64_t localFrame)
{
    return applyToGroup(
        data, clipId, [localFrame](ClipData& clip, Rational) -> std::optional<int64_t> {
            if (localFrame <= 0 || localFrame >= clip.duration || keyIndex(clip, localFrame) < 0)
                return std::nullopt;
            ensureCurve(clip);
            curveOf(clip).removeKey(localFrame);
            simplifyCurve(clip);
            return 0;
        });
}

bool setSegmentSpeed(TimelineData& data, quint64 clipId, int segment, double speed)
{
    if (!std::isfinite(speed))
        return false;
    speed = std::clamp(speed, -kMaxSpeed, kMaxSpeed);
    return applyToGroup(
        data, clipId, [segment, speed](ClipData& clip, Rational) -> std::optional<int64_t> {
            int a = -1, b = -1;
            if (!segmentKeys(clip, segment, a, b))
                return std::nullopt;
            AnimatedValue& c = curveOf(clip);
            const int64_t len = c.keys[b].frame - c.keys[a].frame;
            const double span = c.keys[b].value - c.keys[a].value;
            int64_t newLen = len;
            double newSpan = 0.0;
            if (speed == 0.0) {
                newSpan = 0.0; // freeze: the segment keeps its length
            } else if (std::abs(span) < kSameSpeed) {
                newSpan = speed * double(len); // a freeze starts to play
            } else {
                newLen = std::max<int64_t>(1, std::llround(std::abs(span) / std::abs(speed)));
                newSpan = speed * double(newLen); // exact speed, source end moves < 1 frame
            }
            if (newLen == len && newSpan == span)
                return std::nullopt;
            c.keys[b].value = c.keys[a].value + newSpan;
            c.keys[b].frame = c.keys[a].frame + newLen;
            shiftAfter(c, b, newLen - len, newSpan - span);
            return grow(clip, newLen - len);
        });
}

bool reverseSegment(TimelineData& data, quint64 clipId, int segment)
{
    return applyToGroup(
        data, clipId, [segment](ClipData& clip, Rational) -> std::optional<int64_t> {
            int a = -1, b = -1;
            if (!segmentKeys(clip, segment, a, b))
                return std::nullopt;
            AnimatedValue& c = curveOf(clip);
            const double span = c.keys[b].value - c.keys[a].value;
            if (std::abs(span) < kSameSpeed)
                return std::nullopt; // a freeze has no direction
            c.keys[b].value -= 2.0 * span;
            shiftAfter(c, b, 0, -2.0 * span);
            return grow(clip, 0);
        });
}

bool insertFreeze(TimelineData& data, quint64 clipId, int64_t frame, int64_t lengthFrames)
{
    if (lengthFrames < 1)
        return false;
    return applyToGroup(
        data, clipId, [frame, lengthFrames](ClipData& clip, Rational) -> std::optional<int64_t> {
            const int64_t local = frame - clip.start;
            if (local < 0 || local >= clip.duration)
                return std::nullopt;
            ensureCurve(clip);
            const double value = retime::position(clip, double(local));
            AnimatedValue& c = curveOf(clip);
            if (!c.hasKeyAt(local))
                c.putKey(linearKey(local, value));
            const int at = int(c.indexOf(local));
            shiftAfter(c, at, lengthFrames, 0.0);
            // The key at the freeze start keeps its in handle (a ramp may arrive there); the
            // freeze itself is flat.
            c.keys[at].out = KeyHandle {};
            c.putKey(linearKey(local + lengthFrames, value));
            return grow(clip, lengthFrames);
        });
}

bool freezeClip(TimelineData& data, quint64 clipId, int64_t frame)
{
    return applyToGroup(
        data, clipId, [frame](ClipData& clip, Rational fps) -> std::optional<int64_t> {
            int64_t local = frame - clip.start;
            if (local < 0 || local >= clip.duration)
                local = 0;
            int64_t us = retime::sourceTimeUs(clip, double(local), fps);
            if (clip.sourceDurationUs > 0)
                us = std::min(us, clip.sourceDurationUs - 1);
            us = std::max<int64_t>(0, us);
            if (!retime::hasCurve(clip) && clip.speed == 0.0 && clip.sourceInUs == us)
                return std::nullopt;
            clip.retime.values.remove(keys::RetimeFrame);
            clip.speed = 0.0;
            clip.sourceInUs = us;
            return 0;
        });
}

bool resetClip(TimelineData& data, quint64 clipId)
{
    return applyToGroup(data, clipId, [](ClipData& clip, Rational fps) -> std::optional<int64_t> {
        if (!retime::isRetimed(clip))
            return std::nullopt;
        const double net
            = retime::position(clip, double(clip.duration)) - retime::position(clip, 0.0);
        // Forward material keeps its source range: the length becomes the source it covered.
        int64_t newDuration = net >= 1.0 ? int64_t(std::llround(net)) : clip.duration;
        if (net < 1.0) // reverse / freeze / bounce: play forward from the lowest frame shown
            clip.sourceInUs
                = std::max<int64_t>(0, retime::shownSourceRange(clip, 0, clip.duration, fps).minUs);
        clip.retime.values.remove(keys::RetimeFrame);
        clip.speed = 1.0;
        if (clip.sourceDurationUs > 0 && fps.isValid()) {
            const int64_t available = int64_t(
                std::floor(double(clip.sourceDurationUs - clip.sourceInUs) / retime::frameUs(fps)));
            newDuration = std::clamp<int64_t>(available, 1, newDuration);
        }
        const int64_t delta = newDuration - clip.duration;
        clip.duration = newDuration;
        return delta;
    });
}

int64_t retimePoint(TimelineData& data, quint64 clipId, int64_t pointLocal, int64_t deltaFrames)
{
    int64_t applied = 0;
    applyToGroup(data, clipId,
        [&applied, pointLocal, deltaFrames](ClipData& clip, Rational) -> std::optional<int64_t> {
            if (pointLocal <= 0 || pointLocal > clip.duration || deltaFrames == 0)
                return std::nullopt;
            ensureCurve(clip);
            const int k = keyIndex(clip, pointLocal);
            if (k <= 0)
                return std::nullopt;
            AnimatedValue& c = curveOf(clip);
            const int64_t len = c.keys[k].frame - c.keys[k - 1].frame;
            const int64_t delta = std::max<int64_t>(deltaFrames, 1 - len);
            if (delta == 0)
                return std::nullopt;
            // The segment keeps its source: only frames move, from the point on.
            shiftAfter(c, k - 1, delta, 0.0);
            applied = delta;
            return grow(clip, delta);
        });
    return applied;
}

int64_t slidePoint(TimelineData& data, quint64 clipId, int64_t pointLocal, int64_t deltaFrames)
{
    int64_t applied = 0;
    applyToGroup(data, clipId,
        [&applied, pointLocal, deltaFrames](ClipData& clip, Rational) -> std::optional<int64_t> {
            if (pointLocal <= 0 || pointLocal >= clip.duration || deltaFrames == 0)
                return std::nullopt;
            ensureCurve(clip);
            const int k = keyIndex(clip, pointLocal);
            AnimatedValue& c = curveOf(clip);
            if (k <= 0 || k + 1 >= c.keys.size())
                return std::nullopt;
            const double s1 = chord(c, k - 1, k);
            const double s2 = chord(c, k, k + 1);
            const int64_t len1 = c.keys[k].frame - c.keys[k - 1].frame;
            const int64_t len2 = c.keys[k + 1].frame - c.keys[k].frame;
            const double v = c.keys[k].value;
            const double vNext = c.keys[k + 1].value;
            // Length of the next segment when the point moves by d (its speed kept): it covers
            // the source from the point's new value to the next point's value.
            auto nextLen = [&](int64_t d) -> int64_t {
                if (std::abs(s2) < kSameSpeed)
                    return len2;
                return std::llround((vNext - (v + s1 * double(d))) / s2);
            };
            int64_t d = std::max<int64_t>(deltaFrames, 1 - len1);
            // Shrink |d| until the next segment keeps at least one frame (it can only lose
            // material when the point moves into it).
            while (d != 0 && nextLen(d) < 1)
                d += d > 0 ? -1 : 1;
            if (d == 0)
                return std::nullopt;
            const int64_t newLen2 = nextLen(d);
            const double newValue = v + s1 * double(d);
            c.keys[k].frame += d;
            c.keys[k].value = newValue;
            const int64_t shift = (c.keys[k].frame + newLen2) - c.keys[k + 1].frame;
            if (std::abs(s2) < kSameSpeed) {
                // The freeze after the point shows the new frame; everything after it follows.
                shiftAfter(c, k, shift, newValue - v);
            } else {
                // Whole frames: the next segment keeps its exact speed, so its end (and what
                // follows) moves by the rounding (< half a source frame at that speed).
                const double exactEnd = newValue + s2 * double(newLen2);
                shiftAfter(c, k, shift, exactEnd - vNext);
            }
            applied = d;
            return grow(clip, shift);
        });
    return applied;
}

bool setPointRamp(TimelineData& data, quint64 clipId, int64_t pointLocal, Interp ramp)
{
    return applyToGroup(
        data, clipId, [pointLocal, ramp](ClipData& clip, Rational) -> std::optional<int64_t> {
            if (pointLocal <= 0 || pointLocal >= clip.duration)
                return std::nullopt;
            const int k = keyIndex(clip, pointLocal);
            if (k < 0)
                return std::nullopt;
            Keyframe& key = curveOf(clip).keys[k];
            const Keyframe before = key;
            if (ramp == Interp::Smooth) {
                key.in = KeyHandle::withMode(KeyHandle::Mode::Smooth);
                key.out = KeyHandle::withMode(KeyHandle::Mode::Smooth);
                key.hold = false;
            } else {
                applyInterp(key, ramp);
            }
            if (key == before)
                return std::nullopt;
            return 0;
        });
}

// ---- Dialog ----

bool changeClipSpeed(TimelineData& data, quint64 clipId, const SpeedChange& change)
{
    if (!change.freeze && !(change.speed > 0.0 && std::isfinite(change.speed)))
        return false;
    return applyToGroup(
        data, clipId, [&change](ClipData& clip, Rational fps) -> std::optional<int64_t> {
            const ClipData before = clip;
            clip.pitchCorrection = change.pitchCorrection;
            if (change.freeze) {
                int64_t local = change.freezeFrame - clip.start;
                if (change.freezeFrame < 0 || local < 0 || local >= clip.duration)
                    local = 0;
                int64_t us = retime::sourceTimeUs(clip, double(local), fps);
                if (clip.sourceDurationUs > 0)
                    us = std::min(us, clip.sourceDurationUs - 1);
                clip.sourceInUs = std::max<int64_t>(0, us);
                clip.retime.values.remove(keys::RetimeFrame);
                clip.speed = 0.0;
                if (clip == before)
                    return std::nullopt;
                return 0;
            }
            const double fu = retime::frameUs(fps);
            const double magnitude = std::min(change.speed, kMaxSpeed);
            // The material the clip covers: its lowest shown source time and its length in
            // retime frames (a freeze covers nothing and is treated as one frame per frame).
            const retime::SourceRange shown = retime::shownSourceRange(clip, 0, clip.duration, fps);
            double span = std::abs(
                retime::position(clip, double(clip.duration)) - retime::position(clip, 0.0));
            if (span < kSameSpeed)
                span = double(clip.duration);
            int64_t frames = change.ripple ? std::max<int64_t>(1, std::llround(span / magnitude))
                                           : clip.duration;
            const int64_t low = std::max<int64_t>(0, shown.minUs);
            if (clip.sourceDurationUs > 0 && fu > 0
                && (clip.kind == ClipKind::Media || clip.kind == ClipKind::Nested)) {
                const auto available = int64_t(
                    std::floor(double(clip.sourceDurationUs - low) / (fu * magnitude) + 1e-6));
                frames = std::clamp<int64_t>(available, 1, frames);
            }
            clip.retime.values.remove(keys::RetimeFrame);
            clip.speed = change.reverse ? -magnitude : magnitude;
            // Forward: start at the lowest frame. Reverse: start at the last frame shown at the new
            // speed and walk back to the lowest one.
            clip.sourceInUs = change.reverse
                ? low + int64_t(std::llround(double(frames - 1) * fu * magnitude))
                : low;
            const int64_t delta = frames - clip.duration;
            clip.duration = frames;
            if (clip == before)
                return std::nullopt;
            return change.ripple ? delta : 0;
        });
}

SpeedState speedState(const ClipData& clip)
{
    SpeedState s;
    s.material
        = std::abs(retime::position(clip, double(clip.duration)) - retime::position(clip, 0.0));
    if (s.material < 1e-9)
        s.material = double(std::max<int64_t>(1, clip.duration));
    s.curve = retime::hasCurve(clip);
    s.segments = int(retime::segments(clip).size());
    s.speed = !s.curve && clip.speed != 0.0
        ? std::abs(clip.speed)
        : s.material / double(std::max<int64_t>(1, clip.duration));
    s.reverse = !s.curve && clip.speed < 0;
    s.freeze = !s.curve && clip.speed == 0.0;
    return s;
}

bool setProcess(TimelineData& data, quint64 clipId, RetimeProcess process)
{
    return applyToGroup(
        data, clipId, [process](ClipData& clip, Rational) -> std::optional<int64_t> {
            if (clip.retimeProcess == process)
                return std::nullopt;
            clip.retimeProcess = process;
            return 0;
        });
}

bool setPitchCorrection(TimelineData& data, quint64 clipId, bool on)
{
    return applyToGroup(data, clipId, [on](ClipData& clip, Rational) -> std::optional<int64_t> {
        if (clip.pitchCorrection == on)
            return std::nullopt;
        clip.pitchCorrection = on;
        return 0;
    });
}

} // namespace kk::retimeops
