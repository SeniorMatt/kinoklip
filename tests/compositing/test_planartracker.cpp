// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests of the planar tracker (src/compositing/tracking/PlanarTracker): a textured plane
// that moves, turns, scales and tilts by known homographies, tracked forwards and backwards,
// with noise, a contrast change and an occluder, plus the failure cases (lost plane, flat
// region, bad region).
#include "compositing/tracking/PlanarTracker.h"

#include <QTest>

#include <cmath>
#include <random>

using namespace kk::tracking;

namespace {

constexpr int kW = 360;
constexpr int kH = 260;

struct Wave
{
    double kx, ky, phase, amplitude;
};

/// A fixed texture of random plane waves (wavelengths 6 to 30 px): smooth, without repeats, and
/// exact at any real position, so a warped frame has no resampling blur.
const std::vector<Wave>& waves()
{
    static const std::vector<Wave> list = [] {
        std::mt19937 rng(777);
        std::uniform_real_distribution<double> wl(6.0, 30.0), dir(0.0, 2 * M_PI), ph(0.0, 2 * M_PI);
        std::vector<Wave> w;
        for (int i = 0; i < 24; ++i) {
            const double k = 2 * M_PI / wl(rng), a = dir(rng);
            w.push_back({k * std::cos(a), k * std::sin(a), ph(rng), 0.085});
        }
        return w;
    }();
    return list;
}

double texture(double x, double y)
{
    double v = 0.5;
    for (const Wave& w : waves())
        v += w.amplitude * std::sin(w.kx * x + w.ky * y + w.phase);
    return v;
}

/// The plane at time @p t: the plane's own coordinates (the start frame's pixels) go to the
/// picture through this map. Move, turn, scale and a keystone that all grow with @p t.
Homography planeMap(double t)
{
    auto mat = [](std::array<double, 9> m) { return Homography {m}; };
    const double a = 0.012 * t, s = 1.0 + 0.004 * t;
    const double c = std::cos(a) * s, sn = std::sin(a) * s;
    const Homography keystone = mat({1, 0, 0, 0, 1, 0, 2.0e-5 * t, -1.2e-5 * t, 1});
    const Homography about = mat({1, 0, 180, 0, 1, 130, 0, 0, 1}); // back from the pivot
    const Homography rotate = mat({c, -sn, 0, sn, c, 0, 0, 0, 1});
    const Homography toPivot = mat({1, 0, -180, 0, 1, -130, 0, 0, 1});
    const Homography shift = mat({1, 0, 2.0 * t, 0, 1, -1.2 * t, 0, 0, 1});
    return shift.after(about.after(rotate.after(toPivot.after(keystone))));
}

/// The picture at time @p t (the plane fills it): texture(planeMap(t)^-1 p), then contrast
/// scaling, an optional grey @p occluder (picture pixels) and Gaussian noise.
GrayImage frame(double t, double noise = 0.0, double contrast = 1.0, const QRectF& occluder = {})
{
    GrayImage img;
    img.width = kW;
    img.height = kH;
    img.data.resize(size_t(kW) * kH);
    const Homography inv = *planeMap(t).inverse();
    std::mt19937 rng(unsigned(1000 + t * 10));
    std::normal_distribution<double> n(0.0, noise > 0 ? noise : 1.0);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Vec2 p = inv.map({double(x), double(y)});
            double v = 0.5 + (texture(p.x, p.y) - 0.5) * contrast;
            if (occluder.contains(x, y))
                v = 0.5;
            if (noise > 0)
                v += n(rng);
            img.data[size_t(y) * kW + x] = float(v);
        }
    }
    return img;
}

Quad startQuad()
{
    return {{{110, 70}, {250, 76}, {244, 180}, {116, 174}}};
}

Quad truth(double t)
{
    Quad q = startQuad();
    for (Vec2& p : q)
        p = planeMap(t).map(p);
    return q;
}

double worstCorner(const Quad& got, const Quad& want)
{
    double w = 0;
    for (size_t i = 0; i < 4; ++i)
        w = std::max(w, std::hypot(got[i].x - want[i].x, got[i].y - want[i].y));
    return w;
}

QString describe(double t, const Quad& got)
{
    return QStringLiteral("t=%1 worst corner error %2 px").arg(t).arg(worstCorner(got, truth(t)));
}

} // namespace

class TestPlanarTracker : public QObject
{
    Q_OBJECT
private slots:
    void followsAMovingTurningTiltingPlane()
    {
        PlanarTracker tracker;
        QVERIFY(tracker.reset(frame(0), startQuad()));
        for (int t = 1; t <= 10; ++t) {
            const PlanarResult r = tracker.track(frame(t));
            QVERIFY2(r.ok, qPrintable(QStringLiteral("lost at %1").arg(t)));
            QVERIFY2(worstCorner(r.corners, truth(t)) < 0.5, qPrintable(describe(t, r.corners)));
            QVERIFY(r.correlation > 0.9);
            QVERIFY(r.inliers >= 20);
        }
    }

