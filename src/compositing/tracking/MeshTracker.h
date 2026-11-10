// SPDX-License-Identifier: GPL-3.0-or-later
// MeshTracker — follows a deforming surface with a grid of points (Surface Tracker, spec 70.2).
// Every grid point has a PointTracker of its own (normalised cross-correlation, sub-pixel). After
// each frame the points are related to each other: a homography is fitted to the points that
// matched (RANSAC), the part of each point's movement the homography does not explain (the bend)
// is smoothed against its four neighbours, and the points that were lost or disagree are replaced
// by the homography plus the smoothed bend of their neighbours. So a flat surface behaves like the
// planar tracker, a bending one is followed by the mesh, and a point that is covered for a few
// frames is carried along by the rest. Standard methods (template matching, RANSAC, Laplacian
// smoothing); written for Kinoklip, no code taken from other projects.
// Pure maths on grey images: no widgets, no threads, no project types.
#pragma once

#include "compositing/tracking/Homography.h"
#include "compositing/tracking/PointTracker.h"

#include <vector>

namespace kk::tracking {

/// How a mesh is followed.
struct MeshSettings
{
    int cols = 4; ///< grid points per row (at least 2)
    int rows = 4; ///< grid point rows (at least 2)
    int patchSize = 21; ///< side of the pattern box of one point, pixels
    int searchSize = 61; ///< side of the search box of one point, pixels
    double minCorrelation = 0.55; ///< a point matching worse than this does not count
    /// 0 .. 1: how much of each point's bend is replaced by the mean of its neighbours' bend
    /// (applied three times). 0 keeps every point as found.
    double smoothing = 0.5;
    /// Fewer matching points than this fraction of the grid (and fewer than 4) lose the surface.
    double minGoodFraction = 0.4;
};

/// The mesh in one frame.
struct MeshResult
{
    bool ok = false; ///< false: the surface was lost (the points stay where they were)
    std::vector<Vec2> points; ///< cols * rows points, row by row, image pixels
    int good = 0; ///< points that matched and agree with the surface
    double correlation = 0; ///< mean correlation of those
};

class MeshTracker
{
public:
    explicit MeshTracker(MeshSettings settings = {});

    /// Starts on @p frame with the grid points @p points (cols * rows, row by row, image
    /// pixels). False if fewer than 4 points have a pattern with detail.
    bool reset(const GrayImage& frame, const std::vector<Vec2>& points);
    /// Finds the mesh in @p next.
    MeshResult track(const GrayImage& next);

    [[nodiscard]] const MeshSettings& settings() const { return m_settings; }
    [[nodiscard]] const std::vector<Vec2>& points() const { return m_points; }

private:
    MeshSettings m_settings;
    std::vector<PointTracker> m_trackers;
    std::vector<bool> m_usable; ///< the point had a pattern at the start
    std::vector<Vec2> m_rest; ///< the grid in the start frame
    std::vector<Vec2> m_points; ///< the grid in the latest frame
    bool m_ready = false;
};

/// The smoothing step of MeshTracker on its own (so it can be tested): the offsets @p bend of the
/// grid points from the surface, @p known flagging the ones to trust. Unknown points take the mean
/// of their known neighbours; all points are then blended towards their neighbours' mean
/// @p smoothing, three times. Needs cols * rows entries.
void smoothBend(std::vector<Vec2>& bend, std::vector<bool>& known, int cols, int rows, double smoothing);

} // namespace kk::tracking
