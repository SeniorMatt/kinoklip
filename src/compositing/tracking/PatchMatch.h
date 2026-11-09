// SPDX-License-Identifier: GPL-3.0-or-later
// PatchMatch — the template matching steps of PointTracker (exhaustive normalised correlation,
// Gauss-Newton sub-pixel refinement), shared with the PlanarTracker so there is one matcher.
#pragma once

#include "compositing/tracking/PointTracker.h"

#include <vector>

namespace kk::tracking::detail {

/// Normalised correlation (-1..1) of the zero-mean pattern @p values (side @p size, root of the
/// sum of squares @p norm) with the window of @p image centred on the real position @p p
/// (bicubic samples). 0 for a flat window or pattern.
double correlationReal(
    const GrayImage& image, Vec2 p, const std::vector<float>& values, int size, double norm);

/// Best integer position in [cx - radius, cx + radius] x [cy - radius, cy + radius] whose whole
/// window lies inside the picture; ties go to the candidate nearest the centre. False when no
/// candidate fits. @p best is the correlation there.
bool bestInteger(const GrayImage& image, int cx, int cy, int radius,
    const std::vector<float>& values, int size, double norm, int* bx, int* by, double* best);

/// Gauss-Newton refinement of a real position: minimises the difference between the normalised
/// pattern and the normalised window. Returns @p start when it does not converge within
/// @p maxMove pixels.
Vec2 refine(const GrayImage& image, Vec2 start, const std::vector<float>& values, int size,
    double norm, double maxMove);

} // namespace kk::tracking::detail
