// SPDX-License-Identifier: GPL-3.0-or-later
// SurfaceTracker — the maths of the Surface Tracker effect (spec section 70.2). The effect stores
// a mesh track (core/EffectTrack.h): per source time the positions (u, v) of a square grid of
// points, in normalised picture coordinates (u from the left, v from the BOTTOM), tracked by
// compositing/tracking/MeshTracker.h. Two ways to use it: Pin moves the clip by the perspective
// map that best takes the mesh of the reference time to the mesh now (a graphic stuck on the
// surface), applied in clipTransform() after the clip's own Transform like Match Move; Warp bends
// the clip's picture with the mesh (GL pass, render/GlMeshEffects.cpp). Pure maths.
#pragma once

#include "core/EffectTrack.h"
#include "core/Rational.h"
#include "core/TimelineData.h"

#include <QSizeF>
#include <QString>
#include <QTransform>

#include <vector>

namespace kk::surface {

inline constexpr int kMinDensity = 2;
inline constexpr int kMaxDensity = 8;

/// Text naming everything a stored mesh track depends on (source, region, density, smoothing).
[[nodiscard]] QString signature(const ParamSet& params);

/// True if @p fx is a Surface Tracker with a stored track of its current signature.
[[nodiscard]] bool isTracked(const EffectInstance& fx);
/// True if its stored track does not fit its parameters any more.
[[nodiscard]] bool needsTracking(const EffectInstance& fx);
/// True if @p clip has an enabled Surface Tracker in Pin mode with a current track.
[[nodiscard]] bool hasActivePin(const ClipData& clip);

/// The displacements of the mesh at @p second, for the Warp pass.
struct Mesh
{
    int cols = 0, rows = 0;
    float rest[4] = {0, 0, 1, 1}; ///< bounding rectangle of the reference mesh, content units
    std::vector<float> d; ///< 2 per node (row by row, row 0 at the top): current - reference
};
/// Fills @p mesh from @p table at @p second; false if the table is not a square mesh.
[[nodiscard]] bool meshDisplacement(const efftrack::Table& table, double second, Mesh* mesh);

/// The transform of all Pin-mode Surface Trackers of @p clip (timeline frame px to frame px).
[[nodiscard]] QTransform pinTransform(
    const ClipData& clip, int64_t localFrame, QSizeF frameSize, Rational fps);

} // namespace kk::surface
