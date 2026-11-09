// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests of the point tracker (src/compositing/tracking): synthetic pictures whose
// content moves by known sub-pixel offsets, with and without noise, tracked in both
// directions, plus the failure cases (pattern lost, flat pattern).
#include "compositing/tracking/PointTracker.h"

#include <QTest>

#include <cmath>
#include <random>

using namespace kk::tracking;

namespace {

constexpr int kW = 320;
constexpr int kH = 240;

struct Wave
{
    double kx, ky, phase, amplitude;
};

/// A fixed texture of random plane waves (wavelengths 5 to 30 px, every direction): smooth,
/// without repeats, and exact at any real offset.
const std::vector<Wave>& waves()
{
    static const std::vector<Wave> list = [] {
        std::mt19937 rng(12345);
        std::uniform_real_distribution<double> wl(5.0, 30.0), dir(0.0, 2 * M_PI), ph(0.0, 2 * M_PI);
        std::vector<Wave> w;
        for (int i = 0; i < 48; ++i) {
            const double k = 2 * M_PI / wl(rng), a = dir(rng);
            w.push_back({k * std::cos(a), k * std::sin(a), ph(rng), 0.06});
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

/// The texture moved by (@p sx, @p sy) pixels, plus Gaussian noise of deviation @p noise.
GrayImage frame(double sx, double sy, double noise = 0.0, unsigned seed = 1)
{
    GrayImage img;
    img.width = kW;
    img.height = kH;
    img.data.resize(size_t(kW) * kH);
    std::mt19937 rng(seed);
    std::normal_distribution<double> n(0.0, noise > 0 ? noise : 1.0);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            img.data[size_t(y) * kW + x]
                = float(texture(x - sx, y - sy) + (noise > 0 ? n(rng) : 0.0));
    return img;
}

double dist(Vec2 a, Vec2 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

} // namespace

class TestPointTracker : public QObject
{
    Q_OBJECT
private slots:
    void subPixelOffsetsNoiseFree()
    {
        const Vec2 start {150, 110};
        const double offsets[][2]
            = {{0.0, 0.0}, {0.5, 0.5}, {3.37, -2.81}, {-4.25, 1.1}, {0.13, 0.9}};
        for (const auto& o : offsets) {
            PointTracker t;
            QVERIFY(t.reset(frame(0, 0), start));
            const TrackResult r = t.track(frame(o[0], o[1]));
            QVERIFY(r.ok);
            QVERIFY2(dist(r.position, {start.x + o[0], start.y + o[1]}) < 0.05,
                qPrintable(QStringLiteral("offset %1,%2 found %3,%4")
                               .arg(o[0])
                               .arg(o[1])
                               .arg(r.position.x - start.x)
                               .arg(r.position.y - start.y)));
            QVERIFY(r.correlation > 0.99);
        }
    }

    void subPixelOffsetWithNoise()
    {
        const Vec2 start {150, 110};
        PointTracker t;
        QVERIFY(t.reset(frame(0, 0, 0.02, 7), start));
        const TrackResult r = t.track(frame(2.43, -1.77, 0.02, 8));
        QVERIFY(r.ok);
        QVERIFY2(dist(r.position, {start.x + 2.43, start.y - 1.77}) < 0.15,
            qPrintable(QStringLiteral("found %1,%2")
                           .arg(r.position.x - start.x)
                           .arg(r.position.y - start.y)));
    }

    void followsMotionOverManyFrames()
    {
        const Vec2 start {120, 100};
        const double vx = 1.7, vy = -0.6;
        PointTracker t;
        QVERIFY(t.reset(frame(0, 0, 0.01, 1), start));
        for (int i = 1; i <= 40; ++i) {
            const TrackResult r = t.track(frame(vx * i, vy * i, 0.01, unsigned(100 + i)));
            QVERIFY2(r.ok, qPrintable(QStringLiteral("lost at frame %1").arg(i)));
            QVERIFY2(dist(r.position, {start.x + vx * i, start.y + vy * i}) < 0.4,
                qPrintable(QStringLiteral("frame %1 off by %2")
                               .arg(i)
                               .arg(dist(r.position, {start.x + vx * i, start.y + vy * i}))));
        }
    }

    void backwardsEqualsForwards()
    {
        // Start at the END position of a move and track to the start: the same offsets.
        const Vec2 end {150 + 3.37, 110 - 2.81};
        PointTracker t;
        QVERIFY(t.reset(frame(3.37, -2.81), end));
        const TrackResult r = t.track(frame(0, 0));
        QVERIFY(r.ok);
        QVERIFY(dist(r.position, {150, 110}) < 0.05);
    }

    void largeJumpWithinSearchBoxIsFound()
    {
        // 30 px: needs the coarse levels (the pattern is 31 px, the search box 101 px).
        PointTracker t;
        QVERIFY(t.reset(frame(0, 0), {150, 110}));
        const TrackResult r = t.track(frame(30.4, -22.6));
        QVERIFY(r.ok);
        QVERIFY(dist(r.position, {180.4, 87.4}) < 0.1);
    }

    void brightnessChangeIsTolerated()
    {
        GrayImage moved = frame(1.5, 0.5);
        for (float& v : moved.data)
            v = v * 0.6f + 0.1f;
        PointTracker t;
        QVERIFY(t.reset(frame(0, 0), {150, 110}));
        const TrackResult r = t.track(moved);
        QVERIFY(r.ok);
        QVERIFY(dist(r.position, {151.5, 110.5}) < 0.1);
    }

    void lostPatternFails()
    {
        // The target jumps out of the search box (the search box is +-50 px): not found, the
        // position stays where it was.
        PointTracker t;
        QVERIFY(t.reset(frame(0, 0), {150, 110}));
        const TrackResult r = t.track(frame(90, 60));
        QVERIFY2(!r.ok,
            qPrintable(QStringLiteral("found %1,%2 corr %3")
                           .arg(r.position.x)
                           .arg(r.position.y)
                           .arg(r.correlation)));
        QCOMPARE(r.position, (Vec2 {150, 110}));
        QCOMPARE(t.position(), (Vec2 {150, 110}));
    }

    void unrelatedPictureFails()
    {
        GrayImage other;
        other.width = kW;
        other.height = kH;
        other.data.resize(size_t(kW) * kH);
        std::mt19937 rng(99);
        std::uniform_real_distribution<float> u(0.f, 1.f);
        for (float& v : other.data)
            v = u(rng);
        PointTracker t;
        QVERIFY(t.reset(frame(0, 0), {150, 110}));
        QVERIFY(!t.track(other).ok);
    }

    void flatPatternCannotBeTracked()
    {
        GrayImage flat;
        flat.width = kW;
        flat.height = kH;
        flat.data.assign(size_t(kW) * kH, 0.4f);
        PointTracker t;
        QVERIFY(!t.reset(flat, {150, 110}));
        QVERIFY(!t.track(flat).ok);
    }

    void pointAtTheBorderDoesNotCrash()
    {
        PointTracker t;
        QVERIFY(t.reset(frame(0, 0), {4, 4}));
        t.track(frame(1, 1)); // may or may not be found; must stay in bounds
        PointTracker u;
        QVERIFY(u.reset(frame(0, 0), {kW - 3, kH - 3}));
        u.track(frame(-1, -1));
    }

    void grayFromImageUsesLuma()
    {
        QImage img(2, 1, QImage::Format_ARGB32_Premultiplied);
        img.setPixel(0, 0, qRgb(255, 255, 255));
        img.setPixel(1, 0, qRgb(0, 255, 0));
        const GrayImage g = grayFromImage(img);
        QCOMPARE(g.width, 2);
        QVERIFY(std::abs(g.at(0, 0) - 1.0f) < 0.001f);
        QVERIFY(std::abs(g.at(1, 0) - 0.7152f) < 0.001f);
        QVERIFY(grayFromImage(QImage()).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPointTracker)
#include "test_pointtracker.moc"
