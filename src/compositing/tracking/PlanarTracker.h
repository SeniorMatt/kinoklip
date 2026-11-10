// SPDX-License-Identifier: GPL-3.0-or-later
// PlanarTracker — follows a flat surface (a four-corner region) from frame to frame. A grid of
// patches on the plane is matched in every new frame with the PointTracker's matcher
// (normalised cross-correlation, sub-pixel Gauss-Newton), the patches first warped by the
// homography found for the previous frame so that perspective and rotation do not spoil the
// match. RANSAC with a normalised DLT (Homography.h) turns the matches into the plane's
// homography, which is then used to re-warp the patches and match again at a small radius
// (a second, finer pass). The patches are taken from the start frame (no drift) and re-taken
// from the current frame when the matches get weak. Standard methods; written for Kinoklip.
// Pure maths on grey images: no Qt widgets, no threads, no project types.
#pragma once

#include "compositing/tracking/Homography.h"
#include "compositing/tracking/PointTracker.h"

#include <vector>

namespace kk::tracking {

/// How a plane is followed.
struct PlanarSettings
{
    int gridSize = 7; ///< patches per side of the grid laid on the plane (at least 3)
    int patchSize = 0; ///< side of a patch, pixels; 0 = from the size of the region
    int searchRadius = 24; ///< how far a patch may move between two frames, pixels
    double minCorrelation = 0.6; ///< a patch matching worse than this is ignored
    /// Fraction of the usable patches that must agree on one homography; fewer: the plane is lost.
    double minInlierFraction = 0.4;
    /// Mean correlation of the agreeing patches below which the patches are re-taken from the
    /// current frame (the target changes its look slowly). 0 = never.
    double refreshBelow = 0.85;
    double inlierThreshold = 1.0; ///< RANSAC distance, pixels
};

/// Result of following the plane into one more frame.
struct PlanarResult
{
    bool ok = false; ///< false: the plane was lost (the corners stay where they were)
    Quad corners; ///< the region's corners in the new frame (top-left, top-right, bottom-right,
                  ///< bottom-left), image pixels
    Homography motion; ///< start-frame pixels -> this frame's pixels
    double correlation = 0; ///< mean correlation of the patches that agree
    int inliers = 0; ///< patches that agree with the homography
    int patches = 0; ///< patches that were matched at all
    /// The patches that agree with `motion`: where each was on the plane (start-frame pixels) and
    /// where it was found in this frame. Equal length; empty when the plane was lost. For callers
    /// that fit a simpler model than a homography (the stabilizer).
    std::vector<Vec2> inlierFrom;
    std::vector<Vec2> inlierTo;
};

/// Follows one plane through a sequence of frames, in either direction (the caller decides the
/// order of the frames). Create, reset() on the first frame, then track() each next frame.
class PlanarTracker
{
public:
    explicit PlanarTracker(PlanarSettings settings = {});

    /// Starts on @p frame with the region @p corners (a convex quad inside the picture, image
    /// pixels, pixel (i, j) centred at (i, j)). False if the region is degenerate or has too
    /// little detail (fewer than 6 usable patches).
    bool reset(const GrayImage& frame, const Quad& corners);
    /// Finds the plane in @p next.
    PlanarResult track(const GrayImage& next);

    [[nodiscard]] const Quad& corners() const { return m_corners; }
    [[nodiscard]] const PlanarSettings& settings() const { return m_settings; }

private:
    /// Matches every patch around its place predicted by @p predicted, within @p radius.
    /// Returns the pairs (plane point -> found position) and their correlations.
    void matchPatches(const GrayImage& next, const GrayImage& nextHalf, const Homography& predicted,
        int radius, double minCorrelation, std::vector<Vec2>& from, std::vector<Vec2>& to,
        std::vector<double>& correlation) const;

    PlanarSettings m_settings;
    Quad m_startCorners; ///< the region in the start frame
    Quad m_corners; ///< the region in the latest frame
    GrayImage m_template; ///< the picture the patches are cut from
    GrayImage m_templateHalf; ///< the same at half size (for the coarse search)
    Homography m_templateMap; ///< start-frame pixels -> template pixels
    Homography m_motion; ///< start-frame pixels -> latest frame pixels
    std::vector<Vec2> m_points; ///< patch centres on the plane, in start-frame pixels
    int m_patch = 0; ///< patch side, pixels (odd)
    bool m_ready = false;
};

} // namespace kk::tracking
