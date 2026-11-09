// SPDX-License-Identifier: GPL-3.0-or-later
// TrackLink — makes another node's position follow a track. Kinoklip has no parameter links
// (every parameter is keyframes or a constant), so "Connect to Tracker" resolves the track into
// the target's Center X / Center Y keyframes: a copy, not a live link, which keeps the data model
// and the render path unchanged and leaves the result editable in the Keyframes panel.
#pragma once

#include "core/Composition.h"

#include <QPointF>

#include <cstdint>
#include <vector>

namespace kk::tracking {

/// True for node types that hold a track: Tracker and Planar Tracker.
[[nodiscard]] bool isTrackSource(const CompNode& node);

/// Ids of the track sources of @p comp that have a track (keyframes), in node order.
[[nodiscard]] std::vector<quint64> trackSources(const Composition& comp);

/// True if @p node has a position (Center X / Center Y) that can follow a track: every node with
/// both parameters except the track sources themselves.
[[nodiscard]] bool canFollowTrack(const CompNode& node);

/// The tracked position at @p frame, normalised like Center X / Center Y (x from the left, y
/// from the bottom): a Tracker's point, or the centre of a Planar Tracker's region (where its
/// diagonals cross).
[[nodiscard]] QPointF trackCentre(const CompNode& source, int64_t frame);

/// Frames at which @p source has keys, ascending.
[[nodiscard]] std::vector<int64_t> trackFrames(const CompNode& source);

/// Replaces the Center X / Center Y of @p target with linear keys at every frame of @p source's
/// track: the track's centre plus a fixed offset. The offset is chosen so that the target does not
/// move at @p offsetFrame (it keeps its place there and follows the track's movement from then
/// on). False if @p target cannot follow, @p source has no track, or either node is locked.
bool linkToTrack(CompNode& target, const CompNode& source, int64_t offsetFrame);

} // namespace kk::tracking
