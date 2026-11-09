// SPDX-License-Identifier: GPL-3.0-or-later
// AudioClipEffects — gain, equalizer, compressor, limiter, high / low-pass, delay and reverb
// processors of a clip's audio effect stack. The EQ and dynamics reuse the mixer's
// dsp::Equalizer and dsp::Dynamics; filters use the RBJ biquads of render/Biquad.h.
#include "render/AudioClipEffects.h"

#include "core/AudioEffectKeys.h"
#include "core/EffectRegistry.h"
#include "render/AudioMixer.h"
#include "render/Biquad.h"
#include "render/Dynamics.h"
#include "render/Equalizer.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace kk::dsp {

namespace {

constexpr double kRate = AudioMixer::kSampleRate;
constexpr double kButterworthQ = 0.7071; // maximally flat 12 dB / octave filters

/// Parameter @p key (a plain value), @p def when the set lacks it, clamped to [lo, hi]
/// (damaged or hand-edited projects must not break the processors).
double param(const ParamSet& p, const char* key, double def, double lo, double hi)
{
    return std::clamp(p.value(QLatin1String(key), 0, def), lo, hi);
}

// ---- Gain ----

class GainEffect final : public ClipEffect
{
public:
    void configure(const ParamSet& p) override
    {
        const float target = AudioMixer::dbToGain(param(p, afxkeys::GainDb, 0, -60, 24));
        if (m_fresh)
            m_gain = target;
        m_target = target;
        m_fresh = false;
    }
    void reset() override { m_fresh = true; }
    void process(float* io, int frames) override
    {
        // A changed gain ramps linearly over the block, so editing it never clicks.
        const float step = (m_target - m_gain) / float(std::max(frames, 1));
        float g = m_gain;
        for (int i = 0; i < frames; ++i, g += step) {
            io[size_t(i) * 2] *= g;
            io[size_t(i) * 2 + 1] *= g;
        }
        m_gain = m_target;
    }

private:
    float m_gain = 1.0f, m_target = 1.0f;
    bool m_fresh = true;
};

// ---- Equalizer: low shelf, bell, high shelf on the mixer's 6-band EQ ----

class EqEffect final : public ClipEffect
{
public:
    EqEffect(double rate = kRate)
        : m_eq(rate)
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        EqSettings s;
        s.enabled = true;
        for (EqBand& b : s.bands)
            b.enabled = false;
        auto setBand = [&](int index, EqBandType type, double hz, double gainDb, double q) {
            s.bands[size_t(index)] = {type, true, hz, gainDb, q};
        };
        setBand(1, EqBandType::LowShelf, param(p, k::LowFreqHz, 100, kEqMinFrequencyHz, 1000),
            param(p, k::LowGainDb, 0, -kEqMaxGainDb, kEqMaxGainDb), kButterworthQ);
        setBand(2, EqBandType::Bell, param(p, k::MidFreqHz, 1000, 100, kEqMaxFrequencyHz),
            param(p, k::MidGainDb, 0, -kEqMaxGainDb, kEqMaxGainDb),
            param(p, k::MidQ, 1, kEqMinQ, kEqMaxQ));
        setBand(4, EqBandType::HighShelf, param(p, k::HighFreqHz, 8000, 1000, kEqMaxFrequencyHz),
            param(p, k::HighGainDb, 0, -kEqMaxGainDb, kEqMaxGainDb), kButterworthQ);
        m_eq.setTarget(s);
    }
    void reset() override { m_eq.reset(); }
    void process(float* io, int frames) override { m_eq.process(io, frames); }

private:
    Equalizer m_eq;
};

// ---- Compressor and limiter on the mixer's dynamics ----

class DynamicsEffect final : public ClipEffect
{
public:
    explicit DynamicsEffect(bool limiter)
        : m_limiter(limiter)
        , m_dyn(kRate)
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        DynamicsSettings s;
        if (m_limiter) {
            s.limiterEnabled = true;
            s.limiterCeilingDb = param(p, k::CeilingDb, -1, kLimiterMinCeilingDb, 0);
            s.limiterReleaseMs = param(p, k::ReleaseMs, 50, 1, kDynMaxReleaseMs);
        } else {
            s.compressorEnabled = true;
            s.thresholdDb = param(p, k::ThresholdDb, -20, kDynMinThresholdDb, 0);
            s.ratio = param(p, k::Ratio, 4, 1, kDynMaxRatio);
            s.attackMs = param(p, k::AttackMs, 5, kDynMinTimeMs, kDynMaxAttackMs);
            s.releaseMs = param(p, k::ReleaseMs, 120, kDynMinTimeMs, kDynMaxReleaseMs);
            s.makeupDb = param(p, k::MakeupDb, 0, 0, kDynMaxMakeupDb);
        }
        m_dyn.setTarget(s);
    }
    void reset() override { m_dyn.reset(); }
    void process(float* io, int frames) override { m_dyn.process(io, frames); }

private:
    bool m_limiter;
    Dynamics m_dyn;
};

