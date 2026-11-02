// SPDX-License-Identifier: GPL-3.0-or-later
// Biquad — second-order IIR sections designed with the RBJ "audio EQ cookbook" formulas
// (high / low pass, shelves, peaking bell, notch), their frequency response and a
// transposed direct form II processor.
#pragma once

#include "core/MixerData.h"

#include <complex>

namespace kk::dsp {

/// Normalised biquad coefficients (a0 = 1):
/// y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1] - a2 y[n-2].
struct BiquadCoeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    /// True for the pass-through section (b0 = 1, everything else 0).
    [[nodiscard]] bool isIdentity() const
    {
        return b0 == 1.0 && b1 == 0.0 && b2 == 0.0 && a1 == 0.0 && a2 == 0.0;
    }
};

/// Designs one EQ band at @p sampleRate (Hz). The frequency is clamped below Nyquist; gain is
/// used by bells and shelves only. RBJ formulas: w0 = 2 pi f / fs, alpha = sin(w0) / (2 Q),
/// A = 10^(gain / 40); shelves use Q as their slope parameter (alpha = sin(w0) / (2 Q) as
/// well, so 0.707 is the steepest slope without overshoot).
[[nodiscard]] BiquadCoeffs designBand(
    EqBandType type, double frequencyHz, double gainDb, double q, double sampleRate);

/// Complex response H(e^jw) of @p c at @p frequencyHz.
[[nodiscard]] std::complex<double> response(
    const BiquadCoeffs& c, double frequencyHz, double sampleRate);
/// Magnitude of the response of @p c at @p frequencyHz in dB.
[[nodiscard]] double magnitudeDb(const BiquadCoeffs& c, double frequencyHz, double sampleRate);

/// Filter memory of one channel (transposed direct form II: two state values). Double
/// precision keeps low-frequency bands stable and quiet.
struct BiquadState
{
    double z1 = 0.0;
    double z2 = 0.0;

    /// Filters one sample.
    double process(const BiquadCoeffs& c, double x)
    {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0.0; }
};

} // namespace kk::dsp
