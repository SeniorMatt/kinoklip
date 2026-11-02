// SPDX-License-Identifier: GPL-3.0-or-later
// LoudnessMeter — BS.1770-4 K-weighting, gated integration and 4x true peak.
#include "render/LoudnessMeter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::dsp {

namespace {

/// K-weighting stage 1 (spherical head high shelf) at 48 kHz, BS.1770-4 table 1.
constexpr double kPreB0 = 1.53512485958697, kPreB1 = -2.69169618940638, kPreB2 = 1.19839281085285,
                 kPreA1 = -1.69065929318241, kPreA2 = 0.73248077421585;
/// K-weighting stage 2 (RLB high-pass) at 48 kHz, BS.1770-4 table 2.
constexpr double kRlbB0 = 1.0, kRlbB1 = -2.0, kRlbB2 = 1.0, kRlbA1 = -1.99004745483398,
                 kRlbA2 = 0.99007225036621;
/// BS.1770 offset so that a 1 kHz sine reads its RMS level in dB (K-weighting gain at 1 kHz).
constexpr double kLoudnessOffset = -0.691;
constexpr double kAbsoluteGate = -70.0; ///< LUFS
constexpr double kRelativeGate = -10.0; ///< LU below the absolutely gated loudness
constexpr double kKaiserBeta = 5.0; ///< true-peak interpolator window

double besselI0(double x)
{
    // Power series; converges quickly for the small arguments of a Kaiser window.
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 32; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

/// One transposed direct form II biquad step on the state pair @p z.
inline double filter(
    double x, std::array<double, 2>& z, double b0, double b1, double b2, double a1, double a2)
{
    const double y = b0 * x + z[0];
    z[0] = b1 * x - a1 * y + z[1];
    z[1] = b2 * x - a2 * y;
    return y;
}

} // namespace

LoudnessMeter::LoudnessMeter()
    : m_histEnergy(kHistogramBins, 0.0)
    , m_histCount(kHistogramBins, 0)
{
    // Windowed-sinc 4x interpolator: tap n sits at (n - (kTaps - 1) / 2) / 4 input samples
    // from the centre; each phase is normalised to unity gain at DC.
    const double centre = (kTaps - 1) / 2.0;
    std::array<double, kTaps> h {};
    for (int n = 0; n < kTaps; ++n) {
        const double t = (n - centre) / kPhases;
        const double sinc
            = t == 0.0 ? 1.0 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
        const double r = (n - centre) / centre;
        const double w
            = besselI0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(kKaiserBeta);
        h[size_t(n)] = sinc * w;
    }
    for (int p = 0; p < kPhases; ++p) {
        double sum = 0.0;
        for (int k = 0; k < kTaps / kPhases; ++k)
            sum += h[size_t(p + kPhases * k)];
        for (int k = 0; k < kTaps / kPhases; ++k)
            m_kernel[size_t(p + kPhases * k)] = float(h[size_t(p + kPhases * k)] / sum);
    }
    reset();
}

void LoudnessMeter::reset()
{
    m_pre = {};
    m_rlb = {};
    m_blockSum = 0.0;
    m_blockFill = 0;
    m_blocks = {};
    m_blockCount = 0;
    m_blockHead = 0;
    m_momentary = kSilence;
    m_shortTerm = kSilence;
    std::fill(m_histEnergy.begin(), m_histEnergy.end(), 0.0);
    std::fill(m_histCount.begin(), m_histCount.end(), 0);
    m_history = {};
    m_historyHead = 0;
    m_truePeak = 0.0;
    m_truePeakRecent = 0.0;
}

double LoudnessMeter::loudnessOf(double meanSquareSum)
{
    return meanSquareSum > 0.0
        ? std::max(kSilence, kLoudnessOffset + 10.0 * std::log10(meanSquareSum))
        : kSilence;
}

void LoudnessMeter::process(const float* io, int frames)
{
    constexpr int kHist = kTaps / kPhases;
    double recent = 0.0;
    for (int i = 0; i < frames; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            const double x = io[size_t(i) * 2 + size_t(ch)];
            // ---- K-weighted energy ----
            const double k
                = filter(filter(x, m_pre[size_t(ch)], kPreB0, kPreB1, kPreB2, kPreA1, kPreA2),
                    m_rlb[size_t(ch)], kRlbB0, kRlbB1, kRlbB2, kRlbA1, kRlbA2);
            m_blockSum += k * k;
            // ---- True peak: the sample itself and the 4 interpolated points before it ----
            auto& hist = m_history[size_t(ch)];
            hist[size_t(m_historyHead)] = float(x);
            recent = std::max(recent, std::abs(x));
            for (int p = 0; p < kPhases; ++p) {
                double y = 0.0;
                for (int t = 0; t < kHist; ++t) {
                    const int idx = (m_historyHead - t + kHist) % kHist;
                    y += double(m_kernel[size_t(p + kPhases * t)]) * hist[size_t(idx)];
                }
                recent = std::max(recent, std::abs(y));
            }
        }
        m_historyHead = (m_historyHead + 1) % kHist;
        if (++m_blockFill == kBlockSamples)
            finishBlock();
    }
    m_truePeakRecent = recent;
    m_truePeak = std::max(m_truePeak, recent);
}

