// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the Stabilizer (compositing/tracking/Stabilizer.h, spec section 58): frame-to-frame
// camera motion measured on synthetic shaky footage with a known camera path, per Mode, with a
// moving object in front, and the whole analysis (path, smoothing, cancel, progress, failures).
#include "ShakyFootage.h"
#include "compositing/tracking/Stabilizer.h"
#include "core/Stabilization.h"

#include <QGuiApplication>
#include <QTest>

#include <atomic>
#include <cmath>

using namespace kk;
using namespace kk::test;
using namespace kk::tracking;
namespace st = kk::stabilize;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

const QImage& scene()
{
    static const QImage big = texturedPicture(900, 560, 5);
    return big;
}

GrayImage frameFor(const CameraPose& pose, int object = -1)
{
    return grayFromImage(shakyFrame(scene(), kW, kH, pose, object));
}

/// The pose as the matrix the Stabilizer measures (normalised coordinates, frame 0 -> frame t).
st::Mat3 matrixOf(const CameraPose& p)
{
    const double s = p.zoom;
    const double a = p.degrees * M_PI / 180.0;
    st::Mat3 m;
    m.m = {s * std::cos(a), -s * std::sin(a), p.tx / kW, s * std::sin(a), s * std::cos(a),
        p.ty / kW, 0, 0, 1};
    return m;
}

/// Largest distance (pixels) between where @p a and @p b send the picture's corners and centre.
double worstCornerError(const st::Mat3& a, const st::Mat3& b)
{
    double worst = 0;
    const double pts[5][2] = {{-0.5, -0.5 * kH / kW}, {0.5, -0.5 * kH / kW}, {0.5, 0.5 * kH / kW},
        {-0.5, 0.5 * kH / kW}, {0, 0}};
    for (const auto& p : pts) {
        double ax, ay, bx, by;
        a.apply(p[0], p[1], &ax, &ay);
        b.apply(p[0], p[1], &bx, &by);
        worst = std::max(worst, std::hypot(ax - bx, ay - by) * kW);
    }
    return worst;
}

/// The camera path a StabilizeOutcome stores, as matrices.
st::Mat3 pathMatrix(const st::Data& d, int64_t frame)
{
    const int pc = st::paramCount(d.mode());
    const float* p = &d.path()[size_t(frame) * size_t(pc)];
    st::Mat3 m;
    switch (d.mode()) {
    case st::Mode::Translation:
        return st::Mat3::translation(p[0], p[1]);
    case st::Mode::Similarity: {
        const double s = std::exp(double(p[3]));
        m.m = {s * std::cos(p[2]), -s * std::sin(p[2]), p[0], s * std::sin(p[2]),
            s * std::cos(p[2]), p[1], 0, 0, 1};
        return m;
    }
    case st::Mode::Perspective:
        break;
    }
    for (int i = 0; i < 8; ++i)
        m.m[size_t(i)] = p[i];
    return m;
}

} // namespace

class TestStabilizer : public QObject
{
    Q_OBJECT

private slots:
    // A pure shift is found to a twentieth of a pixel.
    void translationOfOnePair()
    {
        const GrayImage a = frameFor({});
        const GrayImage b = frameFor({3.25, -2.5});
        const FrameMotion m = estimateFrameMotion(a, b, st::Mode::Translation);
        QVERIFY(m.ok);
        QVERIFY(m.inliers >= 10);
        QVERIFY2(std::abs(m.m.m[2] * kW - 3.25) < 0.1, qPrintable(QString::number(m.m.m[2] * kW)));
        QVERIFY2(std::abs(m.m.m[5] * kW + 2.5) < 0.1, qPrintable(QString::number(m.m.m[5] * kW)));
    }

    // Rotation and zoom, found by Similarity (and by Perspective, which contains it).
    void similarityOfOnePair()
    {
        const CameraPose pose {2.0, -1.5, 1.5, 1.03};
        const GrayImage a = frameFor({});
        const GrayImage b = frameFor(pose);
        for (const st::Mode mode : {st::Mode::Similarity, st::Mode::Perspective}) {
            const FrameMotion m = estimateFrameMotion(a, b, mode);
            QVERIFY(m.ok);
            const double err = worstCornerError(m.m, matrixOf(pose));
            QVERIFY2(
                err < 0.6, qPrintable(QStringLiteral("mode %1: %2 px").arg(int(mode)).arg(err)));
        }
        // Translation alone cannot follow it, but gets the shift of the middle right.
        const FrameMotion t = estimateFrameMotion(a, b, st::Mode::Translation);
        QVERIFY(t.ok);
        QVERIFY(std::abs(t.m.m[2] * kW - 2.0) < 1.5);
    }

    // A moving bright object over a tenth of the picture does not steer the measurement.
    void movingObjectIsIgnored()
    {
        const CameraPose pose {-4.0, 3.0, 0.0, 1.0};
        const GrayImage a = frameFor({}, 3);
        const GrayImage b = frameFor(pose, 20); // the object jumped
        const FrameMotion m = estimateFrameMotion(a, b, st::Mode::Translation);
        QVERIFY(m.ok);
        QVERIFY2(std::abs(m.m.m[2] * kW + 4.0) < 0.2, qPrintable(QString::number(m.m.m[2] * kW)));
        QVERIFY2(std::abs(m.m.m[5] * kW - 3.0) < 0.2, qPrintable(QString::number(m.m.m[5] * kW)));
    }

