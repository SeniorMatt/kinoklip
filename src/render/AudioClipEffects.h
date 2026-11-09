// SPDX-License-Identifier: GPL-3.0-or-later
// AudioClipEffects — the processors of a clip's audio effect stack (ClipData::effects of an
// audio clip), run in list order on interleaved 48 kHz stereo float.
#pragma once

#include "core/TimelineData.h"

#include <memory>
#include <vector>

namespace kk::dsp {

/// One audio effect: reads its parameters, then filters a block in place.
class ClipEffect
{
public:
    virtual ~ClipEffect() = default;
    /// Takes the instance's parameters (cheap; called before every block). The first call
    /// after construction or reset() jumps to the values, later calls glide where needed.
    virtual void configure(const ParamSet& params) = 0;
    /// Clears the memory (filter state, delay lines, envelopes).
    virtual void reset() = 0;
    /// Processes @p frames interleaved stereo frames in place.
    virtual void process(float* io, int frames) = 0;
};

/// The effect stack of one clip: keeps one processor (and its memory) per EffectInstance and
/// runs the enabled ones in list order. A bypassed effect is skipped, so it costs nothing and
/// leaves the signal untouched; its memory is cleared, so switching it on starts clean.
///
/// Used by the mixer for playback and export alike (AudioMixer::mixClip), so both give the
/// same samples. The mixer calls reset() when a clip is not read contiguously (seek, loop,
/// new export), so a range always sounds the same from the same start.
///
/// Real-time safe in steady state (processors are created when an effect first appears).
/// Thread-safety: one instance per thread.
class ClipEffectChain
{
public:
    ClipEffectChain();
    ~ClipEffectChain();
    ClipEffectChain(const ClipEffectChain&) = delete;
    ClipEffectChain& operator=(const ClipEffectChain&) = delete;

    /// Processes @p frames frames of @p io through the enabled effects of @p effects.
    /// Returns false (and leaves @p io alone) if none is enabled and known.
    bool process(const QVector<EffectInstance>& effects, float* io, int frames);
    /// Clears the memory of every effect.
    void reset();

    /// True if @p effects has an enabled effect of a known audio type.
    [[nodiscard]] static bool hasActive(const QVector<EffectInstance>& effects);

private:
    struct Slot
    {
        quint64 id = 0;
        QString type;
        std::unique_ptr<ClipEffect> effect;
        bool wasOn = false; ///< ran in the last block
    };
    std::vector<Slot> m_slots;
};

} // namespace kk::dsp
