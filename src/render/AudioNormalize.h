// SPDX-License-Identifier: GPL-3.0-or-later
// AudioNormalize — measuring clips for Clip > Normalize Audio Levels (sample peak, true peak,
// BS.1770 integrated loudness) and turning the measurements into clip gains (spec 37.35).
#pragma once

#include "core/ClipAttributes.h"

#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace kk::audionorm {

/// What the target level measures (Resolve's normalization modes).
enum class Mode {
    SamplePeak, ///< Sample Peak Program: the largest sample, dBFS
    TruePeak, ///< the largest inter-sample peak (4x oversampled, BS.1770 Annex 2), dBTP
    Loudness ///< ITU-R BS.1770-4 integrated (gated) loudness, LUFS
};

/// How several clips are levelled.
enum class Scope {
    Relative, ///< one common gain: the loudest clip reaches the target, the others keep
              ///< their level relative to it
    Independent ///< every clip gets its own gain and reaches the target
};

/// Default target level of @p mode (dBFS / dBTP / LUFS): -9 dBFS sample peak, -2 dBTP true
/// peak, -23 LUFS loudness (EBU R128).
[[nodiscard]] double defaultTarget(Mode mode);

/// The audio of one clip to measure: a media file and its source range.
struct Source
{
    QString path;
    int64_t startUs = 0; ///< first source microsecond of the clip
    int64_t endUs = 0; ///< end of the clip's source range (exclusive)
    ClipAttributes attributes; ///< channel routing (Clip Attributes)
};

/// Levels of one clip's source audio (before the clip's own volume).
struct Measurement
{
    bool valid = false; ///< false: unreadable file, no audio, or cancelled
    double samplePeakDb = -200.0; ///< dBFS
    double truePeakDb = -200.0; ///< dBTP
    double loudnessLufs = -200.0; ///< integrated, LUFS (-200 = silence / too short)
    QString error;
};

/// Level of @p m in @p mode's unit.
[[nodiscard]] double levelOf(const Measurement& m, Mode mode);

/// Measures @p source: decodes its range at 48 kHz stereo (media::AudioDecoder) in blocks,
/// feeding dsp::LoudnessMeter (true peak, integrated loudness) and a plain sample peak.
/// @p cancel (may be null) is polled between blocks; @p progress (may be empty) receives the
/// fraction done of this clip. Runs on any thread (one decoder per call).
[[nodiscard]] Measurement measure(const Source& source, const std::atomic<bool>* cancel = nullptr,
    const std::function<void(double)>& progress = {});

/// The clip gains (dB) that bring @p levels (in the mode's unit, one per clip) to @p target.
/// Relative: target - max(levels) for every clip; Independent: target - level per clip.
/// Entries with a silent level (<= -150, nothing to normalize) get NaN (left unchanged).
[[nodiscard]] std::vector<double> gains(
    const std::vector<double>& levels, double target, Scope scope);

} // namespace kk::audionorm
