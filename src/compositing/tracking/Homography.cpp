// SPDX-License-Identifier: GPL-3.0-or-later
// Homography implementation. See the header.
#include "compositing/tracking/Homography.h"

#include <algorithm>
#include <cmath>

namespace kk::tracking {

namespace {

constexpr double kFarAway = 1e12; ///< where a point sent to infinity is put instead
constexpr double kMinW = 1e-12; ///< |w| below this counts as "at infinity"
constexpr int kJacobiSweeps = 60;
/// Second-smallest eigenvalue of the DLT system relative to the largest: below this the map is
/// not determined by the points (all on one line, or fewer than four distinct ones).
constexpr double kDegenerateRatio = 1e-9;
constexpr double kCollinearSine
    = 0.02; ///< sine of the angle under which three points are "in line"

using Mat3 = std::array<double, 9>;

Mat3 multiply(const Mat3& a, const Mat3& b)
{
    Mat3 r {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k)
                s += a[size_t(i * 3 + k)] * b[size_t(k * 3 + j)];
            r[size_t(i * 3 + j)] = s;
        }
    }
    return r;
}

/// Moves the points to their centroid and scales them to a mean distance of sqrt(2); returns
/// the matrix doing so, or nullopt if all points are the same.
std::optional<Mat3> normalisation(const std::vector<Vec2>& pts)
{
    Vec2 c;
    for (const Vec2& p : pts) {
        c.x += p.x;
        c.y += p.y;
    }
    c.x /= double(pts.size());
    c.y /= double(pts.size());
    double d = 0;
    for (const Vec2& p : pts)
        d += std::hypot(p.x - c.x, p.y - c.y);
    d /= double(pts.size());
    if (d < 1e-9)
        return std::nullopt;
    const double s = std::sqrt(2.0) / d;
    return Mat3 {s, 0, -s * c.x, 0, s, -s * c.y, 0, 0, 1};
}

/// Eigen decomposition of the symmetric n x n matrix @p a (row-major, destroyed) by cyclic
/// Jacobi rotations. Eigenvalues land in @p values, eigenvectors are the columns of @p vectors.
/// Plenty fast for the 9 x 9 DLT matrix, and unlike an SVD it is short enough to read.
void jacobiEigen(
    std::vector<double>& a, int n, std::vector<double>& values, std::vector<double>& vectors)
{
    vectors.assign(size_t(n) * size_t(n), 0.0);
    for (int i = 0; i < n; ++i)
        vectors[size_t(i * n + i)] = 1.0;
    for (int sweep = 0; sweep < kJacobiSweeps; ++sweep) {
        double off = 0;
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j)
                off += a[size_t(i * n + j)] * a[size_t(i * n + j)];
        }
        if (off < 1e-30)
            break;
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                const double apq = a[size_t(p * n + q)];
                if (std::abs(apq) < 1e-300)
                    continue;
                const double theta = (a[size_t(q * n + q)] - a[size_t(p * n + p)]) / (2 * apq);
                const double t
                    = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < n; ++k) { // columns p and q
                    const double akp = a[size_t(k * n + p)], akq = a[size_t(k * n + q)];
                    a[size_t(k * n + p)] = c * akp - s * akq;
                    a[size_t(k * n + q)] = s * akp + c * akq;
                }
                for (int k = 0; k < n; ++k) { // rows p and q
                    const double apk = a[size_t(p * n + k)], aqk = a[size_t(q * n + k)];
                    a[size_t(p * n + k)] = c * apk - s * aqk;
                    a[size_t(q * n + k)] = s * apk + c * aqk;
                }
                for (int k = 0; k < n; ++k) {
                    const double vkp = vectors[size_t(k * n + p)], vkq = vectors[size_t(k * n + q)];
                    vectors[size_t(k * n + p)] = c * vkp - s * vkq;
                    vectors[size_t(k * n + q)] = s * vkp + c * vkq;
                }
            }
        }
    }
    values.resize(size_t(n));
    for (int i = 0; i < n; ++i)
        values[size_t(i)] = a[size_t(i * n + i)];
}

bool finite(Vec2 p)
{
    return std::isfinite(p.x) && std::isfinite(p.y);
}

