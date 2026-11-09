// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests of the planar tracker's maths (src/compositing/tracking/Homography): the normalised
// DLT against known maps (exact, noisy, large pixel coordinates), RANSAC against outliers,
// similarity fits, quad helpers, and degenerate input that must not crash.
#include "compositing/tracking/Homography.h"

#include <QTest>

#include <cmath>
#include <random>

using namespace kk::tracking;

namespace {

/// A perspective map with rotation, scale, translation and a visible keystone.
Homography knownMap()
{
    Homography h;
    h.m = {1.08, -0.21, 37.0, 0.17, 0.97, -12.0, 3.2e-4, -1.9e-4, 1.0};
    return h;
}

std::vector<Vec2> scatter(int count, unsigned seed, double w = 1280, double h = 720)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> ux(0, w), uy(0, h);
    std::vector<Vec2> pts;
    for (int i = 0; i < count; ++i)
        pts.push_back({ux(rng), uy(rng)});
    return pts;
}

std::vector<Vec2> mapAll(const Homography& h, const std::vector<Vec2>& pts)
{
    std::vector<Vec2> out;
    for (const Vec2& p : pts)
        out.push_back(h.map(p));
    return out;
}

double maxError(const Homography& got, const Homography& want, const std::vector<Vec2>& probe)
{
    double worst = 0;
    for (const Vec2& p : probe) {
        const Vec2 a = got.map(p), b = want.map(p);
        worst = std::max(worst, std::hypot(a.x - b.x, a.y - b.y));
    }
    return worst;
}

} // namespace

class TestHomography : public QObject
{
    Q_OBJECT
private slots:
    void fourPointsGiveTheExactMap()
    {
        const Homography want = knownMap();
        const std::vector<Vec2> from {{100, 80}, {900, 120}, {880, 600}, {60, 640}};
        const auto got = homographyFromPoints(from, mapAll(want, from));
        QVERIFY(got);
        QVERIFY2(maxError(*got, want, scatter(200, 3)) < 1e-6,
            qPrintable(QString::number(maxError(*got, want, scatter(200, 3)))));
    }

    void manyNoisyPointsAverageTheNoiseOut()
    {
        const Homography want = knownMap();
        const std::vector<Vec2> from = scatter(80, 7);
        std::vector<Vec2> to = mapAll(want, from);
        std::mt19937 rng(9);
        std::normal_distribution<double> noise(0.0, 0.3);
        for (Vec2& p : to) {
            p.x += noise(rng);
            p.y += noise(rng);
        }
        const auto got = homographyFromPoints(from, to);
        QVERIFY(got);
        QVERIFY2(maxError(*got, want, scatter(200, 5)) < 0.4,
            qPrintable(QString::number(maxError(*got, want, scatter(200, 5)))));
    }

    void identityAndTranslationAreRecovered()
    {
        const std::vector<Vec2> from {{10, 10}, {600, 20}, {590, 400}, {5, 380}, {300, 200}};
        const auto same = homographyFromPoints(from, from);
        QVERIFY(same);
        QVERIFY(maxError(*same, Homography(), from) < 1e-9);
        std::vector<Vec2> moved = from;
        for (Vec2& p : moved) {
            p.x += 12.5;
            p.y -= 3.25;
        }
        const auto t = homographyFromPoints(from, moved);
        QVERIFY(t);
        QVERIFY(maxError(*t, Homography {{1, 0, 12.5, 0, 1, -3.25, 0, 0, 1}}, from) < 1e-8);
    }

    void inverseAndCompositionAgree()
    {
        const Homography h = knownMap();
        const auto inv = h.inverse();
        QVERIFY(inv);
        const Homography round = inv->after(h);
        QVERIFY(maxError(round, Homography(), scatter(50, 11)) < 1e-6);
        Homography singular;
        singular.m = {1, 2, 3, 2, 4, 6, 0, 0, 1};
        QVERIFY(!singular.inverse());
    }

    void aPointAtInfinityDoesNotProduceNan()
    {
        Homography h;
        h.m = {1, 0, 0, 0, 1, 0, 1, 0, 0}; // w = x: the line x = 0 goes to infinity
        const Vec2 p = h.map({0, 5});
        QVERIFY(std::isfinite(p.x) && std::isfinite(p.y));
    }

    void ransacRejectsOutliers()
    {
        const Homography want = knownMap();
        const std::vector<Vec2> from = scatter(100, 21);
        std::vector<Vec2> to = mapAll(want, from);
        std::mt19937 rng(22);
        std::normal_distribution<double> noise(0.0, 0.25);
        std::uniform_real_distribution<double> wild(0, 1000);
        std::vector<bool> isOutlier(from.size(), false);
        for (size_t i = 0; i < to.size(); ++i) {
            if (i % 10 < 4) { // 40 % of the pairs are garbage
                to[i] = {wild(rng), wild(rng)};
                isOutlier[i] = true;
            } else {
                to[i].x += noise(rng);
                to[i].y += noise(rng);
            }
        }
        const auto fit = estimateHomography(from, to);
        QVERIFY(fit);
        QVERIFY2(maxError(fit->h, want, scatter(200, 23)) < 0.3,
            qPrintable(QString::number(maxError(fit->h, want, scatter(200, 23)))));
        QCOMPARE(fit->inliers.size(), from.size());
        int wrong = 0;
        for (size_t i = 0; i < from.size(); ++i)
            wrong += fit->inliers[i] == isOutlier[i];
        QVERIFY2(wrong <= 1, qPrintable(QString::number(wrong)));
        QVERIFY(fit->inlierCount >= 58 && fit->inlierCount <= 61);
        QVERIFY(fit->meanError < 0.5);
    }