void LoudnessMeter::finishBlock()
{
    m_blocks[size_t(m_blockHead)] = m_blockSum / kBlockSamples;
    m_blockHead = (m_blockHead + 1) % kShortTermBlocks;
    m_blockCount = std::min(m_blockCount + 1, 1 << 30);
    m_blockSum = 0.0;
    m_blockFill = 0;

    auto meanOfLast = [this](int n) {
        double sum = 0.0;
        for (int k = 1; k <= n; ++k)
            sum += m_blocks[size_t((m_blockHead - k + kShortTermBlocks) % kShortTermBlocks)];
        return sum / n;
    };
    if (m_blockCount >= kMomentaryBlocks) {
        const double gatingBlock = meanOfLast(kMomentaryBlocks); // 400 ms, 75 % overlap
        m_momentary = loudnessOf(gatingBlock);
        if (m_momentary > kAbsoluteGate) {
            const int bin = std::clamp(
                int((m_momentary - kHistogramMin) / kHistogramStep), 0, kHistogramBins - 1);
            m_histEnergy[size_t(bin)] += gatingBlock;
            ++m_histCount[size_t(bin)];
        }
    }
    if (m_blockCount >= kShortTermBlocks)
        m_shortTerm = loudnessOf(meanOfLast(kShortTermBlocks));
}

double LoudnessMeter::integrated() const
{
    double energy = 0.0;
    int64_t count = 0;
    for (int b = 0; b < kHistogramBins; ++b) {
        energy += m_histEnergy[size_t(b)];
        count += m_histCount[size_t(b)];
    }
    if (count == 0)
        return kSilence;
    const double relative = loudnessOf(energy / double(count)) + kRelativeGate;
    energy = 0.0;
    count = 0;
    // Bins whose lower edge is at or above the relative gate (0.01 LU resolution).
    const int first = std::clamp(
        int(std::ceil((relative - kHistogramMin) / kHistogramStep)), 0, kHistogramBins);
    for (int b = first; b < kHistogramBins; ++b) {
        energy += m_histEnergy[size_t(b)];
        count += m_histCount[size_t(b)];
    }
    return count == 0 ? kSilence : loudnessOf(energy / double(count));
}

double LoudnessMeter::truePeakMax() const
{
    return m_truePeak > 0.0 ? 20.0 * std::log10(m_truePeak) : kSilence;
}

double LoudnessMeter::truePeakRecent() const
{
    return m_truePeakRecent > 0.0 ? 20.0 * std::log10(m_truePeakRecent) : kSilence;
}

} // namespace kk::dsp
