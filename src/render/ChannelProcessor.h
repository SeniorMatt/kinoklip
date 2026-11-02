// SPDX-License-Identifier: GPL-3.0-or-later
// ChannelProcessor — the DSP of one mixer channel (track or bus): channel format, EQ,
// dynamics, then smoothed pan and fader gains, with peak metering.
#pragma once

#include "core/MixerData.h"
#include "render/Dynamics.h"
#include "render/Equalizer.h"

#include <algorithm>
#include <cmath>

namespace kk::dsp {

/// Gains a channel wants at one moment: fader gain (linear, mute included) and the pan
/// gains of the constant-power pan law (render/AudioMixer.h panGains(); 1, 1 = centre).
struct ChannelGains
{
    float gain = 1.0f;
    float left = 1.0f;
    float right = 1.0f;
};

/// Processes one mixer channel in place on interleaved stereo, in signal flow order:
/// mono fold (mono channels: (L + R) / 2 on both sides) -> EQ -> dynamics -> pan -> fader.
///
/// The pan / fader gains are evaluated by the caller at the edges of kGainBlock-sample
/// blocks (automation curves, mute, solo) and smoothed with a ~5 ms one-pole glide, then
/// ramped linearly inside each block, so fader moves, mutes and automation never click.
/// After reset() the glide starts at the first target (no fade-in after a seek).
///
/// Real-time safe: no allocations. Thread-safety: one instance per thread.
class ChannelProcessor
{
public:
    static constexpr int kGainBlock = 64; ///< samples between gain evaluations
    static constexpr int kChannels = 2;

    ChannelProcessor();

    /// New EQ / dynamics settings (cheap; call before every block).
    void setStrip(const ChannelStrip& strip);
    /// Clears filter memory and envelopes; the next gains are taken as they are.
    void reset();
    /// Clears filter memory and envelopes but keeps the current gains (a channel that went
    /// quiet: it glides from where it was when it sounds again).
    void resetInserts();

    /// Mono fold, EQ and dynamics over @p frames frames of @p io.
    void processInserts(float* io, int frames, bool mono);

    /// Applies the smoothed gains: @p gainsAt(offset) gives the wanted gains at block edge
    /// @p offset (0 ... frames). Records the post-fader peaks (peak()).
    template<class GainsAt>
    void applyGains(float* io, int frames, GainsAt&& gainsAt)
    {
        m_peak[0] = m_peak[1] = 0.0f;
        if (m_fresh) {
            m_current = gainsAt(0);
            m_fresh = false;
        }
        for (int b0 = 0; b0 < frames; b0 += kGainBlock) {
            const int b1 = std::min(frames, b0 + kGainBlock);
            const ChannelGains want = gainsAt(b1);
            ChannelGains next;
            next.gain = glide(m_current.gain, want.gain);
            next.left = glide(m_current.left, want.left);
            next.right = glide(m_current.right, want.right);
            const float inv = 1.0f / float(b1 - b0);
            const float dl = (next.gain * next.left - m_current.gain * m_current.left) * inv;
            const float dr = (next.gain * next.right - m_current.gain * m_current.right) * inv;
            float gl = m_current.gain * m_current.left;
            float gr = m_current.gain * m_current.right;
            for (int i = b0; i < b1; ++i, gl += dl, gr += dr) {
                float& l = io[size_t(i) * 2];
                float& r = io[size_t(i) * 2 + 1];
                l *= gl;
                r *= gr;
                m_peak[0] = std::max(m_peak[0], std::abs(l));
                m_peak[1] = std::max(m_peak[1], std::abs(r));
            }
            m_current = next;
        }
    }

    /// Post-fader sample peaks of the last applyGains() (linear, per side).
    [[nodiscard]] float peak(int side) const { return m_peak[side]; }
    /// Fader gain reached by the last applyGains() (linear; 0 = muted or silent fader).
    [[nodiscard]] float currentGain() const { return m_fresh ? 1.0f : m_current.gain; }
    /// Dynamics gain reduction now (dB, >= 0).
    [[nodiscard]] float gainReductionDb() const { return m_dynamics.gainReductionDb(); }
    /// True while EQ or dynamics still hold energy or change the signal (tails).
    [[nodiscard]] bool insertsActive() const { return m_eq.active() || m_dynamics.active(); }

private:
    float glide(float from, float to) const
    {
        const float d = to - from;
        return std::abs(d) < kSnap ? to : from + d * m_alpha;
    }
    static constexpr float kSnap = 1e-6f;

    Equalizer m_eq;
    Dynamics m_dynamics;
    ChannelGains m_current;
    float m_alpha; ///< glide step per kGainBlock
    float m_peak[2] = {0.0f, 0.0f};
    bool m_fresh = true;
};

} // namespace kk::dsp
