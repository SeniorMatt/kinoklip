// SPDX-License-Identifier: GPL-3.0-or-later
// LevelOps — pure edits behind the timeline's audio level lines (clip volume "rubber band")
// and the clip fade handles: offset the volume, add / move volume keys, set fades. Callable
// without the mouse (docs/archive/plan.md 5.9b); the timeline gestures only fill in the arguments.
#pragma once

#include "core/SelectionModel.h"
#include "core/TimelineData.h"

#include <cstdint>
#include <optional>

namespace kk::levelops {

/// Inspector range of the clip volume (dB); -96 = silence.
inline constexpr double kMinVolumeDb = -96.0;
inline constexpr double kMaxVolumeDb = 24.0;

/// Keyframe path of the clip volume ("audio/volumeDb", core/KeyframeOps.h).
[[nodiscard]] QString volumePath();

/// Volume (dB) of @p clip at fractional clip-local frame @p localFrame (keys or the constant).
[[nodiscard]] double volumeDbAt(const ClipData& clip, double localFrame);

/// Drags the level line of clip @p clipId by @p deltaDb at clip-local @p localFrame:
/// without keys the constant volume changes; with keys the keys that shape the line there
/// move (the two keys around it, the key itself when exactly on one, the first / last key
/// before / after them), like dragging a segment of the line. Values are clamped to the
/// volume range. Callers restore the drag origin first (absolute delta). Returns whether
/// anything changed.
bool offsetVolume(TimelineData& data, quint64 clipId, int64_t localFrame, double deltaDb);

/// Sets the clip volume of clip @p clipId to @p gainDb (Clip > Normalize Audio Levels,
/// spec 37.35): the constant volume becomes @p gainDb and, on a keyframed volume, every key
/// moves by the same change, so the shape of the automation is kept. Clamped to the volume
/// range. Returns whether anything changed (false for an unknown clip).
bool setClipGain(TimelineData& data, quint64 clipId, double gainDb);

/// Adds a volume key at @p localFrame with the line's current value there (Alt+click on the
/// line). Returns the key, or nothing if the clip is unknown / not an audio-capable clip.
std::optional<KeyframeRef> addVolumeKey(TimelineData& data, quint64 clipId, int64_t localFrame);

/// Moves the volume key @p ref to clip-local @p newFrame (clamped inside the clip; a key
/// landing on another one replaces it) and sets its value to @p valueDb (clamped). Returns the
/// key at its new place, or nothing if it does not exist.
std::optional<KeyframeRef> setVolumeKey(
    TimelineData& data, const KeyframeRef& ref, int64_t newFrame, double valueDb);

/// Sets the fade-in (@p head) or fade-out of clip @p clipId to @p frames, clamped so that
/// fade-in + fade-out never exceed the clip. Returns whether anything changed.
bool setFade(TimelineData& data, quint64 clipId, bool head, int64_t frames);

} // namespace kk::levelops
