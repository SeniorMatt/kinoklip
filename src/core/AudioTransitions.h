// SPDX-License-Identifier: GPL-3.0-or-later
// AudioTransitions — the audio transition types (Effects library "Audio Transitions") and their
// gain curves, shared by the mixer, the library icons and the edit code.
#pragma once

#include <QString>

namespace kk::audiotransitions {

/// Shape of an audio crossfade. Every curve rises from 0 to 1 for the incoming clip, and the
/// outgoing clip mirrors it: gainOut(t) = gainIn(1 - t).
enum class Curve {
    Linear, ///< "Cross Fade 0 dB": linear gain, the sum dips about 3 dB at the middle
    Minus3dB, ///< "Cross Fade -3 dB": half linear, half equal power (0.604 at the middle)
    EqualPower, ///< "Cross Fade +3 dB": sin / cos, constant power (0.707 at the middle)
    Exponential, ///< slow start, fast finish for the incoming clip
    Logarithmic, ///< fast start, slow finish for the incoming clip
    SCurve ///< raised cosine: gentle at both ends, 0.5 at the middle
};

/// Descriptor ids (EffectRegistry, category AudioTransition).
inline constexpr char kCrossFade0dB[] = "audio_crossfade_0db";
inline constexpr char kCrossFadePlus3dB[] = "audio_crossfade_3db";
inline constexpr char kCrossFadeMinus3dB[] = "audio_crossfade_minus3db";
inline constexpr char kExponential[] = "audio_crossfade_exponential";
inline constexpr char kLogarithmic[] = "audio_crossfade_logarithmic";
inline constexpr char kSCurve[] = "audio_crossfade_scurve";

/// The default audio transition (Ctrl+T on audio, the Inspector's add buttons): +3 dB.
[[nodiscard]] QString defaultId();

/// Curve of transition type @p typeId. Any other id (a video transition id, e.g. the
/// "cross_dissolve" older projects store on audio clips, or an unknown one) is EqualPower.
[[nodiscard]] Curve curveFor(const QString& typeId);

/// True when @p typeId is one of the audio transition ids above.
[[nodiscard]] bool isAudioTransition(const QString& typeId);

/// Gain of the incoming clip at progress @p t (clamped to [0, 1]): 0 at t = 0, 1 at t = 1.
[[nodiscard]] double gainIn(Curve curve, double t);
/// Gain of the outgoing clip at progress @p t (clamped to [0, 1]): 1 at t = 0, 0 at t = 1.
[[nodiscard]] double gainOut(Curve curve, double t);

} // namespace kk::audiotransitions
