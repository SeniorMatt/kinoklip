// SPDX-License-Identifier: GPL-3.0-or-later
#include "compositing/tracking/MeshTracker.h"

#include <algorithm>
#include <cmath>

namespace kk::tracking {

namespace {

/// Mean of the known 4-neighbours of grid cell (c, r); false when none is known.
bool neighbourMean(const std::vector<Vec2>& v, const std::vector<bool>& known, int cols, int rows,
    int c, int r, Vec2* out)
{
    double sx = 0, sy = 0;
    int n = 0;
    const int dc[4] = {-1, 1, 0, 0}, dr[4] = {0, 0, -1, 1};
    for (int i = 0; i < 4; ++i) {
        const int cc = c + dc[i], rr = r + dr[i];
        if (cc < 0 || rr < 0 || cc >= cols || rr >= rows)
            continue;
        const size_t j = size_t(rr) * size_t(cols) + size_t(cc);
        if (!known[j])
            continue;
        sx += v[j].x;
        sy += v[j].y;
        ++n;
    }
    if (n == 0)
        return false;
    *out = {sx / n, sy / n};
    return true;
}

} // namespace

void smoothBend(
    std::vector<Vec2>& bend, std::vector<bool>& known, int cols, int rows, double smoothing)
{
    if (bend.size() != size_t(cols) * size_t(rows) || known.size() != bend.size())
        return;
    // Unknown points: the mean of known neighbours, spreading inwards pass by pass.
    for (int pass = 0; pass < cols + rows; ++pass) {
        bool any = false;
        std::vector<Vec2> filled = bend;
        std::vector<bool> nowKnown = known;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const size_t i = size_t(r) * size_t(cols) + size_t(c);
                if (known[i])
                    continue;
                Vec2 m;
                if (neighbourMean(bend, known, cols, rows, c, r, &m)) {
                    filled[i] = m;
                    nowKnown[i] = true;
                    any = true;
                }
            }
        }
        bend = std::move(filled);
        known = std::move(nowKnown);
        if (!any)
            break;
    }
    const double s = std::clamp(smoothing, 0.0, 1.0);
    for (int iteration = 0; iteration < 3 && s > 0; ++iteration) {
        std::vector<Vec2> next = bend;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const size_t i = size_t(r) * size_t(cols) + size_t(c);
                Vec2 m;
                if (neighbourMean(bend, known, cols, rows, c, r, &m)) {
                    next[i].x = (1 - s) * bend[i].x + s * m.x;
                    next[i].y = (1 - s) * bend[i].y + s * m.y;
                }
            }
        }
        bend = std::move(next);
    }
}

MeshTracker::MeshTracker(MeshSettings settings)
    : m_settings(settings)
{
    m_settings.cols = std::max(2, m_settings.cols);
    m_settings.rows = std::max(2, m_settings.rows);
}

bool MeshTracker::reset(const GrayImage& frame, const std::vector<Vec2>& points)
{
    const size_t n = size_t(m_settings.cols) * size_t(m_settings.rows);
    m_ready = false;
    if (points.size() != n || frame.isEmpty())
        return false;
    TrackerSettings ts;
    ts.patternSize = std::max(7, m_settings.patchSize | 1);
    ts.searchSize = std::max(ts.patternSize, m_settings.searchSize);
    ts.minCorrelation = m_settings.minCorrelation;
    m_trackers.assign(n, PointTracker(ts));
    m_usable.assign(n, false);
    int usable = 0;
    for (size_t i = 0; i < n; ++i) {
        m_usable[i] = m_trackers[i].reset(frame, points[i]);
        usable += m_usable[i] ? 1 : 0;
    }
    m_rest = points;
    m_points = points;
    m_ready = usable >= 4;
    return m_ready;
}

MeshResult MeshTracker::track(const GrayImage& next)
{
    MeshResult result;
    result.points = m_points;
    if (!m_ready)
        return result;
    const size_t n = m_rest.size();
    std::vector<Vec2> found(n);
    std::vector<bool> matched(n, false);
    std::vector<double> corr(n, 0.0);
    std::vector<Vec2> from, to;
    std::vector<size_t> index;
    for (size_t i = 0; i < n; ++i) {
        found[i] = m_points[i];
        if (!m_usable[i])
            continue;
        const TrackResult r = m_trackers[i].track(next);
        if (r.ok) {
            matched[i] = true;
            found[i] = r.position;
            corr[i] = r.correlation;
            from.push_back(m_rest[i]);
            to.push_back(r.position);
            index.push_back(i);
        }
    }
    const int minGood = std::max(4, int(std::ceil(m_settings.minGoodFraction * double(n))));
    if (int(from.size()) < minGood)
        return result;
    RansacSettings ransac;
    ransac.inlierThreshold = 2.5;
    const auto fit = estimateHomography(from, to, ransac);
    if (!fit || fit->inlierCount < 4)
        return result;
    // The bend: what the homography does not explain; only the agreeing points are trusted.
    std::vector<Vec2> bend(n);
    std::vector<bool> known(n, false);
    std::vector<Vec2> surface(n);
    double corrSum = 0;
    int good = 0;
    for (size_t i = 0; i < n; ++i)
        surface[i] = fit->h.map(m_rest[i]);
    for (size_t k = 0; k < index.size(); ++k) {
        const size_t i = index[k];
        bend[i] = {found[i].x - surface[i].x, found[i].y - surface[i].y};
        known[i] = true;
    }
    // A bend is smooth: a matched point whose bend jumps away from its matched neighbours' mean
    // matched a wrong spot, and is carried along by the others instead.
    {
        const double jump = std::max(8.0, 0.5 * double(m_settings.patchSize));
        const std::vector<bool> matchedNow = known;
        for (int r = 0; r < m_settings.rows; ++r) {
            for (int c = 0; c < m_settings.cols; ++c) {
                const size_t i = size_t(r) * size_t(m_settings.cols) + size_t(c);
                if (!matchedNow[i])
                    continue;
                std::vector<bool> others = matchedNow;
                others[i] = false;
                int n = 0;
                const int dc[4] = {-1, 1, 0, 0}, dr[4] = {0, 0, -1, 1};
                for (int q = 0; q < 4; ++q) {
                    const int cc = c + dc[q], rr = r + dr[q];
                    if (cc >= 0 && rr >= 0 && cc < m_settings.cols && rr < m_settings.rows
                        && matchedNow[size_t(rr) * size_t(m_settings.cols) + size_t(cc)])
                        ++n;
                }
                Vec2 m;
                if (n >= 2 && neighbourMean(bend, others, m_settings.cols, m_settings.rows, c, r, &m)
                    && std::hypot(bend[i].x - m.x, bend[i].y - m.y) > jump)
                    known[i] = false;
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (known[i]) {
            corrSum += corr[i];
            ++good;
        }
    }
    const std::vector<bool> trusted = known;
    smoothBend(bend, known, m_settings.cols, m_settings.rows, m_settings.smoothing);
    for (size_t i = 0; i < n; ++i) {
        result.points[i] = {surface[i].x + bend[i].x, surface[i].y + bend[i].y};
        // A point that was lost or disagrees is carried along by the others.
        if (!trusted[i] && m_usable[i])
            m_trackers[i].setPosition(result.points[i]);
    }
    m_points = result.points;
    result.ok = true;
    result.good = good;
    result.correlation = good ? corrSum / good : 0.0;
    return result;
}

} // namespace kk::tracking
