// SPDX-License-Identifier: GPL-3.0-or-later
// LoudnessMeter — ITU-R BS.1770-4 / EBU R128 loudness (momentary, short-term, integrated
// LUFS) and true peak (4x oversampled) of 48 kHz stereo audio.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace kk::dsp {

/// Measures loudness the way BS.1770-4 defines it, for 48 kHz interleaved stereo:
///
/// * K-weighting: the standard's two biquads for 48 kHz (high shelf "pre-filter" + RLB
///   high-pass), per channel; channel weights 1.0 for left and right;
/// * mean square per 100 ms block; momentary = the last 4 blocks (400 ms), short-term = the
///   last 30 blocks (3 s); loudness = -0.691 + 10 log10(sum of the channels' mean squares);
/// * integrated: 400 ms gating blocks every 100 ms (75 % overlap), absolute gate -70 LUFS,
///   relative gate 10 LU below the loudness of the blocks above the absolute gate. Blocks are
///   counted in a 0.01 LU histogram (energy sum + count per bin), so the measurement can run
///   for hours without allocating;
/// * true peak: 4x oversampling with a 48-tap windowed-sinc interpolator (12 taps per phase,
///   designed once in the constructor), the largest absolute value of all phases.
///
/// Values before enough audio was measured are -inf (kSilence). Real-time safe after
/// construction. Thread-safety: one instance per thread.
class LoudnessMeter
{
public:
    static constexpr double kSampleRate = 48000.0;
    static constexpr double kSilence = -200.0; ///< "-inf" LUFS / dBTP

    LoudnessMeter();

    /// Adds @p frames interleaved stereo frames.
    void process(const float* interleaved, int frames);
    /// Forgets everything (integrated loudness and the maximum true peak start again).
    void reset();

    /// Momentary loudness (LUFS, 400 ms window).
    [[nodiscard]] double momentary() const { return m_momentary; }
    /// Short-term loudness (LUFS, 3 s window).
    [[nodiscard]] double shortTerm() const { return m_shortTerm; }
    /// Integrated (gated) loudness since the last reset (LUFS).
    [[nodiscard]] double integrated() const;
    /// Largest true peak since the last reset (dBTP).
    [[nodiscard]] double truePeakMax() const;
    /// Largest true peak of the audio passed to the last process() call (dBTP).
    [[nodiscard]] double truePeakRecent() const;

    /// Loudness (LUFS) of a summed channel mean square.
    [[nodiscard]] static double loudnessOf(double meanSquareSum);

private:
    static constexpr int kBlockSamples = 4800; ///< 100 ms
    static constexpr int kMomentaryBlocks = 4;
    static constexpr int kShortTermBlocks = 30;
    static constexpr int kTaps = 48; ///< true-peak interpolator length (4 phases x 12)
    static constexpr int kPhases = 4;
    static constexpr double kHistogramMin = -70.0; ///< LUFS of the first histogram bin
    static constexpr double kHistogramStep = 0.01; ///< LU per bin
    static constexpr int kHistogramBins = 8000; ///< -70 ... +10 LUFS

    void finishBlock();

    std::array<std::array<double, 2>, 2> m_pre {}, m_rlb {}; ///< K-weighting state per channel
    double m_blockSum = 0.0; ///< sum of squares (both channels) of the running 100 ms block
    int m_blockFill = 0;
    std::array<double, kShortTermBlocks> m_blocks {}; ///< ring of 100 ms block mean squares
    int m_blockCount = 0; ///< blocks ever finished (saturates at the ring size for averages)
    int m_blockHead = 0;
    double m_momentary = kSilence;
    double m_shortTerm = kSilence;
    std::vector<double> m_histEnergy; ///< sum of gating-block mean squares per bin
    std::vector<int64_t> m_histCount;
    std::array<float, kTaps> m_kernel {}; ///< interpolator, kernel[phase + kPhases * k]
    std::array<std::array<float, kTaps / kPhases>, 2> m_history {}; ///< recent input per channel
    int m_historyHead = 0;
    double m_truePeak = 0.0; ///< linear, since reset
    double m_truePeakRecent = 0.0; ///< linear, last process() call
};

} // namespace kk::dsp
