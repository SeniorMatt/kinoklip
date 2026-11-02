// SPDX-License-Identifier: GPL-3.0-or-later
// ChannelProcessor — inserts and smoothing constants.
#include "render/ChannelProcessor.h"

namespace kk::dsp {

namespace {
constexpr double kSampleRate = 48000.0;
/// Glide time constant of the fader / pan / mute gains.
constexpr double kGainSmoothSeconds = 0.005;
} // namespace

ChannelProcessor::ChannelProcessor()
    : m_eq(kSampleRate)
    , m_dynamics(kSampleRate)
    , m_alpha(float(1.0 - std::exp(-double(kGainBlock) / (kGainSmoothSeconds * kSampleRate))))
{ }

void ChannelProcessor::setStrip(const ChannelStrip& strip)
{
    m_eq.setTarget(strip.eq);
    m_dynamics.setTarget(strip.dynamics);
}

void ChannelProcessor::reset()
{
    m_eq.reset();
    m_dynamics.reset();
    m_fresh = true;
}

void ChannelProcessor::resetInserts()
{
    m_eq.reset();
    m_dynamics.reset();
}

void ChannelProcessor::processInserts(float* io, int frames, bool mono)
{
    if (mono) {
        for (int i = 0; i < frames; ++i) {
            const float m = 0.5f * (io[size_t(i) * 2] + io[size_t(i) * 2 + 1]);
            io[size_t(i) * 2] = m;
            io[size_t(i) * 2 + 1] = m;
        }
    }
    m_eq.process(io, frames);
    m_dynamics.process(io, frames);
}

} // namespace kk::dsp
