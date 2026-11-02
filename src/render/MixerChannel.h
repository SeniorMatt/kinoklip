// SPDX-License-Identifier: GPL-3.0-or-later
// MixerChannel — internal: the per-track / per-bus processing state of an AudioMixer
// (included by AudioMixer.cpp and AudioMixerGraph.cpp only).
#pragma once

#include "render/AudioMixer.h"
#include "render/ChannelProcessor.h"

namespace kk {

/// DSP state of one mixer channel (a track or a bus) between mix calls.
struct AudioMixer::Channel
{
    dsp::ChannelProcessor processor;
    /// No sound and no filter / envelope tail: the channel's processing is skipped until a
    /// clip plays on it again.
    bool quiet = true;
    qint64 lastUsedMs = 0; ///< for collectIdle() (deleted tracks / buses)
};

} // namespace kk
