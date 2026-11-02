// SPDX-License-Identifier: GPL-3.0-or-later
// Biquad — RBJ cookbook coefficient formulas and frequency response.
#include "render/Biquad.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::dsp {

namespace {
/// Highest design frequency as a fraction of the sample rate (just below Nyquist, where the
/// bilinear transform's warping makes the formulas degenerate).
constexpr double kMaxFrequencyRatio = 0.49;
} // namespace

BiquadCoeffs designBand(
    EqBandType type, double frequencyHz, double gainDb, double q, double sampleRate)
{
    const double f = std::clamp(frequencyHz, 1.0, sampleRate * kMaxFrequencyRatio);
    const double w0 = 2.0 * std::numbers::pi * f / sampleRate;
    const double cosW = std::cos(w0);
    const double sinW = std::sin(w0);
    const double alpha = sinW / (2.0 * std::max(q, 1e-3));
    const double A = std::pow(10.0, gainDb / 40.0);

    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
    case EqBandType::HighPass:
        b0 = (1.0 + cosW) / 2.0;
        b1 = -(1.0 + cosW);
        b2 = (1.0 + cosW) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW;
        a2 = 1.0 - alpha;
        break;
    case EqBandType::LowPass:
        b0 = (1.0 - cosW) / 2.0;
        b1 = 1.0 - cosW;
        b2 = (1.0 - cosW) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW;
        a2 = 1.0 - alpha;
        break;
    case EqBandType::Bell:
        if (gainDb == 0.0)
            return {}; // exactly flat
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cosW;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cosW;
        a2 = 1.0 - alpha / A;
        break;
    case EqBandType::Notch:
        b0 = 1.0;
        b1 = -2.0 * cosW;
        b2 = 1.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW;
        a2 = 1.0 - alpha;
        break;
    case EqBandType::LowShelf: {
        if (gainDb == 0.0)
            return {};
        const double s = 2.0 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1.0) - (A - 1.0) * cosW + s);
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosW);
        b2 = A * ((A + 1.0) - (A - 1.0) * cosW - s);
        a0 = (A + 1.0) + (A - 1.0) * cosW + s;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosW);
        a2 = (A + 1.0) + (A - 1.0) * cosW - s;
        break;
    }
    case EqBandType::HighShelf: {
        if (gainDb == 0.0)
            return {};
        const double s = 2.0 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1.0) + (A - 1.0) * cosW + s);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW);
        b2 = A * ((A + 1.0) + (A - 1.0) * cosW - s);
        a0 = (A + 1.0) - (A - 1.0) * cosW + s;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosW);
        a2 = (A + 1.0) - (A - 1.0) * cosW - s;
        break;
    }
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

std::complex<double> response(const BiquadCoeffs& c, double frequencyHz, double sampleRate)
{
    const double w = 2.0 * std::numbers::pi * frequencyHz / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w); // z^-1
    const std::complex<double> z2 = z1 * z1;
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
}

double magnitudeDb(const BiquadCoeffs& c, double frequencyHz, double sampleRate)
{
    return 20.0 * std::log10(std::max(std::abs(response(c, frequencyHz, sampleRate)), 1e-12));
}

} // namespace kk::dsp
