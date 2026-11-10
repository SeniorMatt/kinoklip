// SPDX-License-Identifier: GPL-3.0-or-later
// MatchMove — the Match Move effect's maths (spec section 70.1). The effect stores a track
// (core/EffectTrack.h): per source time either the tracked point (u, v) or the four corners of
// the tracked region, in normalised picture coordinates (u from the left, v from the BOTTOM, like
// the Tracker node). The clip is moved by what the track did since its reference time: the
// position, and with a region also the turn and the scale. The result is a transform in timeline
// frame pixels that render::clipTransform() applies AFTER the clip's own Transform and Dynamic
// Zoom, so the clip stays where the tracked thing is whatever its own scale. Pure maths.
#pragma once

#include "core/Rational.h"
#include "core/TimelineData.h"

#include <QSizeF>
#include <QString>
#include <QTransform>

#include <array>

namespace kk::matchmove {

/// Number of floats per key of a point track and of a region track (four corners).
inline constexpr int kPointStride = 2;
inline constexpr int kRegionStride = 8;

/// Text naming everything that a track depends on (source, tracking kind, region); a stored
/// track whose signature differs from the effect's current one is not used (spec 70.1).
[[nodiscard]] QString signature(const ParamSet& params);

/// True if @p fx is a Match Move that moves the clip: on, with a stored track of its current
/// signature.
[[nodiscard]] bool isActive(const EffectInstance& fx);
/// True if @p fx is a Match Move whose stored track does not fit its parameters any more.
[[nodiscard]] bool needsTracking(const EffectInstance& fx);
/// True if @p clip has an active Match Move.
[[nodiscard]] bool hasActive(const ClipData& clip);

/// What a track did between two keys.
struct Motion
{
    double dx = 0; ///< px, centre of the tracked thing
    double dy = 0;
    double angleDeg = 0; ///< clockwise on screen
    double scale = 1;
    double pivotX = 0; ///< the tracked thing's centre at the reference, px
    double pivotY = 0;
};

/// The motion from the key values @p ref to @p cur (stride kPointStride or kRegionStride) in a
/// picture of @p frameSize.
[[nodiscard]] Motion motionBetween(const float* ref, const float* cur, int stride, QSizeF frameSize);

/// The transform of all active Match Move effects of @p clip at clip-local frame @p localFrame
/// (timeline frame pixels to timeline frame pixels). Identity without any. @p fps is the
/// timeline's frame rate (finds the source time).
[[nodiscard]] QTransform transform(
    const ClipData& clip, int64_t localFrame, QSizeF frameSize, Rational fps);

} // namespace kk::matchmove
