// SPDX-License-Identifier: GPL-3.0-or-later
// PlanarTracker implementation. See the header for the method.
#include "compositing/tracking/PlanarTracker.h"

#include "compositing/tracking/PatchMatch.h"

#include <algorithm>
#include <cmath>

namespace kk::tracking {

namespace {

constexpr int kMinPatches = 6; ///< fewer usable patches cannot pin down a homography reliably
constexpr int kMinPatch = 9;
constexpr int kMaxPatch = 31;
constexpr double kPatchFraction = 0.18; ///< patch side / mean side of the region
constexpr double kGridMargin = 0.1; ///< the grid keeps this fraction of the region free at the edge
constexpr double kMinContrast = 0.012; ///< standard deviation of a patch below which it is flat
constexpr int kFinePassRadius = 3; ///< the second pass looks only this far from the first result
constexpr double kMinAreaPixels = 64; ///< smallest region worth tracking
constexpr double kAreaChangeLimit = 40.0; ///< the region may grow / shrink this much from the start
constexpr double kRefineMaxMove = 1.5;
/// Searches wider than this many pixels look first on half-size pictures (a quarter of the
/// candidates, a quarter of the pixels per candidate: about 10 times faster at the default
/// radius), then around that result at full size.
constexpr int kCoarseAbove = 6;
constexpr int kCoarseToFineRadius = 3; ///< full-size search around the half-size result
constexpr int kMinCoarsePatch = 5;
/// The second pass warps the patches by the first pass's homography, so a true match of an
/// unchanged plane correlates well; a partly hidden or foreign patch (an object in front of the
/// plane) correlates worse and would only pull the fit by up to the RANSAC distance. The second
/// pass therefore asks for at least this much, unless that leaves too few patches.
constexpr double kFinePassMinCorrelation = 0.85;

int oddClamped(double v)
{
    const int i = int(std::lround(v));
    return std::clamp(i, kMinPatch, kMaxPatch) | 1;
}

double sideLength(Vec2 a, Vec2 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

/// The patch of @p image around @p centre sampled at positions @p map(centre + offset), zero
/// mean. Returns the root of the sum of squares (0 when a sample falls outside the picture).
///
/// @p step 1 reads @p image at full size. @p step 2 is for a half-size @p image (and a half-size
/// picture being searched): patch pixels are two full-size pixels apart and the positions are
/// halved before sampling, so the patch covers the same ground at half the resolution.
double warpedPatch(const GrayImage& image, const Homography& map, Vec2 centre, int size,
    std::vector<float>& values, double* contrast, double step = 1.0)
{
    values.assign(size_t(size) * size_t(size), 0.0f);
    const int half = size / 2;
    double mean = 0;
    for (int j = 0; j < size; ++j) {
        for (int i = 0; i < size; ++i) {
            Vec2 p = map.map({centre.x + (i - half) * step, centre.y + (j - half) * step});
            p.x /= step;
            p.y /= step;
            if (!(p.x >= 0 && p.y >= 0 && p.x <= image.width - 1 && p.y <= image.height - 1))
                return 0.0;
            const float v = image.sample(p.x, p.y);
            values[size_t(j) * size_t(size) + size_t(i)] = v;
            mean += v;
        }
    }
    mean /= double(values.size());
    double sq = 0;
    for (float& v : values) {
        v = float(v - mean);
        sq += double(v) * double(v);
    }
    *contrast = std::sqrt(sq / double(values.size()));
    return std::sqrt(sq);
}

} // namespace

PlanarTracker::PlanarTracker(PlanarSettings settings)
    : m_settings(settings)
{
    m_settings.gridSize = std::max(3, m_settings.gridSize);
    m_settings.searchRadius = std::max(2, m_settings.searchRadius);
}

bool PlanarTracker::reset(const GrayImage& frame, const Quad& corners)
{
    m_ready = false;
    if (frame.isEmpty() || !isConvexQuad(corners, kMinAreaPixels))
        return false;
    m_startCorners = corners;
    m_corners = corners;
    m_template = frame;
    m_templateHalf = frame.halved();
    m_templateMap = Homography();
    m_motion = Homography();

    const double side
        = (sideLength(corners[0], corners[1]) + sideLength(corners[1], corners[2])
              + sideLength(corners[2], corners[3]) + sideLength(corners[3], corners[0]))
        / 4;
    m_patch = m_settings.patchSize > 0 ? (std::max(5, m_settings.patchSize) | 1)
                                       : oddClamped(side * kPatchFraction);

    // Lay the grid on the plane (not on the screen): through the homography of the unit square,
    // so a region seen in perspective gets its patches spread evenly over the surface.
    const Quad unit {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    const std::optional<Homography> plane = homographyFromQuads(unit, corners);
    if (!plane)
        return false;
    m_points.clear();
    std::vector<float> values;
    for (int j = 0; j < m_settings.gridSize; ++j) {
        for (int i = 0; i < m_settings.gridSize; ++i) {
            const double u = kGridMargin + (1 - 2 * kGridMargin) * i / (m_settings.gridSize - 1);
            const double v = kGridMargin + (1 - 2 * kGridMargin) * j / (m_settings.gridSize - 1);
            const Vec2 p = plane->map({u, v});
            double contrast = 0;
            if (warpedPatch(frame, Homography(), p, m_patch, values, &contrast) > 0
                && contrast >= kMinContrast)
                m_points.push_back(p);
        }
    }
    m_ready = int(m_points.size()) >= kMinPatches;
    return m_ready;
}

void PlanarTracker::matchPatches(const GrayImage& next, const GrayImage& nextHalf,
    const Homography& predicted, int radius, double minCorrelation, std::vector<Vec2>& from,
    std::vector<Vec2>& to, std::vector<double>& correlation) const
{
    const std::optional<Homography> back = predicted.inverse();
    if (!back)
        return;
    // Current-frame pixel -> template pixel: undo the predicted motion, then go to the template.
    const Homography toTemplate = m_templateMap.after(*back);
    std::vector<float> values;
    for (const Vec2& p : m_points) {
        const Vec2 q = predicted.map(p);
        if (!(std::abs(q.x) < 1e7 && std::abs(q.y) < 1e7))
            continue;
        double contrast = 0;
        const double norm = warpedPatch(m_template, toTemplate, q, m_patch, values, &contrast);
        if (norm <= 0 || contrast < kMinContrast)
            continue;
        int bx = int(std::lround(q.x)), by = int(std::lround(q.y));
        double best = -2;
        int searchRadius = radius;
        if (radius > kCoarseAbove) {
            // Coarse: the same patch at half resolution, searched on the half-size picture.
            const int coarse = std::max(kMinCoarsePatch, (m_patch / 2)) | 1;
            std::vector<float> coarseValues;
            double coarseContrast = 0;
            const double coarseNorm = warpedPatch(
                m_templateHalf, toTemplate, q, coarse, coarseValues, &coarseContrast, 2.0);
            int cx = 0, cy = 0;
            double cbest = -2;
            if (coarseNorm > 0
                && detail::bestInteger(nextHalf, int(std::lround(q.x / 2)),
                    int(std::lround(q.y / 2)), radius / 2 + 1, coarseValues, coarse, coarseNorm,
                    &cx, &cy, &cbest)) {
                bx = cx * 2;
                by = cy * 2;
                searchRadius = kCoarseToFineRadius;
            }
        }
        if (!detail::bestInteger(
                next, bx, by, searchRadius, values, m_patch, norm, &bx, &by, &best))
            continue;
        Vec2 pos {double(bx), double(by)};
        const Vec2 fine = detail::refine(next, pos, values, m_patch, norm, kRefineMaxMove);
        double corr = detail::correlationReal(next, fine, values, m_patch, norm);
        if (corr >= best)
            pos = fine;
        else
            corr = best; // the refinement never makes a match worse
        if (corr < minCorrelation)
            continue;
        from.push_back(p);
        to.push_back(pos);
        correlation.push_back(corr);
    }
}

PlanarResult PlanarTracker::track(const GrayImage& next)
{
    PlanarResult result;
    result.corners = m_corners;
    result.motion = m_motion;
    if (!m_ready || next.isEmpty())
        return result;

    const GrayImage nextHalf = next.halved();
    Homography current = m_motion;
    std::vector<bool> inliers;
    std::vector<double> correlation; ///< of the matches of the latest pass, parallel to inliers
    std::vector<Vec2> matchFrom, matchTo; ///< the latest pass's matches, parallel to inliers
    int matched = 0;
    bool coarsePass = true;
    for (const int radius : {m_settings.searchRadius, kFinePassRadius}) {
        std::vector<Vec2> from, to;
        std::vector<double> corr;
        matchPatches(next, nextHalf, current, radius,
            coarsePass ? m_settings.minCorrelation
                       : std::max(m_settings.minCorrelation, kFinePassMinCorrelation),
            from, to, corr);
        if (!coarsePass && int(from.size()) < std::max(kMinPatches, int(inliers.size() / 2))) {
            // Too few good matches for the strict pass: match again with the normal limit.
            from.clear();
            to.clear();
            corr.clear();
            matchPatches(
                next, nextHalf, current, radius, m_settings.minCorrelation, from, to, corr);
        }
        coarsePass = false;
        matched = int(from.size());
        if (matched < kMinPatches)
            return result;
        RansacSettings ransac;
        ransac.inlierThreshold = m_settings.inlierThreshold;
        const std::optional<HomographyFit> fit = estimateHomography(from, to, ransac);
        if (!fit || fit->inlierCount < kMinPatches
            || fit->inlierCount < m_settings.minInlierFraction * double(m_points.size()))
            return result;
        current = fit->h;
        inliers = fit->inliers;
        correlation = corr;
        matchFrom = std::move(from);
        matchTo = std::move(to);
    }

    Quad found;
    for (size_t i = 0; i < 4; ++i)
        found[i] = current.map(m_startCorners[i]);
    const double area = quadArea(found), startArea = quadArea(m_startCorners);
    if (!isConvexQuad(found, kMinAreaPixels) || area > startArea * kAreaChangeLimit
        || area < startArea / kAreaChangeLimit)
        return result;

    double sum = 0;
    int count = 0;
    for (size_t i = 0; i < inliers.size(); ++i) {
        if (inliers[i]) {
            sum += correlation[i];
            ++count;
            result.inlierFrom.push_back(matchFrom[i]);
            result.inlierTo.push_back(matchTo[i]);
        }
    }
    result.ok = true;
    result.corners = found;
    result.motion = current;
    result.inliers = count;
    result.patches = matched;
    result.correlation = count > 0 ? sum / count : 0;
    m_corners = found;
    m_motion = current;
    if (result.correlation < m_settings.refreshBelow) {
        // The look has changed: cut the patches from this frame from now on.
        m_template = next;
        m_templateHalf = nextHalf;
        m_templateMap = current;
    }
    return result;
}

} // namespace kk::tracking
