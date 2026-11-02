// SPDX-License-Identifier: GPL-3.0-or-later
// Equalizer — smoothed 6-band biquad EQ.
#include "render/Equalizer.h"

#include <algorithm>
#include <cmath>

namespace kk::dsp {

namespace {
/// Smoothing time constant of every EQ parameter and of the on / off crossfades.
constexpr double kSmoothSeconds = 0.010;
/// Below these differences a gliding value snaps to its target.
constexpr double kSnapLogFreq = 1e-4; // octaves
constexpr double kSnapGainDb = 1e-3;
constexpr double kSnapQ = 1e-4;
constexpr double kSnapWet = 1e-4;

bool glide(double& value, double target, double alpha, double snap)
{
    const double d = target - value;
    if (std::abs(d) <= snap) {
        value = target;
        return true;
    }
    value += d * alpha;
    return false;
}
} // namespace

Equalizer::Equalizer(double sampleRate)
    : m_sampleRate(sampleRate)
    , m_alpha(1.0 - std::exp(-double(kSmoothBlock) / (kSmoothSeconds * sampleRate)))
{
    reset();
}

void Equalizer::setTarget(const EqSettings& settings)
{
    m_wetTarget = settings.enabled ? 1.0 : 0.0;
    for (int i = 0; i < kEqBandCount; ++i) {
        Band& b = m_bands[size_t(i)];
        const EqBand& t = settings.bands[size_t(i)];
        if (b.target == t)
            continue;
        const bool typeChanged = b.target.type != t.type;
        b.target = t;
        b.settled = false;
        if (typeChanged || m_fresh) {
            // A new shape cannot glide from the old one: jump (the memory stays, so the
            // change is a step in the response, not a click from cleared state).
            b.logFreq = std::log2(t.frequencyHz);
            b.gain = t.gainDb;
            b.q = t.q;
            design(b);
        }
    }
    if (m_fresh) {
        m_fresh = false;
        reset();
    }
}

void Equalizer::reset()
{
    m_wet = m_wetTarget;
    for (Band& b : m_bands) {
        b.logFreq = std::log2(std::max(b.target.frequencyHz, 1.0));
        b.gain = b.target.gainDb;
        b.q = b.target.q;
        b.wet = b.target.enabled ? 1.0 : 0.0;
        for (BiquadState& s : b.state)
            s.reset();
        design(b);
        b.settled = true;
    }
}

void Equalizer::design(Band& b)
{
    b.coeffs = designBand(b.target.type, std::exp2(b.logFreq), b.gain, b.q, m_sampleRate);
}

void Equalizer::stepSmoothing()
{
    glide(m_wet, m_wetTarget, m_alpha, kSnapWet);
    for (Band& b : m_bands) {
        glide(b.wet, b.target.enabled ? 1.0 : 0.0, m_alpha, kSnapWet);
        if (b.settled)
            continue;
        const bool f = glide(b.logFreq, std::log2(b.target.frequencyHz), m_alpha, kSnapLogFreq);
        const bool g = glide(b.gain, b.target.gainDb, m_alpha, kSnapGainDb);
        const bool q = glide(b.q, b.target.q, m_alpha, kSnapQ);
        b.settled = f && g && q;
        design(b);
    }
}

void Equalizer::process(float* io, int frames)
{
    if (m_wet == 0.0 && m_wetTarget == 0.0)
        return; // bypassed (memory was cleared when it faded out)
    for (int b0 = 0; b0 < frames; b0 += kSmoothBlock) {
        stepSmoothing();
        const int b1 = std::min(frames, b0 + kSmoothBlock);
        for (Band& band : m_bands) {
            // The EQ switch and the band switch both crossfade: effective wet = product.
            const double w = band.wet * m_wet;
            if (w == 0.0) {
                if (!band.target.enabled) // faded out: start clean when switched on again
                    for (BiquadState& st : band.state)
                        st.reset();
                continue;
            }
            if (band.coeffs.isIdentity())
                continue;
            const BiquadCoeffs c = band.coeffs;
            for (int ch = 0; ch < kChannels; ++ch) {
                BiquadState s = band.state[size_t(ch)];
                for (int i = b0; i < b1; ++i) {
                    float& v = io[size_t(i) * kChannels + size_t(ch)];
                    const double x = v;
                    const double y = s.process(c, x);
                    v = float(w == 1.0 ? y : x + w * (y - x));
                }
                band.state[size_t(ch)] = s;
            }
        }
    }
    if (m_wet == 0.0 && m_wetTarget == 0.0) {
        for (Band& band : m_bands) {
            for (BiquadState& s : band.state)
                s.reset();
        }
    }
}

} // namespace kk::dsp
