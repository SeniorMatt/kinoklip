// SPDX-License-Identifier: GPL-3.0-or-later
// Retime — the speed model of a clip: a constant speed or a retime curve (source position over
// clip-local timeline frames), its exact evaluation, speed segments and the source it uses.
#pragma once

#include "core/TimelineData.h"

#include <QString>
#include <QVector>

#include <cstdint>

namespace kk::retime {

// ---- Model ----
//
// A clip maps clip-local timeline frame x (x = frame - clip.start, fractional for audio) to a
// source position R(x), measured in *source frames at the timeline frame rate* ("retime
// frames": at 100 % R grows by 1 per frame, so the curve editor shows a 45 degree line). Only
// differences of R matter: the source time shown at x is
//
//     sourceTimeUs(x) = clip.sourceInUs + (R(x) - R(0)) * frameUs          (frameUs = 1e6/fps)
//
// so clip.sourceInUs stays "the source time shown at the clip's first frame" (PLAN 5.1) and
// trims / splits / slips keep working by moving sourceInUs and shifting keys like every other
// keyframe.
//
// * Constant speed (no curve): R(x) = x * clip.speed. Negative = reverse, 0 = freeze frame.
// * Retime curve: clip.retime.values[keys::RetimeFrame] with >= 2 keys. Its keys are the
//   *speed points*; between two keys R follows the keyframe Bezier (core/Animation.h): Linear
//   handles = a constant speed segment, Smooth / Ease / Free handles = a speed ramp. Outside
//   the first / last key R continues as a straight line with the chord slope of the first /
//   last segment, so trimming a clip longer keeps the edge segment's speed.
// * Segment speed = (R(b) - R(a)) / (b - a) (the average over the segment for ramps).
// * The curve may decrease (reverse segment) or stay flat (freeze segment): the clip's
//   content is continuous at every speed point (a reverse segment "bounces" back).
//
// Retime operations (core/RetimeOps.h) keep keys at the clip's first frame (0) and at its end
// (duration) as segment boundaries; the speed points the UI shows are the keys strictly inside
// the clip.

/// The retime curve of @p clip, or nullptr when the clip plays at its constant `speed` (no
/// keys::RetimeFrame value or fewer than two keys).
[[nodiscard]] const AnimatedValue* curve(const ClipData& clip);
/// True if @p clip has a retime curve (see curve()).
[[nodiscard]] inline bool hasCurve(const ClipData& clip)
{
    return curve(clip) != nullptr;
}
/// True if the clip does not play at a constant 100 % (curve, other speed, reverse, freeze).
[[nodiscard]] bool isRetimed(const ClipData& clip);

/// R(x): source position in retime frames at clip-local (fractional) frame @p localFrame.
/// Allocation-free; binary search over the keys.
[[nodiscard]] double position(const ClipData& clip, double localFrame);
/// Instantaneous speed dR/dx at @p localFrame (1 = 100 %), central difference of position().
[[nodiscard]] double speedAt(const ClipData& clip, double localFrame);
/// Duration of one timeline frame in µs (0 for an invalid rate).
[[nodiscard]] double frameUs(Rational fps);
/// Source µs between clip-local frames @p fromLocal and @p toLocal (signed).
[[nodiscard]] int64_t sourceOffsetUs(
    const ClipData& clip, double fromLocal, double toLocal, Rational fps);
/// Source time (µs, unclamped) shown at clip-local frame @p localFrame. For a clip without a
/// curve and an integer frame this is exactly the PLAN 5.1 formula (no new rounding).
[[nodiscard]] int64_t sourceTimeUs(const ClipData& clip, double localFrame, Rational fps);

/// Smallest and largest source time (µs) shown by the clip-local frames [@p fromLocal,
/// @p toLocal) (unclamped; empty range: both = the time at @p fromLocal).
struct SourceRange
{
    int64_t minUs = 0;
    int64_t maxUs = 0;
};
[[nodiscard]] SourceRange shownSourceRange(
    const ClipData& clip, int64_t fromLocal, int64_t toLocal, Rational fps);

/// Number of frames (>= 0, at most @p cap) starting at clip-local frame @p firstLocal and going
/// in direction @p step (+1 / -1) whose source time stays inside the media
/// ([0, sourceDurationUs - 1]). Unbounded clips (no media length) return @p cap.
[[nodiscard]] int64_t validRun(
    const ClipData& clip, int64_t firstLocal, int step, Rational fps, int64_t cap);

// ---- Segments (speed bar) ----

/// One speed segment of a clip, between two boundaries inside [0, duration].
struct Segment
{
    int64_t startLocal = 0; ///< clip-local first frame
    int64_t endLocal = 0; ///< clip-local end (exclusive)
    double speed = 1.0; ///< average speed over the segment (1 = 100 %, < 0 reverse, 0 freeze)
    bool rampIn = false; ///< the speed changes smoothly at the start (non-linear handles)
    bool rampOut = false; ///< ... at the end
    int startKey = -1; ///< index of the curve key at startLocal, -1 = none (clip edge)
    int endKey = -1; ///< index of the curve key at endLocal, -1 = none

    [[nodiscard]] int64_t length() const { return endLocal - startLocal; }
    [[nodiscard]] bool isFreeze() const { return speed == 0.0; }
    [[nodiscard]] bool isReverse() const { return speed < 0.0; }
};
/// The speed segments of @p clip in order (one segment for a constant-speed clip). Keys
/// outside the clip are ignored; key frames inside (0, duration) split segments.
[[nodiscard]] QVector<Segment> segments(const ClipData& clip);
/// Index of the segment containing clip-local frame @p localFrame (clamped to the clip).
[[nodiscard]] int segmentAt(const QVector<Segment>& segs, int64_t localFrame);
/// Clip-local frames of the speed points (curve keys strictly inside the clip), ascending.
[[nodiscard]] QVector<int64_t> speedPoints(const ClipData& clip);

// ---- Retime process ----

/// The process that applies to @p clip in @p timeline (ProjectDefault resolved). A clip at a
/// constant 100 % always uses Nearest (nothing to interpolate).
[[nodiscard]] RetimeProcess effectiveProcess(const ClipData& clip, const TimelineData& timeline);
/// Stable id stored in project files: "default", "nearest", "frameBlend", "opticalFlow".
[[nodiscard]] QString processToString(RetimeProcess process);
/// Parses processToString(); unknown text yields @p fallback.
[[nodiscard]] RetimeProcess processFromString(
    const QString& text, RetimeProcess fallback = RetimeProcess::ProjectDefault);

/// Speed formatted for the speed bar and menus: "50%", "-100%", "0%" (freeze), at most one
/// decimal ("33.3%").
[[nodiscard]] QString formatPercent(double speed);

} // namespace kk::retime