double cross(Vec2 o, Vec2 a, Vec2 b)
{
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

/// True if the three points are nearly on one line (or two of them coincide).
bool nearlyCollinear(Vec2 a, Vec2 b, Vec2 c)
{
    const double lab = std::hypot(b.x - a.x, b.y - a.y), lac = std::hypot(c.x - a.x, c.y - a.y);
    if (lab < 1e-9 || lac < 1e-9)
        return true;
    return std::abs(cross(a, b, c)) < kCollinearSine * lab * lac;
}

bool sampleIsDegenerate(const std::vector<Vec2>& pts, const int idx[4])
{
    for (int skip = 0; skip < 4; ++skip) {
        Vec2 t[3];
        int n = 0;
        for (int i = 0; i < 4; ++i) {
            if (i != skip)
                t[n++] = pts[size_t(idx[i])];
        }
        if (nearlyCollinear(t[0], t[1], t[2]))
            return true;
    }
    return false;
}

/// xorshift32: a tiny deterministic generator, so a fit is reproducible.
uint32_t nextRandom(uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

double distance(Vec2 a, Vec2 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

} // namespace

// ---- Homography ----

Vec2 Homography::map(Vec2 p) const
{
    const double w = m[6] * p.x + m[7] * p.y + m[8];
    const double x = m[0] * p.x + m[1] * p.y + m[2];
    const double y = m[3] * p.x + m[4] * p.y + m[5];
    if (std::abs(w) < kMinW) {
        const double len = std::max(std::hypot(x, y), 1e-300);
        return {x / len * kFarAway, y / len * kFarAway};
    }
    return {x / w, y / w};
}

std::optional<Homography> Homography::inverse() const
{
    // Adjugate over determinant; the scale of the result does not matter for a projective map
    // but dividing keeps entries comparable.
    const auto& a = m;
    const double c00 = a[4] * a[8] - a[5] * a[7], c01 = a[5] * a[6] - a[3] * a[8],
                 c02 = a[3] * a[7] - a[4] * a[6];
    const double det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    const double scale = std::max({std::abs(a[0]), std::abs(a[4]), std::abs(a[8]), 1e-300});
    if (!std::isfinite(det) || std::abs(det) < 1e-14 * scale * scale * scale)
        return std::nullopt;
    Homography r;
    r.m = {c00 / det, (a[2] * a[7] - a[1] * a[8]) / det, (a[1] * a[5] - a[2] * a[4]) / det,
        c01 / det, (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det, c02 / det,
        (a[1] * a[6] - a[0] * a[7]) / det, (a[0] * a[4] - a[1] * a[3]) / det};
    return r;
}

Homography Homography::after(const Homography& first) const
{
    Homography r;
    r.m = multiply(m, first.m);
    return r;
}

Homography Homography::normalised() const
{
    Homography r = *this;
    double d = m[8];
    if (std::abs(d) < 1e-12) {
        d = 0;
        for (double v : m)
            d += v * v;
        d = std::sqrt(d);
    }
    if (d > 0) {
        for (double& v : r.m)
            v /= d;
    }
    return r;
}

bool Homography::isFinite() const
{
    return std::all_of(m.begin(), m.end(), [](double v) { return std::isfinite(v); });
}

// ---- Estimation ----

std::optional<Homography> homographyFromPoints(
    const std::vector<Vec2>& from, const std::vector<Vec2>& to)
{
    const size_t n = from.size();
    if (n < 4 || to.size() != n)
        return std::nullopt;
    for (size_t i = 0; i < n; ++i) {
        if (!finite(from[i]) || !finite(to[i]))
            return std::nullopt;
    }
    const std::optional<Mat3> ta = normalisation(from), tb = normalisation(to);
    if (!ta || !tb)
        return std::nullopt;

    // Accumulate AtA of the 2n x 9 DLT matrix directly (9 x 9 whatever n is).
    std::vector<double> ata(81, 0.0);
    auto addRow = [&ata](const double row[9]) {
        for (int i = 0; i < 9; ++i) {
            for (int j = 0; j < 9; ++j)
                ata[size_t(i * 9 + j)] += row[i] * row[j];
        }
    };
    for (size_t i = 0; i < n; ++i) {
        const double x = (*ta)[0] * from[i].x + (*ta)[2], y = (*ta)[4] * from[i].y + (*ta)[5];
        const double u = (*tb)[0] * to[i].x + (*tb)[2], v = (*tb)[4] * to[i].y + (*tb)[5];
        const double r1[9] = {-x, -y, -1, 0, 0, 0, u * x, u * y, u};
        const double r2[9] = {0, 0, 0, -x, -y, -1, v * x, v * y, v};
        addRow(r1);
        addRow(r2);
    }
    std::vector<double> values, vectors;
    jacobiEigen(ata, 9, values, vectors);
    std::vector<int> order(9);
    for (int i = 0; i < 9; ++i)
        order[size_t(i)] = i;
    std::sort(order.begin(), order.end(),
        [&](int a, int b) { return values[size_t(a)] < values[size_t(b)]; });
    const double largest = std::max(values[size_t(order[8])], 1e-300);
    if (values[size_t(order[1])] < kDegenerateRatio * largest)
        return std::nullopt; // the null space is more than one-dimensional: not determined

    Mat3 hn;
    for (int i = 0; i < 9; ++i)
        hn[size_t(i)] = vectors[size_t(i * 9 + order[0])];
    const std::optional<Homography> tbInverse = Homography {*tb}.inverse();
    if (!tbInverse)
        return std::nullopt;
    Homography h;
    h.m = multiply(multiply(tbInverse->m, hn), *ta);
    h = h.normalised();
    if (!h.isFinite())
        return std::nullopt;
    return h;
}

std::optional<Homography> homographyFromQuads(const Quad& from, const Quad& to)
{
    return homographyFromPoints(
        std::vector<Vec2>(from.begin(), from.end()), std::vector<Vec2>(to.begin(), to.end()));
}

std::optional<HomographyFit> estimateHomography(
    const std::vector<Vec2>& from, const std::vector<Vec2>& to, const RansacSettings& settings)
{
    const size_t n = from.size();
    if (n < 4 || to.size() != n)
        return std::nullopt;
    for (size_t i = 0; i < n; ++i) {
        if (!finite(from[i]) || !finite(to[i]))
            return std::nullopt;
    }
    const double threshold = std::max(1e-6, settings.inlierThreshold);

    auto score = [&](const Homography& h, std::vector<bool>& flags, double* errorSum) {
        int count = 0;
        double sum = 0;
        flags.assign(n, false);
        for (size_t i = 0; i < n; ++i) {
            const double e = distance(h.map(from[i]), to[i]);
            if (e <= threshold) {
                flags[i] = true;
                ++count;
                sum += e;
            }
        }
        *errorSum = sum;
        return count;
    };

    uint32_t rng = settings.seed ? settings.seed : 1u;
    Homography bestH;
    std::vector<bool> bestFlags;
    int bestCount = 0;
    double bestError = 0;
    std::vector<bool> flags;
    for (int it = 0; it < std::max(1, settings.iterations); ++it) {
        int idx[4];
        for (int k = 0; k < 4; ++k) { // four distinct indices
            bool again = true;
            while (again) {
                idx[k] = int(nextRandom(rng) % n);
                again = false;
                for (int j = 0; j < k; ++j)
                    again = again || idx[j] == idx[k];
            }
        }
        if (sampleIsDegenerate(from, idx) || sampleIsDegenerate(to, idx))
            continue;
        std::vector<Vec2> a, b;
        for (int k : idx) {
            a.push_back(from[size_t(k)]);
            b.push_back(to[size_t(k)]);
        }
        const std::optional<Homography> h = homographyFromPoints(a, b);
        if (!h)
            continue;
        double err = 0;
        const int count = score(*h, flags, &err);
        if (count > bestCount || (count == bestCount && count > 0 && err < bestError)) {
            bestCount = count;
            bestError = err;
            bestH = *h;
            bestFlags = flags;
        }
    }
    if (bestCount < 4)
        return std::nullopt;

    // Refit on all inliers until the inlier set stops growing.
    for (int round = 0; round < 6; ++round) {
        std::vector<Vec2> a, b;
        for (size_t i = 0; i < n; ++i) {
            if (bestFlags[i]) {
                a.push_back(from[i]);
                b.push_back(to[i]);
            }
        }
        const std::optional<Homography> h = homographyFromPoints(a, b);
        if (!h)
            break;
        double err = 0;
        const int count = score(*h, flags, &err);
        const bool better = count >= bestCount; // the least-squares refit is the better model
        if (!better)
            break;
        const bool same = flags == bestFlags;
        bestH = *h;
        bestFlags = flags;
        bestCount = count;
        bestError = err;
        if (same)
            break;
    }
    HomographyFit fit;
    fit.h = bestH;
    fit.inliers = bestFlags;
    fit.inlierCount = bestCount;
    fit.meanError = bestCount > 0 ? bestError / bestCount : 0;
    return fit;
}

// ---- Similarity ----

Vec2 Similarity::map(Vec2 p) const
{
    const double c = std::cos(angleRad) * scale, s = std::sin(angleRad) * scale;
    return {c * p.x - s * p.y + dx, s * p.x + c * p.y + dy};
}

Similarity Similarity::inverse() const
{
    Similarity r;
    r.scale = 1.0 / scale;
    r.angleRad = -angleRad;
    // p = (q - t) / (s R)  ->  translation is -R^-1 t / s.
    const double c = std::cos(r.angleRad) * r.scale, s = std::sin(r.angleRad) * r.scale;
    r.dx = -(c * dx - s * dy);
    r.dy = -(s * dx + c * dy);
    return r;
}

std::optional<Similarity> similarityFromPoints(
    const std::vector<Vec2>& from, const std::vector<Vec2>& to)
{
    const size_t n = from.size();
    if (n < 2 || to.size() != n)
        return std::nullopt;
    Vec2 ca, cb;
    for (size_t i = 0; i < n; ++i) {
        if (!finite(from[i]) || !finite(to[i]))
            return std::nullopt;
        ca.x += from[i].x;
        ca.y += from[i].y;
        cb.x += to[i].x;
        cb.y += to[i].y;
    }
    ca.x /= double(n);
    ca.y /= double(n);
    cb.x /= double(n);
    cb.y /= double(n);
    double dot = 0, crs = 0, varA = 0;
    for (size_t i = 0; i < n; ++i) {
        const double ax = from[i].x - ca.x, ay = from[i].y - ca.y;
        const double bx = to[i].x - cb.x, by = to[i].y - cb.y;
        dot += ax * bx + ay * by;
        crs += ax * by - ay * bx;
        varA += ax * ax + ay * ay;
    }
    if (varA < 1e-12)
        return std::nullopt;
    Similarity s;
    s.angleRad = std::atan2(crs, dot);
    s.scale = std::hypot(dot, crs) / varA;
    const double c = std::cos(s.angleRad) * s.scale, sn = std::sin(s.angleRad) * s.scale;
    s.dx = cb.x - (c * ca.x - sn * ca.y);
    s.dy = cb.y - (sn * ca.x + c * ca.y);
    if (s.scale < 1e-9)
        return std::nullopt;
    return s;
}

std::optional<Similarity> similarityFromQuads(const Quad& from, const Quad& to)
{
    return similarityFromPoints(
        std::vector<Vec2>(from.begin(), from.end()), std::vector<Vec2>(to.begin(), to.end()));
}

// ---- Quads ----

double quadArea(const Quad& q)
{
    double a = 0;
    for (size_t i = 0; i < 4; ++i) {
        const Vec2 &p = q[i], &r = q[(i + 1) % 4];
        a += p.x * r.y - r.x * p.y;
    }
    return std::abs(a) / 2;
}

bool isConvexQuad(const Quad& q, double minArea)
{
    for (const Vec2& p : q) {
        if (!finite(p))
            return false;
    }
    double sign = 0;
    for (size_t i = 0; i < 4; ++i) {
        const double c = cross(q[i], q[(i + 1) % 4], q[(i + 2) % 4]);
        if (std::abs(c) < 1e-9)
            return false;
        if (sign == 0)
            sign = c > 0 ? 1 : -1;
        else if ((c > 0 ? 1 : -1) != sign)
            return false;
    }
    return quadArea(q) >= minArea;
}

Vec2 quadCentre(const Quad& q)
{
    // Intersection of p0-p2 and p1-p3: p0 + t (p2 - p0).
    const double d1x = q[2].x - q[0].x, d1y = q[2].y - q[0].y;
    const double d2x = q[3].x - q[1].x, d2y = q[3].y - q[1].y;
    const double den = d1x * d2y - d1y * d2x;
    if (std::abs(den) < 1e-12)
        return {(q[0].x + q[1].x + q[2].x + q[3].x) / 4, (q[0].y + q[1].y + q[2].y + q[3].y) / 4};
    const double t = ((q[1].x - q[0].x) * d2y - (q[1].y - q[0].y) * d2x) / den;
    return {q[0].x + t * d1x, q[0].y + t * d1y};
}

} // namespace kk::tracking
