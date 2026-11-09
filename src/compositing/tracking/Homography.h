// SPDX-License-Identifier: GPL-3.0-or-later
// Homography — the maths of the planar tracker: a 3x3 projective map, its estimation from point
// pairs (normalised direct linear transform, with RANSAC against outliers) and the similarity
// (move, turn, scale) that best describes a quad's motion. Textbook methods (Hartley and
// Zisserman, "Multiple View Geometry", chapters 4 and 11), written for Kinoklip; no code taken
// from other projects. Pure maths: no Qt types, no threads, no project types.
#pragma once

#include "compositing/tracking/PointTracker.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace kk::tracking {

/// A projective map of the plane: (x, y) -> ((m0 x + m1 y + m2) / w, (m3 x + m4 y + m5) / w)
/// with w = m6 x + m7 y + m8. Row-major. Default: the identity.
struct Homography
{
    std::array<double, 9> m {1, 0, 0, 0, 1, 0, 0, 0, 1};

    /// Where @p p goes. A point sent to infinity (w ~ 0) comes back as the far-away point
    /// 1e12 along its direction, never as NaN.
    [[nodiscard]] Vec2 map(Vec2 p) const;
    /// The inverse map, or nullopt when the matrix is singular.
    [[nodiscard]] std::optional<Homography> inverse() const;
    /// The map "first @p first, then this": this * first.
    [[nodiscard]] Homography after(const Homography& first) const;
    /// Scaled so that m8 = 1 (or, when m8 is ~0, to unit length); keeps the map.
    [[nodiscard]] Homography normalised() const;
    /// True if every entry is finite.
    [[nodiscard]] bool isFinite() const;
};

/// Four corners of a quad, in the order top-left, top-right, bottom-right, bottom-left (for a
/// picture, y down).
using Quad = std::array<Vec2, 4>;

/// Least-squares homography taking @p from[i] to @p to[i] (normalised DLT: both point sets are
/// first moved to their centroid and scaled to a mean distance of sqrt(2), which keeps the
/// linear system well conditioned for pixel coordinates). Needs at least 4 pairs; exactly 4 give
/// the exact map. Returns nullopt for fewer pairs, mismatched sizes, non-finite input, and
/// degenerate configurations (all points equal or on one line, so the map is not determined).
[[nodiscard]] std::optional<Homography> homographyFromPoints(
    const std::vector<Vec2>& from, const std::vector<Vec2>& to);

/// The homography taking quad @p from to quad @p to; nullopt if either is degenerate.
[[nodiscard]] std::optional<Homography> homographyFromQuads(const Quad& from, const Quad& to);

/// How RANSAC searches.
struct RansacSettings
{
    int iterations = 300; ///< random 4-point samples tried
    double inlierThreshold = 1.5; ///< a pair is an inlier if the map lands this close (pixels)
    uint32_t seed = 12345; ///< the random generator is deterministic: same input, same result
};

/// Result of estimateHomography().
struct HomographyFit
{
    Homography h;
    std::vector<bool> inliers; ///< one flag per input pair
    int inlierCount = 0;
    double meanError = 0; ///< mean distance of the inliers from their target, pixels
};

/// Robust homography: RANSAC over 4-point samples (samples with three nearly collinear points
/// are skipped), then a least-squares refit on all inliers, repeated until the inlier set stops
/// growing. Nullopt when no model with at least 4 inliers exists (also for fewer than 4 pairs).
[[nodiscard]] std::optional<HomographyFit> estimateHomography(const std::vector<Vec2>& from,
    const std::vector<Vec2>& to, const RansacSettings& settings = {});

/// A similarity: to = scale * R(angleRad) * from + (dx, dy), R a rotation in the picture's own
/// coordinates (for y-down pictures, a positive angle turns clockwise on screen).
struct Similarity
{
    double scale = 1;
    double angleRad = 0;
    double dx = 0;
    double dy = 0;

    /// Where @p p goes.
    [[nodiscard]] Vec2 map(Vec2 p) const;
    /// The opposite similarity (undoes this one). Scale must not be 0.
    [[nodiscard]] Similarity inverse() const;
};

/// Least-squares similarity from @p from to @p to (closed form, Umeyama 1991). Nullopt for fewer
/// than 2 pairs, mismatched sizes, or when all @p from points coincide.
[[nodiscard]] std::optional<Similarity> similarityFromPoints(
    const std::vector<Vec2>& from, const std::vector<Vec2>& to);

/// The similarity that best describes how quad @p from became quad @p to.
[[nodiscard]] std::optional<Similarity> similarityFromQuads(const Quad& from, const Quad& to);

/// True if @p q is a usable plane: convex, same winding all round, and an area of at least
/// @p minArea (square pixels).
[[nodiscard]] bool isConvexQuad(const Quad& q, double minArea = 1.0);

/// Area of @p q (shoelace formula, absolute value).
[[nodiscard]] double quadArea(const Quad& q);

/// The point where the diagonals of @p q cross: the centre of the plane as seen in the picture
/// (for a perspective view it is not the average of the corners).
[[nodiscard]] Vec2 quadCentre(const Quad& q);

} // namespace kk::tracking
