// SPDX-License-Identifier: GPL-3.0-or-later
// Dynamics — the real-time compressor and limiter of a mixer channel.
#pragma once

#include "core/MixerData.h"

namespace kk::dsp {

/// Static curve of the compressor: gain reduction (dB, >= 0) for an input level @p levelDb
/// with threshold @p thresholdDb, @p ratio and soft knee width @p kneeDb (0 = hard knee;
/// the quadratic knee of Giannoulis, Massberg and Reiss, "Digital dynamic range compressor
/// design", JAES 2012).
[[nodiscard]] double compressorGainReductionDb(
    double levelDb, double thresholdDb, double ratio, double kneeDb);

/// Feed-forward stereo-linked compressor followed by a peak limiter, on interleaved stereo.
///
/// Compressor: the detector is the larger absolute sample of both channels in dB; the static
/// curve gives the wanted gain reduction, which a branching one-pole smoother follows with
/// the attack time constant while it rises and the release time constant while it falls (in
/// the dB domain, so the timing does not depend on the level: after a step the reduction
/// reaches 1 - 1/e = 63 % of its final value in one attack time). Make-up gain is added
/// after it. Limiter: instantaneous attack (the reduction needed by the current sample is
/// applied to that sample, so no sample exceeds the ceiling) and a one-pole release.
/// Switching a section on or off, the make-up gain and the ceiling are smoothed (~10 ms).
///
/// Real-time safe: no allocations. Thread-safety: one instance per thread.
class Dynamics
{
public:
    static constexpr int kChannels = 2;

    /// Sets the sample rate (Hz).
    explicit Dynamics(double sampleRate = 48000.0);

    /// New target settings (cheap; call before every process()).
    void setTarget(const DynamicsSettings& settings);
    /// Clears the envelopes and jumps to the target (after a seek).
    void reset();
    /// Processes @p frames interleaved stereo frames in place.
    void process(float* interleaved, int frames);
    /// True if the processor currently changes the signal.
    [[nodiscard]] bool active() const;
    /// Current total gain reduction in dB (compressor + limiter, >= 0; for the meters).
    [[nodiscard]] float gainReductionDb() const { return float(m_compGr + m_limGr); }

private:
    double coefficient(double ms) const;

    double m_sampleRate;
    DynamicsSettings m_target;
    double m_compGr = 0.0; ///< current compressor reduction (dB)
    double m_limGr = 0.0; ///< current limiter reduction (dB)
    double m_compWet = 0.0; ///< compressor on / off crossfade
    double m_limWet = 0.0;
    double m_makeupDb = 0.0; ///< smoothed make-up gain
    double m_ceilingDb = -1.0; ///< smoothed limiter ceiling
    double m_attack = 0.0, m_release = 0.0, m_limRelease = 0.0; ///< one-pole coefficients
    double m_smooth = 0.0; ///< per-sample smoothing coefficient (~10 ms)
    bool m_fresh = true;
};

} // namespace kk::dsp
