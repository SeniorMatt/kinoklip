// SPDX-License-Identifier: GPL-3.0-or-later
// TrackStore — writes tracked positions into a Tracker node as keyframed X / Y data.
#pragma once

#include "compositing/tracking/TrackerRunner.h"
#include "core/Composition.h"

#include <vector>

namespace kk::tracking {

/// Stores @p samples as keys of the node's CenterX / CenterY (linear keys at the sample frames;
/// existing keys at those frames are replaced, other keys stay). A node whose point was not
/// animated yet also gets its Reference Frame set to @p referenceFrame, so the first track
/// leaves the picture where it is at the frame it started on. False if there is nothing to store.
bool storeTrack(CompNode& node, const std::vector<TrackSample>& samples, int64_t referenceFrame);

/// Removes all tracked keys: the point stays where it is at @p frame.
void clearTrack(CompNode& node, int64_t frame);

} // namespace kk::tracking
