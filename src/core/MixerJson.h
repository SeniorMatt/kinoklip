// SPDX-License-Identifier: GPL-3.0-or-later
// MixerJson — JSON form of the mixer model (project format 12): "mix" on audio tracks and
// "mixer" on timelines. Only values that differ from the defaults are written.
#pragma once

#include "core/MixerData.h"

#include <QJsonObject>

#include <vector>

namespace kk {
struct TrackData;
}

namespace kk::serialization {

/// Encodes a channel strip (only the non-default fields; {} for a default strip).
[[nodiscard]] QJsonObject stripToJson(const ChannelStrip& strip);
/// Decodes stripToJson(); missing fields keep their defaults, values are clamped.
[[nodiscard]] ChannelStrip stripFromJson(const QJsonObject& json);

/// Encodes a track's mixer settings ({} for the defaults: the loader then gives the defaults).
[[nodiscard]] QJsonObject trackMixToJson(const TrackMix& mix);
/// Decodes trackMixToJson(); a missing object (formats 1 - 11) yields the defaults.
[[nodiscard]] TrackMix trackMixFromJson(const QJsonObject& json);

/// Encodes the timeline's buses and output selection ({} for the defaults).
[[nodiscard]] QJsonObject mixerToJson(const MixerData& mixer);
/// Decodes mixerToJson(): the main bus always exists (added if missing), duplicate ids are
/// dropped, an unknown output bus falls back to the main bus.
[[nodiscard]] MixerData mixerFromJson(const QJsonObject& json);

/// Drops routes of the tracks of @p tracks to buses @p mixer does not have.
void repairRoutes(const MixerData& mixer, std::vector<TrackData>& tracks);

} // namespace kk::serialization