    void followsBackwardsToo()
    {
        // The same movement played in reverse: start on the last frame, walk to the first.
        const int last = 9;
        const Quad startAtLast = truth(last);
        PlanarTracker tracker;
        QVERIFY(tracker.reset(frame(last), startAtLast));
        for (int t = last - 1; t >= 0; --t) {
            const PlanarResult r = tracker.track(frame(t));
            QVERIFY2(r.ok, qPrintable(QStringLiteral("lost at %1").arg(t)));
            QVERIFY2(worstCorner(r.corners, truth(t)) < 0.5, qPrintable(describe(t, r.corners)));
        }
    }

    void toleratesNoiseAndAContrastChange()
    {
        PlanarTracker tracker;
        QVERIFY(tracker.reset(frame(0, 0.02), startQuad()));
        for (int t = 1; t <= 12; ++t) {
            const PlanarResult r = tracker.track(frame(t, 0.02, 0.6));
            QVERIFY2(r.ok, qPrintable(QStringLiteral("lost at %1").arg(t)));
            QVERIFY2(worstCorner(r.corners, truth(t)) < 1.0, qPrintable(describe(t, r.corners)));
        }
    }

    void anOccluderOverPartOfThePlaneIsIgnored()
    {
        // A flat grey box hides about a fifth of the region from frame 4 on: the patches under it
        // fail to match and RANSAC leaves them out; the corners stay on the plane.
        PlanarTracker tracker;
        QVERIFY(tracker.reset(frame(0), startQuad()));
        for (int t = 1; t <= 10; ++t) {
            const QRectF box = t >= 4 ? QRectF(150, 100, 70, 40) : QRectF();
            const PlanarResult r = tracker.track(frame(t, 0.0, 1.0, box));
            QVERIFY2(r.ok, qPrintable(QStringLiteral("lost at %1").arg(t)));
            QVERIFY2(worstCorner(r.corners, truth(t)) < 0.8, qPrintable(describe(t, r.corners)));
            if (t >= 4)
                QVERIFY(r.inliers < r.patches + 1 && r.inliers >= 10);
        }
    }

    void aWrongPictureLosesThePlaneWithoutCrashing()
    {
        PlanarTracker tracker;
        QVERIFY(tracker.reset(frame(0), startQuad()));
        GrayImage noise;
        noise.width = kW;
        noise.height = kH;
        noise.data.resize(size_t(kW) * kH);
        std::mt19937 rng(5);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        for (float& v : noise.data)
            v = u(rng);
        const PlanarResult r = tracker.track(noise);
        QVERIFY(!r.ok);
        QCOMPARE(tracker.corners()[0].x, startQuad()[0].x); // the corners stay where they were
        // And the tracker is still usable afterwards.
        QVERIFY(tracker.track(frame(1)).ok);
    }

    void aFlatRegionCannotBeTracked()
    {
        GrayImage flat;
        flat.width = kW;
        flat.height = kH;
        flat.data.assign(size_t(kW) * kH, 0.4f);
        PlanarTracker tracker;
        QVERIFY(!tracker.reset(flat, startQuad()));
        QVERIFY(!tracker.track(flat).ok); // not ready: no crash, no result
    }

    void badRegionsAreRefused()
    {
        PlanarTracker tracker;
        const GrayImage img = frame(0);
        QVERIFY(!tracker.reset(GrayImage(), startQuad()));
        const Quad bowtie {{{110, 70}, {250, 174}, {244, 70}, {116, 174}}};
        QVERIFY(!tracker.reset(img, bowtie));
        const Quad tiny {{{100, 100}, {104, 100}, {104, 104}, {100, 104}}};
        QVERIFY(!tracker.reset(img, tiny));
        const Quad line {{{10, 10}, {50, 10}, {100, 10}, {150, 10}}};
        QVERIFY(!tracker.reset(img, line));
        // Mostly outside the picture: patches that do not fit are dropped, never read outside.
        const Quad outside {{{-100, -100}, {20, -100}, {20, 20}, {-100, 20}}};
        tracker.reset(img, outside);
        const Quad partly {{{300, 200}, {500, 200}, {500, 400}, {300, 400}}};
        if (tracker.reset(img, partly))
            tracker.track(frame(1));
    }

    void resultsAreRepeatable()
    {
        PlanarTracker a, b;
        QVERIFY(a.reset(frame(0), startQuad()) && b.reset(frame(0), startQuad()));
        const PlanarResult ra = a.track(frame(3)), rb = b.track(frame(3));
        QVERIFY(ra.ok && rb.ok);
        QCOMPARE(ra.motion.m, rb.motion.m);
    }
};

QTEST_APPLESS_MAIN(TestPlanarTracker)
#include "test_planartracker.moc"
