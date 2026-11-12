// SPDX-License-Identifier: GPL-3.0-or-later
// Keyframe interpolation maths (cubic Bezier segments with resolved handles) and ParamSet
// helpers.
#include "core/Animation.h"

#include "core/ParamKeys.h"

#include <QSettings>

#include <algorithm>
#include <cmath>

namespace kk {

using Mode = KeyHandle::Mode;

// ---- Presets ----

void applyInterp(Keyframe& key, Interp interp)
{
    auto set = [&key](Mode in, Mode out) {
        key.in = KeyHandle::withMode(in);
        key.out = KeyHandle::withMode(out);
        key.hold = false;
    };
    switch (interp) {
    case Interp::Linear:
        set(Mode::Linear, Mode::Linear);
        break;
    case Interp::EaseIn:
        set(Mode::Ease, Mode::Linear);
        break;
    case Interp::EaseOut:
        set(Mode::Linear, Mode::Ease);
        break;
    case Interp::EaseInOut:
        set(Mode::Ease, Mode::Ease);
        break;
    case Interp::Smooth:
        set(Mode::Smooth, Mode::Smooth);
        break;
    case Interp::Hold:
        key.hold = true; // handles kept: switching back restores the shape
        break;
    case Interp::Custom:
        break;
    }
}

Interp keyInterp(const Keyframe& key)
{
    if (key.hold)
        return Interp::Hold;
    const Mode i = key.in.mode;
    const Mode o = key.out.mode;
    if (i == Mode::Linear && o == Mode::Linear)
        return Interp::Linear;
    if (i == Mode::Ease && o == Mode::Linear)
        return Interp::EaseIn;
    if (i == Mode::Linear && o == Mode::Ease)
        return Interp::EaseOut;
    if (i == Mode::Ease && o == Mode::Ease)
        return Interp::EaseInOut;
    if (i == Mode::Smooth && o == Mode::Smooth)
        return Interp::Smooth;
    return Interp::Custom;
}

namespace {
// Index of the first key whose frame is >= frame (keys are sorted).
qsizetype lowerBound(const QVector<Keyframe>& keys, int64_t frame)
{
    const auto it = std::lower_bound(keys.cbegin(), keys.cend(), frame,
        [](const Keyframe& k, int64_t f) { return k.frame < f; });
    return it - keys.cbegin();
}

// One coordinate of a cubic Bezier with control values c0..c3 at parameter s.
inline double bezier(double c0, double c1, double c2, double c3, double s)
{
    const double r = 1.0 - s;
    return r * r * r * c0 + 3.0 * r * r * s * c1 + 3.0 * r * s * s * c2 + s * s * s * c3;
}

// Derivative of bezier() with respect to s.
inline double bezierSlope(double c0, double c1, double c2, double c3, double s)
{
    const double r = 1.0 - s;
    return 3.0 * r * r * (c1 - c0) + 6.0 * r * s * (c2 - c1) + 3.0 * s * s * (c3 - c2);
}

// Solves x(s) = u for s in [0, 1] where x is the time coordinate of a segment Bezier with
// x0 = 0, x3 = length and x1, x2 inside [0, length]. Those control points make x(s) monotone
// (the derivative's Bernstein coefficients a, b, c satisfy b >= -sqrt(a c)), so the root is
// unique. Newton converges in 2-4 steps from the linear guess; a bisection bracket keeps it
// safe where the derivative vanishes (handles of length 0 or of the full segment).
double solveBezierTime(double x1, double x2, double length, double u)
{
    double lo = 0.0, hi = 1.0;
    double s = u / length;
    constexpr int kMaxIterations = 24;
    const double tolerance = 1e-10 * length;
    for (int i = 0; i < kMaxIterations; ++i) {
        const double f = bezier(0.0, x1, x2, length, s) - u;
        if (std::abs(f) <= tolerance)
            break;
        if (f > 0)
            hi = s;
        else
            lo = s;
        const double d = bezierSlope(0.0, x1, x2, length, s);
        double next = d > 1e-12 ? s - f / d : -1.0;
        if (next <= lo || next >= hi)
            next = 0.5 * (lo + hi); // Newton left the bracket: bisect
        s = next;
    }
    return s;
}
} // namespace

// ---- Handles ----

double AnimatedValue::smoothSlope(qsizetype i) const
{
    // Catmull-Rom tangent through the neighbours, flattened at extrema and at both ends so a
    // smooth curve never overshoots the key values (no surprise negative opacity).
    if (i <= 0 || i >= keys.size() - 1)
        return 0.0;
    const Keyframe& p = keys[i - 1];
    const Keyframe& k = keys[i];
    const Keyframe& n = keys[i + 1];
    if ((k.value - p.value) * (n.value - k.value) <= 0.0)
        return 0.0;
    return (n.value - p.value) / double(n.frame - p.frame);
}

namespace {
// Resolves one handle of a key given the segment it belongs to (length dt > 0, value change
// dv measured away from the key) and the smooth slope. Returns {dtFrames, dv}.
std::pair<double, double> resolveHandle(
    const KeyHandle& h, double segDt, double segDv, double slope)
{
    const double third = segDt / 3.0;
    switch (h.mode) {
    case Mode::Linear:
        return {third, segDv / 3.0};
    case Mode::Ease:
        return {third, 0.0};
    case Mode::Smooth:
        return {third, slope * third};
    case Mode::Free:
        break;
    }
    // Free: clamp the time length into the segment, keeping the slope.
    double dt = std::max(0.0, h.dtFrames);
    double dv = h.dv;
    if (dt > segDt && dt > 0.0) {
        dv *= segDt / dt;
        dt = segDt;
    }
    return {dt, dv};
}
} // namespace

std::pair<double, double> AnimatedValue::outHandle(qsizetype i) const
{
    if (i < 0 || i >= keys.size())
        return {0.0, 0.0};
    const Keyframe& k = keys[i];
    if (i + 1 < keys.size()) {
        const Keyframe& n = keys[i + 1];
        // The outgoing handle's value runs in the segment direction (+time).
        return resolveHandle(k.out, double(n.frame - k.frame), n.value - k.value, smoothSlope(i));
    }
    if (i > 0) {
        // Last key: mirror the length of the incoming segment (display only). A Linear handle
        // would point at a missing neighbour, so it is drawn flat.
        const double segDt = double(k.frame - keys[i - 1].frame);
        const KeyHandle h = k.out.mode == Mode::Linear ? KeyHandle::withMode(Mode::Ease) : k.out;
        return resolveHandle(h, segDt, 0.0, smoothSlope(i));
    }
    return {0.0, 0.0};
}

std::pair<double, double> AnimatedValue::inHandle(qsizetype i) const
{
    if (i < 0 || i >= keys.size())
        return {0.0, 0.0};
    const Keyframe& k = keys[i];
    if (i > 0) {
        const Keyframe& p = keys[i - 1];
        // Incoming handle: dv is key.value - tip.value, so a Linear handle uses the segment's
        // own value change and the tip sits on the straight line back to the previous key.
        return resolveHandle(k.in, double(k.frame - p.frame), k.value - p.value, smoothSlope(i));
    }
    if (i + 1 < keys.size()) {
        const double segDt = double(keys[i + 1].frame - k.frame);
        const KeyHandle h = k.in.mode == Mode::Linear ? KeyHandle::withMode(Mode::Ease) : k.in;
        return resolveHandle(h, segDt, 0.0, smoothSlope(i));
    }
    return {0.0, 0.0};
}

// ---- Evaluation ----

double AnimatedValue::segmentValue(qsizetype i, double localFrame) const
{
    // keys[i].frame < localFrame < keys[i + 1].frame
    const Keyframe& a = keys[i];
    const Keyframe& b = keys[i + 1];
    if (a.hold)
        return a.value;
    const double length = double(b.frame - a.frame);
    const double u = localFrame - double(a.frame);
    // Fast path: a straight segment (the default for every key).
    if (a.out.mode == Mode::Linear && b.in.mode == Mode::Linear)
        return a.value + (b.value - a.value) * (u / length);

    const auto [outDt, outDv] = outHandle(i);
    const auto [inDt, inDv] = inHandle(i + 1);
    const double y1 = a.value + outDv;
    const double y2 = b.value - inDv;
    double s = u / length;
    // Non-Free handles are exactly one third of the segment long, which makes x(s) = s *
    // length: no root finding needed. Only user-dragged handles need the solver.
    if (a.out.mode == Mode::Free || b.in.mode == Mode::Free)
        s = solveBezierTime(outDt, length - inDt, length, u);
    return bezier(a.value, y1, y2, b.value, s);
}

double AnimatedValue::valueAtTime(double localFrame) const
{
    if (keys.isEmpty())
        return base;
    if (localFrame <= double(keys.front().frame))
        return keys.front().value;
    if (localFrame >= double(keys.back().frame))
        return keys.back().value;
    // Last key at or before localFrame (binary search; keys are sorted).
    const auto it = std::upper_bound(keys.cbegin(), keys.cend(), localFrame,
        [](double f, const Keyframe& k) { return f < double(k.frame); });
    const qsizetype i = (it - keys.cbegin()) - 1;
    if (double(keys[i].frame) == localFrame)
        return keys[i].value;
    return segmentValue(i, localFrame);
}

// Hot path (called per parameter, per clip, per rendered frame): binary search on the sorted
// keys, no allocation. Frames are exact in a double (|frame| < 2^53).
double AnimatedValue::valueAt(int64_t localFrame) const
{
    return valueAtTime(double(localFrame));
}

// ---- Editing ----

void AnimatedValue::setKey(int64_t localFrame, double value, std::optional<Interp> interp)
{
    const qsizetype i = lowerBound(keys, localFrame);
    if (i < keys.size() && keys[i].frame == localFrame) {
        keys[i].value = value;
        if (interp)
            applyInterp(keys[i], *interp);
        return;
    }
    Keyframe key;
    key.frame = localFrame;
    key.value = value;
    applyInterp(key, interp ? *interp : defaultKeyInterp());
    keys.insert(i, key);
}

void AnimatedValue::putKey(const Keyframe& key)
{
    const qsizetype i = lowerBound(keys, key.frame);
    if (i < keys.size() && keys[i].frame == key.frame)
        keys[i] = key;
    else
        keys.insert(i, key);
}

void AnimatedValue::removeKey(int64_t localFrame)
{
    const qsizetype i = indexOf(localFrame);
    if (i < 0)
        return;
    if (keys.size() == 1)
        base = keys[i].value;
    keys.remove(i);
}

qsizetype AnimatedValue::indexOf(int64_t localFrame) const
{
    const qsizetype i = lowerBound(keys, localFrame);
    return i < keys.size() && keys[i].frame == localFrame ? i : -1;
}

void AnimatedValue::setValue(int64_t localFrame, double value)
{
    if (!isAnimated()) {
        base = value;
        return;
    }
    // Keeps the handles of an existing key; new keys get the default (Linear).
    setKey(localFrame, value);
}

std::optional<int64_t> AnimatedValue::previousKeyFrame(int64_t localFrame) const
{
    const qsizetype i = lowerBound(keys, localFrame);
    if (i == 0)
        return std::nullopt;
    return keys[i - 1].frame;
}

std::optional<int64_t> AnimatedValue::nextKeyFrame(int64_t localFrame) const
{
    const auto it = std::upper_bound(keys.cbegin(), keys.cend(), localFrame,
        [](int64_t f, const Keyframe& k) { return f < k.frame; });
    if (it == keys.cend())
        return std::nullopt;
    return it->frame;
}

void AnimatedValue::shiftKeys(int64_t deltaFrames)
{
    for (Keyframe& k : keys)
        k.frame += deltaFrames;
}

void migrateLegacyInterp(AnimatedValue& value, const QVector<Interp>& legacy)
{
    const qsizetype n = std::min(value.keys.size(), legacy.size());
    for (qsizetype i = 0; i < n; ++i) {
        Keyframe& k = value.keys[i];
        const Interp old = legacy[i];
        if (old == Interp::Hold)
            k.hold = true;
        // v1 easeIn: the segment starts slowly = the key is left with zero speed.
        if (old == Interp::EaseIn || old == Interp::EaseInOut)
            k.out.mode = Mode::Ease;
        // v1 easeOut: the segment ends slowly = the next key is reached with zero speed.
        if ((old == Interp::EaseOut || old == Interp::EaseInOut) && i + 1 < value.keys.size())
            value.keys[i + 1].in.mode = Mode::Ease;
    }
}

// ---- ParamSet ----

double ParamSet::value(const QString& key, int64_t localFrame, double fallback) const
{
    const auto it = values.constFind(key);
    return it == values.cend() ? fallback : it->valueAt(localFrame);
}

double ParamSet::value(const QString& key, int64_t localFrame) const
{
    // Look up first: the default table is only consulted for absent keys.
    const auto it = values.constFind(key);
    return it == values.cend() ? keys::defaultValue(key) : it->valueAt(localFrame);
}

QVariant ParamSet::staticValue(const QString& key, const QVariant& fallback) const
{
    return statics.value(key, fallback);
}

AnimatedValue& ParamSet::animated(const QString& key, double defaultBase)
{
    auto it = values.find(key);
    if (it == values.end())
        it = values.insert(key, AnimatedValue(defaultBase));
    return *it;
}

void ParamSet::setBase(const QString& key, double value)
{
    animated(key, value).base = value;
}

bool ParamSet::isAnimated() const
{
    return std::any_of(
        values.cbegin(), values.cend(), [](const AnimatedValue& v) { return v.isAnimated(); });
}

void ParamSet::shiftKeys(int64_t deltaFrames)
{
    for (AnimatedValue& v : values)
        v.shiftKeys(deltaFrames);
}

// ---- Names ----

Interp defaultKeyInterp()
{
    static constexpr Interp kChoices[] = {Interp::Linear, Interp::EaseIn, Interp::EaseOut,
        Interp::EaseInOut, Interp::Smooth, Interp::Hold};
    const QString text = QSettings()
                             .value(QStringLiteral("edit/newKeyInterp"), QStringLiteral("linear"))
                             .toString();
    for (const Interp choice : kChoices) {
        if (interpToString(choice) == text)
            return choice;
    }
    return Interp::Linear;
}

void setDefaultKeyInterp(Interp interp)
{
    if (interp != Interp::Custom)
        QSettings().setValue(QStringLiteral("edit/newKeyInterp"), interpToString(interp));
}

QString interpToString(Interp interp)
{
    switch (interp) {
    case Interp::Linear:
        return QStringLiteral("linear");
    case Interp::EaseIn:
        return QStringLiteral("easeIn");
    case Interp::EaseOut:
        return QStringLiteral("easeOut");
    case Interp::EaseInOut:
        return QStringLiteral("easeInOut");
    case Interp::Hold:
        return QStringLiteral("hold");
    case Interp::Smooth:
        return QStringLiteral("smooth");
    case Interp::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("linear");
}

Interp interpFromString(const QString& text)
{
    if (text == QLatin1String("easeIn"))
        return Interp::EaseIn;
    if (text == QLatin1String("easeOut"))
        return Interp::EaseOut;
    if (text == QLatin1String("easeInOut"))
        return Interp::EaseInOut;
    if (text == QLatin1String("hold"))
        return Interp::Hold;
    if (text == QLatin1String("smooth"))
        return Interp::Smooth;
    if (text == QLatin1String("custom"))
        return Interp::Custom;
    return Interp::Linear;
}

QString handleModeToString(KeyHandle::Mode mode)
{
    switch (mode) {
    case Mode::Linear:
        return QStringLiteral("linear");
    case Mode::Ease:
        return QStringLiteral("ease");
    case Mode::Smooth:
        return QStringLiteral("smooth");
    case Mode::Free:
        return QStringLiteral("free");
    }
    return QStringLiteral("linear");
}

KeyHandle::Mode handleModeFromString(const QString& text)
{
    if (text == QLatin1String("ease"))
        return Mode::Ease;
    if (text == QLatin1String("smooth"))
        return Mode::Smooth;
    if (text == QLatin1String("free"))
        return Mode::Free;
    return Mode::Linear;
}

} // namespace kk
