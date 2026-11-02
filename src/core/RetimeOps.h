// SPDX-License-Identifier: GPL-3.0-or-later
// RetimeOps — pure editing functions for clip speed: speed points, segment speeds, reverse and
// freeze segments, freeze frames, ramps, the Change Clip Speed dialog and the retime process.
#pragma once

#include "core/Animation.h"
#include "core/TimelineData.h"

#include <QVector>

#include <cstdint>

namespace kk::retimeops {

// Every function edits the clip @p clipId *and its retime partners* (linked clips of the same
// link group with the same start, duration and retime, i.e. the audio of a V+A clip), so a
// linked pair always plays in sync. When the clip gets longer or shorter, the later clips on
// the partners' tracks ripple (move by the same amount), like Resolve's retime controls.
// Keyframes of other parameters stay at their clip-local frames: they are attached to the
// timeline, not to the source (spec 31.10). Locked tracks are never changed; functions
// return false / 0 when nothing changed. Model and units: core/Retime.h.

/// The clip and its retime partners (ids; @p clipId first). Empty if the clip is missing.
[[nodiscard]] QVector<quint64> partners(const TimelineData& data, quint64 clipId);
/// @p clipIds with every partner group reduced to its first member, so a command on a linked
/// selection edits each pair once.
[[nodiscard]] QVector<quint64> primaries(const TimelineData& data, const QVector<quint64>& clipIds);

/// Adds a speed point at timeline frame @p frame (strictly inside the clip). The two new
/// segments keep the speed there.
bool addSpeedPoint(TimelineData& data, quint64 clipId, int64_t frame);
/// Removes the speed point at clip-local @p localFrame; the neighbouring segments merge (the
/// clip keeps its length; the merged segment covers the same source). A curve left without
/// speed points and with one constant speed becomes a plain constant speed again.
bool removeSpeedPoint(TimelineData& data, quint64 clipId, int64_t localFrame);
/// Sets the speed of segment @p segment (index into retime::segments()) to @p speed (1 = 100 %,
/// negative = reverse, 0 = freeze). The segment keeps its source material, so its length
/// becomes |source| / |speed| (rounded to frames, the speed is then exact); later segments and
/// clips ripple. A freeze segment keeps its length and starts to play at @p speed.
bool setSegmentSpeed(TimelineData& data, quint64 clipId, int segment, double speed);
/// Plays segment @p segment backwards (same length, negated speed); later segments continue
/// from where it ends (the content stays continuous). Freeze segments cannot be reversed.
bool reverseSegment(TimelineData& data, quint64 clipId, int segment);
/// Inserts a freeze segment of @p lengthFrames frames showing the frame at timeline frame
/// @p frame; the rest of the clip plays on after it (the clip and later clips ripple).
bool insertFreeze(TimelineData& data, quint64 clipId, int64_t frame, int64_t lengthFrames);
/// Freeze frame (Shift+R): the whole clip shows the frame at timeline frame @p frame (the
/// clip's first frame when @p frame is outside it); its length stays.
bool freezeClip(TimelineData& data, quint64 clipId, int64_t frame);
/// Back to a constant 100 % from the clip's first source frame. The length follows the source
/// the clip covered (forward clips), else stays; later clips ripple.
bool resetClip(TimelineData& data, quint64 clipId);
/// Retime handle (bottom of a speed point): moves the speed point at clip-local
/// @p pointLocal (or the clip end, pointLocal = duration) by @p deltaFrames keeping the
/// source of the segment before it, i.e. changes that segment's speed; later material and
/// clips ripple. Returns the applied delta (the segment stays at least one frame long).
int64_t retimePoint(TimelineData& data, quint64 clipId, int64_t pointLocal, int64_t deltaFrames);
/// Speed point handle (top of a speed point): moves where the speed changes by
/// @p deltaFrames keeping both segments' speeds; the source frame at the point follows the
/// segment before it and the next segment covers correspondingly more or less source, so the
/// clip length changes (ripple). Returns the applied delta.
int64_t slidePoint(TimelineData& data, quint64 clipId, int64_t pointLocal, int64_t deltaFrames);
/// Speed ramp at the speed point at clip-local @p pointLocal: Interp::Linear = hard speed
/// change, Smooth = the speed ramps between the two segment speeds, EaseInOut = eases out of
/// and into the point (ramp through a stop). Other presets are applied as given.
bool setPointRamp(TimelineData& data, quint64 clipId, int64_t pointLocal, Interp ramp);

/// Change Clip Speed dialog input.
struct SpeedChange
{
    double speed = 1.0; ///< magnitude, 1 = 100 % (> 0; the sign comes from `reverse`)
    bool reverse = false; ///< play backwards over the same source
    bool freeze = false; ///< freeze frame at `freezeFrame` (overrides speed / reverse)
    int64_t freezeFrame = -1; ///< timeline frame of the frozen picture (-1 = clip start)
    /// true: the clip length follows the speed and later clips move; false: the length stays
    /// (limited by the media).
    bool ripple = true;
    bool pitchCorrection = true;
};
/// Applies @p change to the clip: one constant speed over the material the clip covers
/// (replaces a retime curve). Returns whether anything changed.
bool changeClipSpeed(TimelineData& data, quint64 clipId, const SpeedChange& change);
/// Sets the retime process of the clip (and partners).
bool setProcess(TimelineData& data, quint64 clipId, RetimeProcess process);
/// Turns pitch correction of the clip (and partners) on or off.
bool setPitchCorrection(TimelineData& data, quint64 clipId, bool on);

/// Converts a constant-speed clip into a retime curve with keys at its first frame and its
/// end (no visible change), and makes sure a curve has keys at both edges.
void ensureCurve(ClipData& clip);
/// Turns a curve without speed points that is one straight line (all Linear, equal speeds)
/// back into a constant speed. No visible change.
void simplifyCurve(ClipData& clip);

} // namespace kk::retimeops
