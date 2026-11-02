// SPDX-License-Identifier: GPL-3.0-or-later
// Dynamics — compressor (branching dB-domain envelope) and peak limiter.
#include "render/Dynamics.h"

#include <algorithm>
#include <cmath>

namespace kk::dsp {

namespace {
/// Smoothing time of the on / off switches, make-up gain and ceiling.
constexpr double kSmoothSeconds = 0.010;
/// Detector floor: -180 dBFS (silence would be -inf).
constexpr double kFloor = 1e-9;
constexpr double kSnap = 1e-6;
} // namespace

double compressorGainReductionDb(double levelDb, double thresholdDb, double ratio, double kneeDb)
{
    const double slope = 1.0 - 1.0 / std::max(ratio, 1.0);
    const double over = levelDb - thresholdDb;
    if (kneeDb > 0.0) {
        if (2.0 * over < -kneeDb)
            return 0.0;
        if (2.0 * std::abs(over) <= kneeDb) {
            const double x = over + kneeDb / 2.0;
            return slope * x * x / (2.0 * kneeDb);
        }
    }
    return over > 0.0 ? slope * over : 0.0;
}

Dynamics::Dynamics(double sampleRate)
    : m_sampleRate(sampleRate)
    , m_smooth(1.0 - std::exp(-1.0 / (kSmoothSeconds * sampleRate)))
{
    // Coefficients of the defaults; the first setTarget() still jumps (m_fresh).
    m_attack = coefficient(m_target.attackMs);
    m_release = coefficient(m_target.releaseMs);
    m_limRelease = coefficient(m_target.limiterReleaseMs);
    reset();
}

double Dynamics::coefficient(double ms) const
{
    return std::exp(-1.0 / (std::max(ms, kDynMinTimeMs) * 1e-3 * m_sampleRate));
}

void Dynamics::setTarget(const DynamicsSettings& s)
{
    if (!(s == m_target) || m_fresh) {
        m_target = s;
        m_attack = coefficient(s.attackMs);
        m_release = coefficient(s.releaseMs);
        m_limRelease = coefficient(s.limiterReleaseMs);
    }
    if (m_fresh) {
        m_fresh = false;
        reset();
    }
}

void Dynamics::reset()
{
    m_compGr = 0.0;
    m_limGr = 0.0;
    m_compWet = m_target.compressorEnabled ? 1.0 : 0.0;
    m_limWet = m_target.limiterEnabled ? 1.0 : 0.0;
    m_makeupDb = m_target.makeupDb;
    m_ceilingDb = m_target.limiterCeilingDb;
}

bool Dynamics::active() const
{
    return m_target.compressorEnabled || m_target.limiterEnabled || m_compWet > 0.0
        || m_limWet > 0.0;
}

void Dynamics::process(float* io, int frames)
{
    if (!active())
        return;
    const DynamicsSettings& t = m_target;
    const double compTarget = t.compressorEnabled ? 1.0 : 0.0;
    const double limTarget = t.limiterEnabled ? 1.0 : 0.0;
    for (int i = 0; i < frames; ++i) {
        float& l = io[size_t(i) * 2];
        float& r = io[size_t(i) * 2 + 1];
        // Switches, make-up and ceiling glide (per sample: cheap next to the log / exp).
        m_compWet += (compTarget - m_compWet) * m_smooth;
        m_limWet += (limTarget - m_limWet) * m_smooth;
        m_makeupDb += (t.makeupDb - m_makeupDb) * m_smooth;
        m_ceilingDb += (t.limiterCeilingDb - m_ceilingDb) * m_smooth;
        const double peak = std::max(std::abs(double(l)), std::abs(double(r)));
        const double levelDb = 20.0 * std::log10(std::max(peak, kFloor));

        double gainDb = 0.0;
        if (m_compWet > kSnap) {
            const double want
                = compressorGainReductionDb(levelDb, t.thresholdDb, t.ratio, t.kneeDb);
            const double k = want > m_compGr ? m_attack : m_release;
            m_compGr = want + (m_compGr - want) * k;
            gainDb = m_compWet * (m_makeupDb - m_compGr);
        } else {
            m_compGr = 0.0;
        }
        if (m_limWet > kSnap) {
            const double need = std::max(0.0, levelDb + gainDb - m_ceilingDb);
            m_limGr = need >= m_limGr ? need : need + (m_limGr - need) * m_limRelease;
            gainDb -= m_limWet * m_limGr;
        } else {
            m_limGr = 0.0;
        }
        if (gainDb != 0.0) {
            const float g = float(std::pow(10.0, gainDb / 20.0));
            l *= g;
            r *= g;
        }
    }
    if (compTarget == 0.0 && m_compWet < kSnap)
        m_compWet = 0.0;
    if (limTarget == 0.0 && m_limWet < kSnap)
        m_limWet = 0.0;
}

} // namespace kk::dsp
