// SPDX-License-Identifier: GPL-3.0-or-later
// FrameRateConvert — changes the frame rate of a timeline that already has content. Clips,
// keyframes, markers and everything else that is stored in frames keep their place in time.
#pragma once

#include "core/Rational.h"
#include "core/TimelineData.h"

#include <cstdint>

namespace kk {

/// @p frames at rate @p from as frames at rate @p to, rounded to the nearest frame (halves away
/// from zero). Exact integer arithmetic, so 24 -> 60 -> 24 returns every frame unchanged and
/// 30000/1001 -> 60 has no drift. Invalid rates return @p frames.
[[nodiscard]] int64_t convertFrames(int64_t frames, Rational from, Rational to);

/// Gives @p data the frame rate @p newFps and moves everything stored in frames so that it stays
/// at the same moment in time:
///
/// * Clip edit points (start and end) are converted, not durations, so clips that touched still
///   touch and gaps stay gaps (a clip that would shrink to nothing keeps one frame and pushes
///   its followers, so the tracks never overlap). Source in/out are microseconds and stay.
/// * Keyframes of every parameter set (transform, audio, title and its layers, effects, masks
///   and their shapes, compositions, grades, retime curves, transitions) keep their distance from
///   the clip start; Bezier handle lengths scale with the time; retime curve values (source
///   frames at the timeline rate) scale too. Track automation and the timeline grade are
///   converted in timeline frames. Two keys that land on one frame keep the one nearer to its
///   moment (only when the rate goes down).
/// * Fades, transition lengths, clip and timeline markers (with their length), the in/out marks
///   and settings that count frames (Motion Trails spacing, Time Window, a Timecode generator's
///   start) are converted. The starting timecode keeps its HH:MM:SS:FF.
/// * Nested and multicam clips keep showing the same time of their timeline (their source is in
///   microseconds). The nested timelines themselves are not touched: convert them separately.
///
/// Does nothing when @p newFps is invalid or equal to the current rate. Converting back gives
/// the original frames again where the rate went up first (24 -> 60 -> 24).
void convertTimelineFrameRate(TimelineData& data, Rational newFps);

} // namespace kk
