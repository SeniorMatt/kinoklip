// SPDX-License-Identifier: GPL-3.0-or-later
// JSON (de)serialization of the core value types, used by project files and the clipboard.
#pragma once

#include "core/MediaItem.h"
#include "core/TimelineData.h"

#include <QDir>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace kk::serialization {

/// Version written to the "formatVersion" field of project files. Bump it when the format
/// changes incompatibly and add a migration in Project::load().
///
/// History:
/// - 1: keyframes carry one "interp" (curve of the segment starting at the key).
/// - 2: keyframes carry Bezier handles ("in" / "out": mode string or {"mode":"free","dt","dv"})
///   and a "hold" flag. Version-1 keys are detected by their "interp" field and converted by
///   paramSetFromJson() (migrateLegacyInterp() in core/Animation.h).
/// - 3: clips may carry "masks" (core/Mask.h): {"id", "shape", "mode", "name"?, "enabled"?,
///   "invert"?, "closed"?, "params"?, and "points" (static shape) or "shapeKeys" (keyframes with
///   their "points")}; a point is [x, y, inX, inY, outX, outY] plus 1 when smooth. Version-2
///   files have no masks and load unchanged (see Project::load()); older builds refuse version 3
///   instead of silently dropping the masks.
inline constexpr int kFormatVersion = 3;

/// Encodes one keyframe: {"frame", "value"} plus "in" / "out" / "hold" when not default.
[[nodiscard]] QJsonObject keyframeToJson(const Keyframe& key);
/// Decodes keyframeToJson() (a version-1 "interp" field is ignored here; see paramSetFromJson).
[[nodiscard]] Keyframe keyframeFromJson(const QJsonObject& json);
/// Encodes a key handle: the mode string, or an object for Free handles.
[[nodiscard]] QJsonValue keyHandleToJson(const KeyHandle& handle);
/// Decodes keyHandleToJson(); anything unknown yields a Linear handle.
[[nodiscard]] KeyHandle keyHandleFromJson(const QJsonValue& json);

/// Encodes a ParamSet as {"values": {...}, "statics": {...}}.
[[nodiscard]] QJsonObject paramSetToJson(const ParamSet& params);
/// Decodes a ParamSet; unknown or malformed entries are skipped.
[[nodiscard]] ParamSet paramSetFromJson(const QJsonObject& json);

/// Encodes a static parameter value (bool, number, string, QColor → {"color": "#aarrggbb"}).
[[nodiscard]] QJsonValue variantToJson(const QVariant& value);
/// Decodes a value written by variantToJson().
[[nodiscard]] QVariant variantFromJson(const QJsonValue& json);

/// Encodes one clip (also used for copy/paste).
[[nodiscard]] QJsonObject clipToJson(const ClipData& clip);
/// Decodes one clip; missing fields keep their defaults.
[[nodiscard]] ClipData clipFromJson(const QJsonObject& json);

/// Encodes a complete timeline.
[[nodiscard]] QJsonObject timelineToJson(const TimelineData& data);
/// Decodes a timeline, repairs ids/sort order and validates the invariants.
/// Returns false with a message in @p error if the data is unusable.
[[nodiscard]] bool timelineFromJson(const QJsonObject& json, TimelineData& out, QString* error);

/// Encodes a media probe result.
[[nodiscard]] QJsonObject mediaInfoToJson(const media::MediaInfo& info);
/// Decodes a media probe result.
[[nodiscard]] media::MediaInfo mediaInfoFromJson(const QJsonObject& json);

/// Encodes the media pool. Paths are stored absolute plus relative to @p projectDir so a moved
/// project folder still finds its media.
[[nodiscard]] QJsonObject mediaPoolToJson(const MediaPoolState& state, const QDir& projectDir);
/// Decodes the media pool, preferring the relative path when the absolute one is missing.
[[nodiscard]] MediaPoolState mediaPoolFromJson(const QJsonObject& json, const QDir& projectDir);

} // namespace kk::serialization