    void ransacIsDeterministic()
    {
        const std::vector<Vec2> from = scatter(40, 31);
        std::vector<Vec2> to = mapAll(knownMap(), from);
        for (size_t i = 0; i < 12; ++i)
            to[i] = {double(i) * 77, 5.0 * double(i)};
        const auto a = estimateHomography(from, to), b = estimateHomography(from, to);
        QVERIFY(a && b);
        QCOMPARE(a->h.m, b->h.m);
        QCOMPARE(a->inliers, b->inliers);
    }

    void degenerateInputDoesNotCrash()
    {
        const std::vector<Vec2> three {{0, 0}, {10, 0}, {0, 10}};
        QVERIFY(!homographyFromPoints(three, three));
        QVERIFY(!homographyFromPoints({}, {}));
        QVERIFY(!homographyFromPoints(three, {{0, 0}}));
        const std::vector<Vec2> line {{0, 0}, {10, 10}, {20, 20}, {30, 30}, {40, 40}};
        QVERIFY(!homographyFromPoints(line, line)); // collinear: the map is not determined
        const std::vector<Vec2> same(6, Vec2 {5, 5});
        QVERIFY(!homographyFromPoints(same, same));
        const double nan = std::nan("");
        const std::vector<Vec2> bad {{0, 0}, {10, 0}, {10, 10}, {nan, 10}};
        QVERIFY(!homographyFromPoints(bad, bad));
        QVERIFY(!estimateHomography(three, three));
        QVERIFY(!estimateHomography(line, line));
        QVERIFY(!estimateHomography(same, same));
        QVERIFY(!estimateHomography(bad, bad));
        // Pure noise: either no model or one with few inliers; never a crash or a NaN.
        const auto junk = estimateHomography(scatter(30, 41), scatter(30, 42));
        if (junk)
            QVERIFY(junk->h.isFinite() && junk->inlierCount < 30);
        QVERIFY(!similarityFromPoints({{1, 1}}, {{2, 2}}));
        QVERIFY(!similarityFromPoints(same, same));
    }

    void similarityIsRecovered()
    {
        Similarity want;
        want.scale = 1.37;
        want.angleRad = 0.41;
        want.dx = -22.0;
        want.dy = 91.5;
        const std::vector<Vec2> from = scatter(30, 51);
        std::vector<Vec2> to;
        for (const Vec2& p : from)
            to.push_back(want.map(p));
        const auto got = similarityFromPoints(from, to);
        QVERIFY(got);
        QVERIFY(std::abs(got->scale - want.scale) < 1e-9);
        QVERIFY(std::abs(got->angleRad - want.angleRad) < 1e-9);
        QVERIFY(std::abs(got->dx - want.dx) < 1e-6 && std::abs(got->dy - want.dy) < 1e-6);
        const Similarity back = got->inverse();
        for (const Vec2& p : from) {
            const Vec2 q = back.map(got->map(p));
            QVERIFY(std::hypot(q.x - p.x, q.y - p.y) < 1e-6);
        }
    }

    void similarityOfAPerspectiveQuadIsTheBestFit()
    {
        const Quad ref {{{100, 100}, {300, 100}, {300, 250}, {100, 250}}};
        // A pure shift of 30 px right: the fit must be exactly that, whatever the quad.
        Quad moved = ref;
        for (Vec2& p : moved)
            p.x += 30;
        const auto s = similarityFromQuads(ref, moved);
        QVERIFY(s);
        QVERIFY(std::abs(s->scale - 1) < 1e-9 && std::abs(s->angleRad) < 1e-9);
        QVERIFY(std::abs(s->dx - 30) < 1e-9 && std::abs(s->dy) < 1e-9);
    }

    void quadHelpers()
    {
        const Quad box {{{0, 0}, {100, 0}, {100, 50}, {0, 50}}};
        QVERIFY(isConvexQuad(box));
        QCOMPARE(quadArea(box), 5000.0);
        const Vec2 c = quadCentre(box);
        QVERIFY(std::abs(c.x - 50) < 1e-9 && std::abs(c.y - 25) < 1e-9);
        const Quad bowtie {{{0, 0}, {100, 50}, {100, 0}, {0, 50}}};
        QVERIFY(!isConvexQuad(bowtie));
        const Quad flat {{{0, 0}, {100, 0}, {200, 0}, {300, 0}}};
        QVERIFY(!isConvexQuad(flat));
        QVERIFY(!isConvexQuad(box, 6000.0)); // too small for the asked minimum
        // Perspective: the narrow edge (y = 0) is the far one, so the plane's centre lies nearer to
        // it than the average of the corners (y = 50): the diagonals cross at y = 100 / 6.
        const Quad trapezoid {{{40, 0}, {60, 0}, {100, 100}, {0, 100}}};
        const Vec2 t = quadCentre(trapezoid);
        QVERIFY(std::abs(t.y - 100.0 / 6) < 1e-9);
        QVERIFY(std::abs(t.x - 50) < 1e-9);
        const Quad nanQuad {{{0, 0}, {1, 0}, {1, 1}, {std::nan(""), 1}}};
        QVERIFY(!isConvexQuad(nanQuad, 0));
    }

    void quadToQuadMapsTheCorners()
    {
        const Quad a {{{0, 0}, {320, 0}, {320, 180}, {0, 180}}};
        const Quad b {{{40, 30}, {280, 10}, {300, 170}, {20, 150}}};
        const auto h = homographyFromQuads(a, b);
        QVERIFY(h);
        for (size_t i = 0; i < 4; ++i) {
            const Vec2 p = h->map(a[i]);
            QVERIFY(std::hypot(p.x - b[i].x, p.y - b[i].y) < 1e-6);
        }
    }
};

QTEST_APPLESS_MAIN(TestHomography)
#include "test_homography.moc"
