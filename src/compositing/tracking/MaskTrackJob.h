// SPDX-License-Identifier: GPL-3.0-or-later
// MaskTrackJob — "Follow Subject" for the masks of the Edit page (spec section 187): a mask drawn
// once follows what is under it by itself. planMaskTrack() takes the mask's outline at the
// playhead as the region of a planar track (the EffectTrackJob machinery of Match Move: the
// clip's pictures from the playback engine, the TrackerRunner), storeMaskTrack() turns the
// finished samples into the mask's keyframes (position, scale and rotation, through the same
// followMatchMove() the Follow Track row uses). No Match Move effect has to be added first.
// Pure maths and project types, no widgets.
#pragma once

#include "compositing/tracking/EffectTrackJob.h"
#include "core/Rational.h"

#include <QRectF>
#include <QSizeF>

#include <optional>
#include <vector>

namespace kk::tracking {

/// A planned mask track: the planar track of the mask's region and what is needed to store it.
struct MaskTrackPlan
{
    EffectTrackPlan track; ///< timeline frames; `source` is left for the caller
    quint64 clipId = 0;
    quint64 maskId = 0;
    int64_t offsetFrame = 0; ///< the clip-local frame the mask keeps its place on
    QRectF region; ///< the tracked square, picture px from the picture's middle (y down)
};

/// The bounds of @p mask at clip-local @p localFrame in picture px from the clip's middle (y
/// down); an empty rectangle for a mask without a shape (an open path, no points).
[[nodiscard]] QRectF maskBounds(const MaskData& mask, int64_t localFrame);

/// Plans following mask @p maskId of clip @p clipId from timeline frame @p playhead forwards or
/// backwards to the end or start of the clip. The clip is tracked without its masks (a mask that
/// hides the subject would hide it from the tracker too). nullopt with a sentence in @p why when
/// it cannot be done (unknown clip or mask, a mask without a shape, the playhead outside the
/// clip, no video).
[[nodiscard]] std::optional<MaskTrackPlan> planMaskTrack(const TimelineData& timeline,
    quint64 clipId, quint64 maskId, int64_t playhead, bool forward, QString* why);

/// Writes the mask's Position, Scale and Rotation keys (a key at every frame of the clip) from
/// @p samples, the frames found by one or both runs made with plans of the same playhead. The
/// mask keeps its place at the playhead. False if the mask or clip is missing or there are too
/// few samples.
bool storeMaskTrack(ClipData& clip, const MaskTrackPlan& plan,
    const std::vector<TrackSample>& samples, QSizeF frameSize, Rational fps);

} // namespace kk::tracking
