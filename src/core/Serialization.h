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
/// - 3: several timelines per project ("timelines" array with "uid", "compound",
///   "useProjectSettings"; "activeTimeline", "tabs", "timelineViews"; project resolution and
///   frame rate in "project"), Nested clips ("kind": "nested", "nestedUid"), timeline /
///   compound clip items in the media pool ("kind", "timelineUid"). Files of versions 1 and 2
///   store one "timeline" object; Project::load() turns it into timeline uid 1 with a media
///   pool item, which reproduces the old project exactly.
/// - 4: clips may carry "masks" (core/Mask.h): {"id", "shape", "mode", "name"?, "enabled"?,
///   "invert"?, "closed"?, "params"?, and "points" (static shape) or "shapeKeys" (keyframes with
///   their "points")}; a point is [x, y, inX, inY, outX, outY] plus 1 when smooth. Files of
///   versions 1 - 3 have no masks and load through the older migrations unchanged; older builds
///   refuse version 4 instead of silently dropping the masks.
/// - 5: retiming. A clip's speed moved into a "retime" object {"speed", "curve" (a ParamSet
///   with the keys::RetimeFrame curve, optional), "process", "pitchCorrection"}; timelines
///   carry "retimeProcess" (the project default). Older files store a top-level clip
///   "speed": clipFromJson() reads it as a single constant-speed segment with pitch
///   correction off (audio of older projects was resampled), which plays exactly as before.
/// - 6: proxy / optimized media links ("mediaLinks": [{"mediaId", "proxy"?, "optimized"?}],
///   each {"path", "source": {"size", "mtimeMs"}, "width", "height", "format"?}), the
///   project's "cacheSettings" (core/MediaLinks.h: proxy / optimized / render cache formats,
///   background caching, proxy handling, render cache mode) and the clip flag
///   "cacheOutput" (Render Cache Clip Output). Older files get the defaults
///   (proxy handling "prefer proxies", render cache "none"); nothing else changes, so the
///   loader needs no step for 4 -> 6 or 5 -> 6; proxies already in the proxy folder are linked
///   by the application after loading (MediaLinks::autoLink()).
/// - 7: adjustment clips ("kind": "adjustment"), generator clips ("kind": "generator",
///   "generator": descriptor id, parameters in "title"), subtitle tracks ("subtitleTracks":
///   tracks with "subtitleStyle" and "language"?, holding "kind": "subtitle" clips whose text
///   and style overrides are in "title"), subtitle media pool items ("kind": "subtitle",
///   "cues": [[startUs, endUs, text], ...]). Older
///   files have none of these and load unchanged; older builds refuse version 7 instead of silently
///   dropping captions and adjustment clips.
/// - 8: multicam clips. Timelines may carry "multicam": true (always with "compound"), clips
///   "multicamAngle" (0-based angle of a Nested clip showing a multicam timeline), media
///   pool items "kind": "multicam", and media info "timecode" (embedded start timecode).
///   Every field is optional, so files of versions 1 - 7 load unchanged; older builds refuse
///   version 8 instead of showing every angle of a multicam clip stacked.
/// - 9: media pool organisation (bins branch). Media pool items may carry "clipColor" (palette
///   id, core/ClipColors.h), "flags" (array of flag ids), "metadata" (description, comments,
///   keywords, scene, shot, take, camera, angle, goodTake, rating; only the fields set),
///   "added" / "modified" (ms since the epoch); the media pool may carry "smartBins"
///   ([{"id", "name", "match": "all" | "any", "rules": [{"field", "op", "value"}]}], ids from
///   core/SmartBins.h); the project stores "binViews" ([{"bin", "mode", "sort", "descending"}],
///   per-bin view mode and sort order). The order of "bins" is the order of sibling bins.
///   Every field is optional, so older files load unchanged; for files older than 9
///   Project::fromJson() fills "modified" from the files on disk and uses it as "added" too
///   (the best known bound), so the date rules of smart bins work on old projects. Older builds
///   refuse version 9 instead of silently dropping colours, flags and metadata.
inline constexpr int kFormatVersion = 9;

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
