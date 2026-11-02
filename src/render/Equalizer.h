// SPDX-License-Identifier: GPL-3.0-or-later
// Equalizer — the real-time 6-band parametric EQ of a mixer channel (render/Biquad.h bands)
// with parameter smoothing.
#pragma once

#include "core/MixerData.h"
#include "render/Biquad.h"

#include <array>

namespace kk::dsp {

/// Processes interleaved stereo float audio through the bands of an EqSettings.
///
/// Smoothing (no zipper noise): every band glides its frequency (in octaves), gain and Q
/// towards the target with a ~10 ms time constant, recomputing its coefficients once per
/// kSmoothBlock samples while it moves; switching a band or the whole EQ on / off crossfades
/// its output with the input over the same time. Coefficients are only recomputed while a
/// value moves, so a static EQ costs 6 biquads per channel and sample.
///
/// Real-time safe: no allocations, no locks. Thread-safety: one instance per thread.
class Equalizer
{
public:
    static constexpr int kChannels = 2;
    static constexpr int kSmoothBlock = 32; ///< samples between coefficient updates

    /// Sets the sample rate (Hz) and resets.
    explicit Equalizer(double sampleRate = 48000.0);

    /// New target settings (cheap; call before every process()).
    void setTarget(const EqSettings& settings);
    /// Clears the filter memory and jumps to the target (after a seek).
    void reset();
    /// Filters @p frames interleaved stereo frames in place.
    void process(float* interleaved, int frames);
    /// True if the EQ currently changes the signal (enabled, or still fading out).
    [[nodiscard]] bool active() const { return m_wet > 0.0 || m_wetTarget > 0.0; }

private:
    struct Band
    {
        EqBand target;
        double logFreq = 0.0, gain = 0.0, q = 1.0; ///< current (smoothed) values
        double wet = 0.0; ///< 0 = bypassed, 1 = fully in
        BiquadCoeffs coeffs;
        std::array<BiquadState, kChannels> state;
        bool settled = false; ///< current == target (no recompute needed)
    };
    void stepSmoothing();
    void design(Band& band);

    double m_sampleRate;
    double m_alpha; ///< smoothing step per kSmoothBlock
    std::array<Band, kEqBandCount> m_bands;
    double m_wet = 0.0;
    double m_wetTarget = 0.0;
    bool m_fresh = true; ///< no target seen yet: the first setTarget() jumps
};

} // namespace kk::dsp
