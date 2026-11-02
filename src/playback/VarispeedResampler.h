// SPDX-License-Identifier: GPL-3.0-or-later
// VarispeedResampler — band-limited (windowed-sinc) resampling of a mixed audio span for
// shuttle, reverse and slow play (spec 1.16).
#pragma once

#include <array>

namespace kk {

/// Reads a block of interleaved stereo frames at fractional source positions with a
/// Kaiser-windowed sinc kernel whose cutoff follows the speed.
///
/// At |rate| > 1 the source is decimated: the kernel is stretched by |rate| so its cutoff sits
/// just below the output Nyquist frequency, which removes what would alias (a linear
/// interpolator folds everything above 24 kHz / |rate| back into the audible band, heard as
/// a metallic hiss at 4x - 8x). At |rate| <= 1 the kernel is a plain band-limited interpolator.
///
/// Performance: the kernel is a precomputed table (kTableSteps entries per zero crossing,
/// linearly interpolated), so a tap costs two loads and a multiply-add; there are
/// 2 * kZeroCrossings * max(1, |rate|) taps per output frame and channel (256 at 8x).
/// process() allocates nothing and touches no shared state: real-time safe.
/// Thread-safety: a const object may be used from any thread.
class VarispeedResampler
{
public:
    /// Half-width of the kernel in output samples (zero crossings on each side).
    static constexpr int kZeroCrossings = 8;
    /// Table resolution: kernel values per output sample of distance.
    static constexpr int kTableSteps = 512;
    /// Cutoff as a fraction of the output Nyquist frequency (room for the transition band).
    static constexpr double kCutoff = 0.85;
    /// Kaiser window beta: about 70 dB stop band; with 16 output samples of kernel the
    /// transition band is about 0.4 of the output Nyquist frequency wide around kCutoff.
    static constexpr double kKaiserBeta = 7.0;
    /// Interleaved channels of the span and the output (AudioRing::kChannels).
    static constexpr int kChannels = 2;

    VarispeedResampler();

    /// Source frames the kernel needs on each side of a position at @p rate (round up).
    [[nodiscard]] static int margin(double rate);

    /// Writes @p count output frames into @p out: frame k is the band-limited value of
    /// @p span at the fractional frame position `pos0 + rate * k` (positions relative to the
    /// span start). Taps outside [0, spanFrames) read as silence. Weights are normalised per
    /// output frame, so DC and slow ramps pass with unity gain.
    void process(
        const float* span, int spanFrames, double pos0, double rate, int count, float* out) const;

private:
    /// Kernel value at distance @p t >= 0 output samples (0 beyond kZeroCrossings).
    [[nodiscard]] float kernel(double t) const;

    std::array<float, kZeroCrossings * kTableSteps + 2> m_table {};
};

} // namespace kk