// ---- High-pass / low-pass: one 12 dB / octave biquad per channel ----

class FilterEffect final : public ClipEffect
{
public:
    explicit FilterEffect(bool highPass)
        : m_highPass(highPass)
    { }
    void configure(const ParamSet& p) override
    {
        const double hz = m_highPass
            ? param(p, afxkeys::FrequencyHz, 80, kEqMinFrequencyHz, 2000)
            : param(p, afxkeys::FrequencyHz, 12000, 200, kEqMaxFrequencyHz);
        if (hz != m_hz) {
            m_hz = hz;
            m_coeffs = designBand(m_highPass ? EqBandType::HighPass : EqBandType::LowPass, hz, 0.0,
                kButterworthQ, kRate);
        }
    }
    void reset() override
    {
        for (BiquadState& s : m_state)
            s.reset();
    }
    void process(float* io, int frames) override
    {
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < 2; ++c) {
                float& x = io[size_t(i) * 2 + size_t(c)];
                x = float(m_state[size_t(c)].process(m_coeffs, double(x)));
            }
        }
    }

private:
    bool m_highPass;
    double m_hz = 0.0;
    BiquadCoeffs m_coeffs;
    std::array<BiquadState, 2> m_state;
};

// ---- Delay: feedback echo, wet / dry crossfade ----

class DelayEffect final : public ClipEffect
{
public:
    DelayEffect()
        : m_line(size_t(std::ceil(kAudioDelayMaxMs * 1e-3 * kRate)) + 1, 0.0f)
        , m_size(int(m_line.size() / 2))
    { }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        m_delay = std::clamp(
            int(std::lround(param(p, k::TimeMs, 250, 1, kAudioDelayMaxMs) * 1e-3 * kRate)), 1,
            m_size - 1);
        m_feedback = float(param(p, k::Feedback, 35, 0, 95) / 100.0);
        m_mix = float(param(p, k::Mix, 30, 0, 100) / 100.0);
    }
    void reset() override
    {
        std::fill(m_line.begin(), m_line.end(), 0.0f);
        m_write = 0;
    }
    void process(float* io, int frames) override
    {
        for (int i = 0; i < frames; ++i) {
            int read = m_write - m_delay;
            if (read < 0)
                read += m_size;
            for (int c = 0; c < 2; ++c) {
                const float dry = io[size_t(i) * 2 + size_t(c)];
                const float echo = m_line[size_t(read) * 2 + size_t(c)];
                m_line[size_t(m_write) * 2 + size_t(c)] = dry + echo * m_feedback;
                io[size_t(i) * 2 + size_t(c)] = dry * (1.0f - m_mix) + echo * m_mix;
            }
            if (++m_write == m_size)
                m_write = 0;
        }
    }

private:
    std::vector<float> m_line; ///< interleaved stereo ring, m_size frames
    int m_size;
    int m_write = 0;
    int m_delay = 1; ///< frames
    float m_feedback = 0.0f, m_mix = 0.0f;
};

// ---- Reverb: Schroeder / Moorer design (parallel damped combs into series all-passes) ----
// The comb and all-pass lengths are the classic public-domain "Freeverb" set (given for
// 44.1 kHz, scaled to the mixer rate); the right side is lengthened a little for width.

class Comb
{
public:
    void init(int length) { m_buf.assign(size_t(length), 0.0f), m_pos = 0, m_filter = 0.0f; }
    void clear()
    {
        std::fill(m_buf.begin(), m_buf.end(), 0.0f);
        m_filter = 0.0f;
    }
    float process(float in, float feedback, float damp)
    {
        const float out = m_buf[size_t(m_pos)];
        m_filter = out * (1.0f - damp) + m_filter * damp; // low-pass in the feedback path
        m_buf[size_t(m_pos)] = in + m_filter * feedback;
        if (++m_pos == int(m_buf.size()))
            m_pos = 0;
        return out;
    }

private:
    std::vector<float> m_buf;
    int m_pos = 0;
    float m_filter = 0.0f;
};

class AllPass
{
public:
    void init(int length) { m_buf.assign(size_t(length), 0.0f), m_pos = 0; }
    void clear() { std::fill(m_buf.begin(), m_buf.end(), 0.0f); }
    float process(float in)
    {
        constexpr float kFeedback = 0.5f;
        const float buffered = m_buf[size_t(m_pos)];
        m_buf[size_t(m_pos)] = in + buffered * kFeedback;
        if (++m_pos == int(m_buf.size()))
            m_pos = 0;
        return buffered - in;
    }

private:
    std::vector<float> m_buf;
    int m_pos = 0;
};

