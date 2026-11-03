// SPDX-License-Identifier: GPL-3.0-or-later
// AudioTransitions implementation: id lookup and the closed-form gain curves.
#include "core/AudioTransitions.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::audiotransitions {

namespace {

/// Steepness of the exponential / logarithmic curves (chosen so the middle is clearly off
/// centre: exponential 0.119, logarithmic 0.740).
constexpr double kExpK = 4.0;
constexpr double kLogK = 9.0;

} // namespace

QString defaultId()
{
    return QLatin1String(kCrossFadePlus3dB);
}

Curve curveFor(const QString& typeId)
{
    if (typeId == QLatin1String(kCrossFade0dB))
        return Curve::Linear;
    if (typeId == QLatin1String(kCrossFadeMinus3dB))
        return Curve::Minus3dB;
    if (typeId == QLatin1String(kExponential))
        return Curve::Exponential;
    if (typeId == QLatin1String(kLogarithmic))
        return Curve::Logarithmic;
    if (typeId == QLatin1String(kSCurve))
        return Curve::SCurve;
    return Curve::EqualPower;
}

bool isAudioTransition(const QString& typeId)
{
    for (const char* id : {kCrossFade0dB, kCrossFadePlus3dB, kCrossFadeMinus3dB, kExponential,
             kLogarithmic, kSCurve}) {
        if (typeId == QLatin1String(id))
            return true;
    }
    return false;
}

double gainIn(Curve curve, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    constexpr double kHalfPi = std::numbers::pi / 2.0;
    switch (curve) {
    case Curve::Linear:
        return t;
    case Curve::Minus3dB:
        return 0.5 * (t + std::sin(t * kHalfPi));
    case Curve::EqualPower:
        return std::sin(t * kHalfPi);
    case Curve::Exponential:
        return std::expm1(kExpK * t) / std::expm1(kExpK);
    case Curve::Logarithmic:
        return std::log1p(kLogK * t) / std::log1p(kLogK);
    case Curve::SCurve:
        return 0.5 - 0.5 * std::cos(t * std::numbers::pi);
    }
    return t;
}

double gainOut(Curve curve, double t)
{
    return gainIn(curve, 1.0 - std::clamp(t, 0.0, 1.0));
}

} // namespace kk::audiotransitions