    // Two unrelated pictures cannot be related: not ok, no crash; so is a flat picture.
    void unrelatedFramesAreNotMatched()
    {
        const GrayImage a = frameFor({});
        const GrayImage other = grayFromImage(texturedPicture(kW, kH, 99));
        QVERIFY(!estimateFrameMotion(a, other, st::Mode::Similarity).ok);
        GrayImage flat = a;
        std::fill(flat.data.begin(), flat.data.end(), 0.5f);
        QVERIFY(!estimateFrameMotion(flat, flat, st::Mode::Translation).ok);
        QVERIFY(!estimateFrameMotion(GrayImage {}, a, st::Mode::Translation).ok);
    }

    // The whole analysis: the stored path follows the true camera path (pan and jitter).
    void analysisFollowsTheCameraPath()
    {
        ShakeSettings shake;
        shake.frames = 40;
        shake.panPerFrameX = 0.5;
        shake.panPerFrameY = -0.2;
        shake.jitterPx = 4.0;
        const auto poses = shakyPath(shake);
        std::vector<GrayImage> frames;
        for (size_t t = 0; t < poses.size(); ++t)
            frames.push_back(frameFor(poses[t], int(t)));

        StabilizeRequest request;
        request.mode = st::Mode::Translation;
        request.firstFrame = 10; // source frame numbers do not start at 0
        request.lastFrame = 10 + shake.frames - 1;
        int lastDone = 0, calls = 0;
        const StabilizeOutcome out = stabilizeAnalyse(
            request, [&](int64_t f) { return frames[size_t(f - 10)]; },
            [&](int done, int total) {
                QCOMPARE(total, shake.frames);
                QVERIFY(done > lastDone);
                lastDone = done;
                ++calls;
            });
        QVERIFY2(out.data, qPrintable(out.error));
        QCOMPARE(calls, shake.frames);
        QCOMPARE(out.lost, 0);
        QCOMPARE(out.data->frames(), int64_t(shake.frames));
        QCOMPARE(out.data->firstFrame(), int64_t(10));
        QVERIFY(std::abs(out.data->aspect() - double(kH) / kW) < 1e-9);
        double worst = 0;
        for (int t = 0; t < shake.frames; ++t)
            worst = std::max(
                worst, worstCornerError(pathMatrix(*out.data, t), matrixOf(poses[size_t(t)])));
        QVERIFY2(worst < 0.5, qPrintable(QString::number(worst)));
    }

    // Similarity analysis of rotation and zoom jitter, with an object moving in front.
    void analysisOfRotationAndZoomJitter()
    {
        ShakeSettings shake;
        shake.frames = 30;
        shake.jitterPx = 3.0;
        shake.jitterDegrees = 1.0;
        shake.jitterZoom = 0.02;
        shake.seed = 11;
        const auto poses = shakyPath(shake);
        std::vector<GrayImage> frames;
        for (size_t t = 0; t < poses.size(); ++t)
            frames.push_back(frameFor(poses[t], int(t)));
        StabilizeRequest request;
        request.mode = st::Mode::Similarity;
        request.lastFrame = shake.frames - 1;
        const StabilizeOutcome out
            = stabilizeAnalyse(request, [&](int64_t f) { return frames[size_t(f)]; });
        QVERIFY2(out.data, qPrintable(out.error));
        QCOMPARE(out.lost, 0);
        double worst = 0;
        for (int t = 0; t < shake.frames; ++t)
            worst = std::max(
                worst, worstCornerError(pathMatrix(*out.data, t), matrixOf(poses[size_t(t)])));
        QVERIFY2(worst < 1.0, qPrintable(QString::number(worst)));
    }

    // Cancel: the data is not produced; the flag is honoured between frames.
    void cancelStopsTheAnalysis()
    {
        std::atomic<bool> cancel {false};
        StabilizeRequest request;
        request.lastFrame = 30;
        int asked = 0;
        const StabilizeOutcome out = stabilizeAnalyse(
            request,
            [&](int64_t f) {
                ++asked;
                return frameFor({0.1 * double(f), 0});
            },
            [&](int done, int) {
                if (done == 3)
                    cancel = true;
            },
            &cancel);
        QVERIFY(out.cancelled);
        QVERIFY(!out.data);
        QVERIFY(asked <= 5);
    }

    // A frame that cannot be read ends the run with a message; a pair that cannot be related is
    // counted and treated as no movement; fewer than two frames is refused.
    void failuresAreReported()
    {
        StabilizeRequest request;
        request.lastFrame = 5;
        StabilizeOutcome out = stabilizeAnalyse(
            request, [&](int64_t f) { return f == 3 ? GrayImage {} : frameFor({double(f), 0}); });
        QVERIFY(!out.data);
        QVERIFY(!out.cancelled);
        QVERIFY(out.error.contains(QStringLiteral("frame 3")));

        // Frame 2 is a different scene altogether: the pairs 1-2 and 2-3 are lost.
        const GrayImage other = grayFromImage(texturedPicture(kW, kH, 77));
        out = stabilizeAnalyse(
            request, [&](int64_t f) { return f == 2 ? other : frameFor({double(f), 0}); });
        QVERIFY2(out.data, qPrintable(out.error));
        QCOMPARE(out.pairs, 5);
        QCOMPARE(out.lost, 2);

        request.lastFrame = 0;
        out = stabilizeAnalyse(request, [&](int64_t) { return frameFor({}); });
        QVERIFY(!out.data);
        QVERIFY(!out.error.isEmpty());
    }
};

QTEST_MAIN(TestStabilizer)
#include "test_stabilizer.moc"
