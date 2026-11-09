// SPDX-License-Identifier: GPL-3.0-or-later
// AudioEffectKeys — descriptor ids, parameter keys and limits of the clip audio effects
// (EffectCategory::AudioEffect). Shared by the catalogue (core/BuiltinAudioEffects.cpp) and
// the processors (render/AudioClipEffects.cpp).
#pragma once

namespace kk {

/// Descriptor ids (stable: stored in project files).
namespace afxid {
inline constexpr char Gain[] = "audio_gain";
inline constexpr char Eq[] = "audio_eq";
inline constexpr char Compressor[] = "audio_compressor";
inline constexpr char Limiter[] = "audio_limiter";
inline constexpr char HighPass[] = "audio_highpass";
inline constexpr char LowPass[] = "audio_lowpass";
inline constexpr char Delay[] = "audio_delay";
inline constexpr char Reverb[] = "audio_reverb";
} // namespace afxid

/// Parameter keys (stored in EffectInstance::params). All audio effect parameters are plain
/// values: they are not keyframable.
namespace afxkeys {
inline constexpr char GainDb[] = "gain_db";
// Equalizer: low shelf, one bell, high shelf.
inline constexpr char LowGainDb[] = "low_gain_db";
inline constexpr char LowFreqHz[] = "low_freq_hz";
inline constexpr char MidGainDb[] = "mid_gain_db";
inline constexpr char MidFreqHz[] = "mid_freq_hz";
inline constexpr char MidQ[] = "mid_q";
inline constexpr char HighGainDb[] = "high_gain_db";
inline constexpr char HighFreqHz[] = "high_freq_hz";
// Compressor.
inline constexpr char ThresholdDb[] = "threshold_db";
inline constexpr char Ratio[] = "ratio";
inline constexpr char AttackMs[] = "attack_ms";
inline constexpr char ReleaseMs[] = "release_ms";
inline constexpr char MakeupDb[] = "makeup_db";
// Limiter.
inline constexpr char CeilingDb[] = "ceiling_db";
// High-pass / low-pass.
inline constexpr char FrequencyHz[] = "frequency_hz";
// Delay.
inline constexpr char TimeMs[] = "time_ms";
inline constexpr char Feedback[] = "feedback"; ///< percent
inline constexpr char Mix[] = "mix"; ///< percent wet (delay and reverb)
// Reverb.
inline constexpr char RoomSize[] = "room_size"; ///< percent
inline constexpr char Damping[] = "damping"; ///< percent
} // namespace afxkeys

/// Longest delay time (ms); the processor's buffer is sized for it.
inline constexpr double kAudioDelayMaxMs = 2000.0;

} // namespace kk
