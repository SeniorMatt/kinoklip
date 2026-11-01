// SPDX-License-Identifier: GPL-3.0-or-later
// Keyframe interpolation maths and ParamSet helpers.
#include "core/Animation.h"

#include "core/ParamKeys.h"

#include <algorithm>

namespace cw {

double easeCurve(Interp interp, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    switch (interp) {
    case Interp::Linear:
        return t;
    case Interp::EaseIn:
        return t * t;
    case Interp::EaseOut:
        return 1.0 - (1.0 - t) * (1.0 - t);
    case Interp::EaseInOut:
        return t * t * (3.0 - 2.0 * t);
    case Interp::Hold:
        return 0.0;
    }
    return t;
}

namespace {
// Index of the first key whose frame is >= frame (keys are sorted).
qsizetype lowerBound(const QVector<Keyframe>& keys, int64_t frame)
{
    const auto it = std::lower_bound(keys.cbegin(), keys.cend(), frame,
        [](const Keyframe& k, int64_t f) { return k.frame < f; });
    return it - keys.cbegin();
}
} // namespace

double AnimatedValue::valueAt(int64_t localFrame) const
{
    if (keys.isEmpty())
        return base;
    if (localFrame <= keys.front().frame)
        return keys.front().value;
    if (localFrame >= keys.back().frame)
        return keys.back().value;

    // keys[i-1].frame < localFrame <= keys[i].frame
    const qsizetype i = lowerBound(keys, localFrame);
    const Keyframe& b = keys[i];
    if (b.frame == localFrame)
        return b.value;
    const Keyframe& a = keys[i - 1];
    const double t = double(localFrame - a.frame) / double(b.frame - a.frame);
    return a.value + (b.value - a.value) * easeCurve(a.interp, t);
}

void AnimatedValue::setKey(int64_t localFrame, double value, Interp interp)
{
    const qsizetype i = lowerBound(keys, localFrame);
    if (i < keys.size() && keys[i].frame == localFrame) {
        keys[i].value = value;
        keys[i].interp = interp;
        return;
    }
    keys.insert(i, Keyframe {localFrame, value, interp});
}

void AnimatedValue::removeKey(int64_t localFrame)
{
    const qsizetype i = lowerBound(keys, localFrame);
    if (i >= keys.size() || keys[i].frame != localFrame)
        return;
    if (keys.size() == 1)
        base = keys[i].value;
    keys.remove(i);
}

bool AnimatedValue::hasKeyAt(int64_t localFrame) const
{
    const qsizetype i = lowerBound(keys, localFrame);
    return i < keys.size() && keys[i].frame == localFrame;
}

void AnimatedValue::setValue(int64_t localFrame, double value)
{
    if (!isAnimated()) {
        base = value;
        return;
    }
    // Keep the interpolation of an existing key; new keys get the default (Linear).
    const qsizetype i = lowerBound(keys, localFrame);
    if (i < keys.size() && keys[i].frame == localFrame)
        keys[i].value = value;
    else
        keys.insert(i, Keyframe {localFrame, value, Interp::Linear});
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

// ---- ParamSet ----

double ParamSet::value(const QString& key, int64_t localFrame, double fallback) const
{
    const auto it = values.constFind(key);
    return it == values.cend() ? fallback : it->valueAt(localFrame);
}

double ParamSet::value(const QString& key, int64_t localFrame) const
{
    return value(key, localFrame, keys::defaultValue(key));
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

// ---- Interp names ----

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
    return Interp::Linear;
}

} // namespace cw
