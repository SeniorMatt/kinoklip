// SPDX-License-Identifier: GPL-3.0-or-later
// JSON (de)serialization of the core value types, used by project files and the clipboard.
#pragma once

#include "core/MediaItem.h"
#include "core/TimelineData.h"

#include <QDir>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace cw::serialization {

/// Version written to the "formatVersion" field of project files. Bump it when the format
/// changes incompatibly and add a migration in Project::load().
inline constexpr int kFormatVersion = 1;

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

} // namespace cw::serialization
