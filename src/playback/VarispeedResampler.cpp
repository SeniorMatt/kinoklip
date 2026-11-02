// SPDX-License-Identifier: GPL-3.0-or-later
// VarispeedResampler — windowed-sinc table and the per-block resampling loop.
#include "playback/VarispeedResampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk {

namespace {
/// Zeroth-order modified Bessel function of the first kind (series; for the Kaiser window).
double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= q / (double(k) * double(k));
        sum += term;
        if (term < sum * 1e-12)
            break;
    }
    return sum;
}
} // namespace

VarispeedResampler::VarispeedResampler()
{
    // h(t) = sinc(kCutoff * t) * kaiser(t / kZeroCrossings), t in output samples. The scale
    // factor kCutoff is dropped: the weights are normalised per output frame anyway.
    const double i0Beta = besselI0(kKaiserBeta);
    for (size_t i = 0; i < m_table.size(); ++i) {
        const double t = double(i) / kTableSteps;
        if (t >= kZeroCrossings) {
            m_table[i] = 0.0f;
            continue;
        }
        const double x = std::numbers::pi * kCutoff * t;
        const double sinc = t == 0.0 ? 1.0 : std::sin(x) / x;
        const double r = t / kZeroCrossings;
        const double window = besselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / i0Beta;
        m_table[i] = float(sinc * window);
    }
}

int VarispeedResampler::margin(double rate)
{
    return int(std::ceil(kZeroCrossings * std::max(1.0, std::abs(rate)))) + 1;
}

float VarispeedResampler::kernel(double t) const
{
    const double idx = t * kTableSteps;
    const int i = int(idx);
    if (i >= kZeroCrossings * kTableSteps)
        return 0.0f;
    const float frac = float(idx - double(i));
    return m_table[size_t(i)] + (m_table[size_t(i) + 1] - m_table[size_t(i)]) * frac;
}

void VarispeedResampler::process(
    const float* span, int spanFrames, double pos0, double rate, int count, float* out) const
{
    constexpr int ch = kChannels;
    // Decimating: stretch the kernel by |rate| (cutoff follows the output Nyquist frequency).
    const double scale = std::max(1.0, std::abs(rate));
    const double invScale = 1.0 / scale;
    const double reach = kZeroCrossings * scale;
    for (int k = 0; k < count; ++k) {
        const double centre = pos0 + rate * double(k);
        const int first = std::max(0, int(std::ceil(centre - reach)));
        const int last = std::min(spanFrames - 1, int(std::floor(centre + reach)));
        float accL = 0.0f;
        float accR = 0.0f;
        float weights = 0.0f;
        for (int j = first; j <= last; ++j) {
            const float w = kernel(std::abs(double(j) - centre) * invScale);
            accL += w * span[size_t(j) * ch];
            accR += w * span[size_t(j) * ch + 1];
            weights += w;
        }
        // Taps outside the span count as silence; the normalisation uses the full kernel sum
        // (about scale for a stretched kernel), so a span edge fades instead of jumping.
        float norm = weights;
        if (first > int(std::ceil(centre - reach)) || last < int(std::floor(centre + reach))) {
            norm = 0.0f;
            const int a = int(std::ceil(centre - reach));
            const int b = int(std::floor(centre + reach));
            for (int j = a; j <= b; ++j)
                norm += kernel(std::abs(double(j) - centre) * invScale);
        }
        const float g = norm > 1e-6f ? 1.0f / norm : 0.0f;
        out[size_t(k) * ch] = accL * g;
        out[size_t(k) * ch + 1] = accR * g;
    }
}

} // namespace kk