class ReverbEffect final : public ClipEffect
{
public:
    ReverbEffect()
    {
        constexpr int kCombs[kCombCount] = {1116, 1188, 1277, 1356};
        constexpr int kAllPasses[kAllPassCount] = {556, 441};
        constexpr int kStereoSpread = 23; // samples at 44.1 kHz
        const double scale = kRate / 44100.0;
        for (int side = 0; side < 2; ++side) {
            for (int i = 0; i < kCombCount; ++i)
                m_combs[size_t(side)][size_t(i)].init(
                    int(std::lround((kCombs[i] + side * kStereoSpread) * scale)));
            for (int i = 0; i < kAllPassCount; ++i)
                m_allPasses[size_t(side)][size_t(i)].init(
                    int(std::lround((kAllPasses[i] + side * kStereoSpread) * scale)));
        }
    }
    void configure(const ParamSet& p) override
    {
        namespace k = afxkeys;
        // Freeverb's ranges: feedback 0.70 ... 0.98 with the room size, damping up to 0.4.
        m_feedback = float(0.7 + 0.28 * param(p, k::RoomSize, 50, 0, 100) / 100.0);
        m_damp = float(0.4 * param(p, k::Damping, 50, 0, 100) / 100.0);
        m_mix = float(param(p, k::Mix, 25, 0, 100) / 100.0);
    }
    void reset() override
    {
        for (auto& side : m_combs)
            for (Comb& c : side)
                c.clear();
        for (auto& side : m_allPasses)
            for (AllPass& a : side)
                a.clear();
    }
    void process(float* io, int frames) override
    {
        constexpr float kInputGain = 0.06f; // keeps the comb sum at a sensible level
        constexpr float kWetGain = 3.0f;
        for (int i = 0; i < frames; ++i) {
            float& l = io[size_t(i) * 2];
            float& r = io[size_t(i) * 2 + 1];
            const float in = (l + r) * kInputGain;
            float wet[2] = {0.0f, 0.0f};
            for (size_t side = 0; side < 2; ++side) {
                for (Comb& c : m_combs[side])
                    wet[side] += c.process(in, m_feedback, m_damp);
                for (AllPass& a : m_allPasses[side])
                    wet[side] = a.process(wet[side]);
            }
            l = l * (1.0f - m_mix) + wet[0] * kWetGain * m_mix;
            r = r * (1.0f - m_mix) + wet[1] * kWetGain * m_mix;
        }
    }

private:
    static constexpr int kCombCount = 4;
    static constexpr int kAllPassCount = 2;
    std::array<std::array<Comb, kCombCount>, 2> m_combs;
    std::array<std::array<AllPass, kAllPassCount>, 2> m_allPasses;
    float m_feedback = 0.84f, m_damp = 0.2f, m_mix = 0.25f;
};

std::unique_ptr<ClipEffect> create(const QString& type)
{
    using namespace afxid;
    if (type == QLatin1String(Gain))
        return std::make_unique<GainEffect>();
    if (type == QLatin1String(Eq))
        return std::make_unique<EqEffect>();
    if (type == QLatin1String(Compressor))
        return std::make_unique<DynamicsEffect>(false);
    if (type == QLatin1String(Limiter))
        return std::make_unique<DynamicsEffect>(true);
    if (type == QLatin1String(HighPass))
        return std::make_unique<FilterEffect>(true);
    if (type == QLatin1String(LowPass))
        return std::make_unique<FilterEffect>(false);
    if (type == QLatin1String(Delay))
        return std::make_unique<DelayEffect>();
    if (type == QLatin1String(Reverb))
        return std::make_unique<ReverbEffect>();
    return nullptr;
}

} // namespace

ClipEffectChain::ClipEffectChain() = default;
ClipEffectChain::~ClipEffectChain() = default;

bool ClipEffectChain::hasActive(const QVector<EffectInstance>& effects)
{
    return std::any_of(effects.begin(), effects.end(), [](const EffectInstance& fx) {
        return fx.enabled && fx.type.startsWith(QLatin1String("audio_"));
    });
}

void ClipEffectChain::reset()
{
    for (Slot& s : m_slots) {
        if (s.effect)
            s.effect->reset();
    }
}

bool ClipEffectChain::process(const QVector<EffectInstance>& effects, float* io, int frames)
{
    // Drop the processors of removed effects (or whose type changed).
    std::erase_if(m_slots, [&](const Slot& s) {
        return std::none_of(effects.begin(), effects.end(),
            [&](const EffectInstance& fx) { return fx.id == s.id && fx.type == s.type; });
    });
    bool any = false;
    for (const EffectInstance& fx : effects) {
        auto it = std::find_if(m_slots.begin(), m_slots.end(),
            [&](const Slot& s) { return s.id == fx.id && s.type == fx.type; });
        if (!fx.enabled) {
            if (it != m_slots.end() && it->wasOn) { // bypassed: start clean when switched on
                it->effect->reset();
                it->wasOn = false;
            }
            continue;
        }
        if (it == m_slots.end()) {
            std::unique_ptr<ClipEffect> effect = create(fx.type);
            if (!effect)
                continue; // unknown type: skipped, kept in the project
            m_slots.push_back({fx.id, fx.type, std::move(effect), false});
            it = m_slots.end() - 1;
        }
        it->effect->configure(fx.params);
        it->effect->process(io, frames);
        it->wasOn = true;
        any = true;
    }
    return any;
}

} // namespace kk::dsp
