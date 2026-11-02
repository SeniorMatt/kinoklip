// SPDX-License-Identifier: GPL-3.0-or-later
// Golden-pixel tests of the mask pass (GlMasks) and its CPU fallback: every mode, invert,
// opacity, feather monotonicity, expansion, point paths, animated masks at exact frames, masks
// following the clip transform, and a 1080p timing comparison (spec section 27).
#include "RenderTestUtil.h"
#include "core/Mask.h"
#include "render/Compositor.h"

#include <QElapsedTimer>
#include <QTest>

#include <memory>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;
const QColor kRed(255, 0, 0);

/// A timeline with one red Solid clip (covers the frame: content = 320 × 180, centre (160, 90)).
TimelineData solidTimeline(int w = kW, int h = kH)
{
    TimelineData tl = emptyTimeline(w, h);
    addClip(tl, TrackType::Video, 0, solidClip(tl, kRed, 0, 50));
    return tl;
}

ClipData& theClip(TimelineData& tl)
{
    return tl.videoTracks[0].clips[0];
}

MaskData rectMask(
    TimelineData& tl, double cx, double cy, double w, double h, MaskMode mode = MaskMode::Add)
{
    MaskData m = makeMask(MaskShape::Rectangle, tl.allocateId(), QSizeF(w, h));
    m.params.setBase(maskkeys::PosX, cx);
    m.params.setBase(maskkeys::PosY, cy);
    m.mode = mode;
    return m;
}

MaskData ellipseMask(TimelineData& tl, double w, double h)
{
    return makeMask(MaskShape::Ellipse, tl.allocateId(), QSizeF(w, h));
}

/// A closed polygon / bezier mask through @p points (mask-local = content coordinates).
MaskData pathMask(TimelineData& tl, MaskShape shape, const MaskPath& points)
{
    MaskData m = makeMask(shape, tl.allocateId(), {});
    m.closed = true;
    m.path.setShape(0, points);
    return m;
}

int alphaAt(const QImage& image, int x, int y)
{
    return px(image, x, y).a;
}

} // namespace

class TestMaskRender : public QObject
{
    Q_OBJECT
    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

    QImage render(const TimelineData& tl, int64_t frame = 0)
    {
        return m_gpu->renderImage(snap(tl), frame);
    }

private slots:
    void initTestCase()
    {
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
        qInfo("GPU compositor: %s", qPrintable(m_gpu->backendDescription()));
    }
    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    void ellipseAdd()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = solidTimeline();
        theClip(tl).masks.append(ellipseMask(tl, 201, 100)); // 59.5..260.5 × 40..140
        const QImage img = render(tl);
        QVERIFY2(near(px(img, 160, 90), 255, 0, 0, 255, 1), describe(px(img, 160, 90)));
        QVERIFY(near(px(img, 255, 90), 255, 0, 0, 255, 1)); // inside near the right end
        QCOMPARE(alphaAt(img, 266, 90), 0); // outside
        QCOMPARE(alphaAt(img, 70, 45), 0); // inside the bounding box, outside the ellipse
        QCOMPARE(alphaAt(img, 2, 2), 0);
        // The hard edge is antialiased over about one pixel: pixel 260 has its centre on it.
        const int edge = alphaAt(img, 260, 90);
        QVERIFY2(edge > 40 && edge < 215, QByteArray::number(edge));
    }

    void modes()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        // Two overlapping rectangles: A = x 40..160, B = x 120..240 (content x −120..0 and
        // −40..80), both y 50..130.
        auto withModes = [&](MaskMode first, MaskMode second) {
            TimelineData tl = solidTimeline();
            theClip(tl).masks.append(rectMask(tl, -60, 0, 120, 80, first));
            theClip(tl).masks.append(rectMask(tl, 20, 0, 120, 80, second));
            return render(tl);
        };
        struct Expect
        {
            MaskMode first, second;
            int onlyA, both, onlyB, outside;
        };
        const Expect cases[] = {
            {MaskMode::Add, MaskMode::Add, 255, 255, 255, 0},
            {MaskMode::Add, MaskMode::Subtract, 255, 0, 0, 0},
            {MaskMode::Add, MaskMode::Intersect, 0, 255, 0, 0},
            {MaskMode::Add, MaskMode::Difference, 255, 0, 255, 0},
            // A first subtract mask starts from a full matte: it cuts a hole.
            {MaskMode::Subtract, MaskMode::Subtract, 0, 0, 0, 255},
        };
        for (const Expect& e : cases) {
            const QImage img = withModes(e.first, e.second);
            const QByteArray what = maskModeToString(e.first).toLatin1() + '/'
                + maskModeToString(e.second).toLatin1();
            QVERIFY2(std::abs(alphaAt(img, 80, 90) - e.onlyA) <= 1, what + " only A");
            QVERIFY2(std::abs(alphaAt(img, 140, 90) - e.both) <= 1, what + " both");
            QVERIFY2(std::abs(alphaAt(img, 200, 90) - e.onlyB) <= 1, what + " only B");
            QVERIFY2(std::abs(alphaAt(img, 300, 20) - e.outside) <= 1, what + " outside");
        }
    }

    void invertAndOpacity()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = solidTimeline();
        MaskData m = ellipseMask(tl, 100, 100);
        m.invert = true;
        theClip(tl).masks.append(m);
        QImage img = render(tl);
        QCOMPARE(alphaAt(img, 160, 90), 0);
        QVERIFY(near(px(img, 10, 10), 255, 0, 0, 255, 1));
        theClip(tl).masks[0].invert = false;
        theClip(tl).masks[0].params.setBase(maskkeys::Opacity, 50);
        img = render(tl);
        QVERIFY2(near(px(img, 160, 90), 128, 0, 0, 128, 2), describe(px(img, 160, 90)));
        QCOMPARE(alphaAt(img, 10, 10), 0);
    }

    void featherIsMonotonic_data()
    {
        QTest::addColumn<int>("shape");
        QTest::newRow("ellipse") << int(MaskShape::Ellipse);
        QTest::newRow("rectangle") << int(MaskShape::Rectangle);
        QTest::newRow("polygon") << int(MaskShape::Polygon);
    }
    void featherIsMonotonic()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        QFETCH(int, shape);
        TimelineData tl = solidTimeline();
        MaskData m;
        if (shape == int(MaskShape::Polygon)) {
            // The same 160 × 100 rectangle as a point path.
            m = pathMask(tl, MaskShape::Polygon, {{-80, -50}, {80, -50}, {80, 50}, {-80, 50}});
        } else {
            m = makeMask(MaskShape(shape), tl.allocateId(), QSizeF(160, 100));
        }
        m.params.setBase(maskkeys::FeatherIn, 10);
        m.params.setBase(maskkeys::FeatherOut, 20);
        theClip(tl).masks.append(m);
        const QImage img = render(tl);
        // Along the horizontal centre line from the centre outwards (edge at x = 240): full
        // inside the ramp, then never increasing, about half at the middle of the ramp
        // (edge + 5 px), zero beyond it.
        int previous = 256;
        for (int x = 160; x < 300; ++x) {
            const int a = alphaAt(img, x, 90);
            QVERIFY2(a <= previous + 1, QByteArray::number(x) + ": " + QByteArray::number(a));
            previous = a;
        }
        QVERIFY(alphaAt(img, 200, 90) >= 254);
        const int mid = alphaAt(img, 245, 90);
        QVERIFY2(mid > 90 && mid < 165, QByteArray::number(mid));
        QVERIFY2(alphaAt(img, 236, 90) > alphaAt(img, 250, 90) + 40, "no visible ramp");
        QVERIFY(alphaAt(img, 268, 90) <= 3);
        // A wider feather gives a softer (more spread) edge.
        theClip(tl).masks[0].params.setBase(maskkeys::FeatherOut, 40);
        theClip(tl).masks[0].params.setBase(maskkeys::FeatherIn, 0);
        const QImage softer = render(tl);
        QVERIFY(alphaAt(softer, 262, 90) > alphaAt(img, 262, 90));
    }

    void expandContract()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        for (const bool path : {false, true}) {
            TimelineData tl = solidTimeline();
            MaskData m = path
                ? pathMask(tl, MaskShape::Polygon, {{-50, -50}, {50, -50}, {50, 50}, {-50, 50}})
                : rectMask(tl, 0, 0, 100, 100);
            m.params.setBase(maskkeys::Expand, 10); // edge 210 → 220
            theClip(tl).masks.append(m);
            QImage img = render(tl);
            QVERIFY2(alphaAt(img, 216, 90) >= 250, path ? "path" : "rect");
            QVERIFY(alphaAt(img, 224, 90) <= 5);
            theClip(tl).masks[0].params.setBase(maskkeys::Expand, -20); // edge 190
            img = render(tl);
            QVERIFY(alphaAt(img, 186, 90) >= 250);
            QVERIFY(alphaAt(img, 194, 90) <= 5);
        }
    }

    void bezierPath()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = solidTimeline();
        // A lens shape: two points with tangents bulging up and down.
        theClip(tl).masks.append(pathMask(tl, MaskShape::Bezier,
            {{-100, 0, 0, 60, 0, -60, true}, {100, 0, 0, -60, 0, 60, true}}));
        const QImage img = render(tl);
        QVERIFY(near(px(img, 160, 90), 255, 0, 0, 255, 1));
        QVERIFY(alphaAt(img, 160, 50) >= 250); // bulge reaches y = ±45 at the centre
        QVERIFY(alphaAt(img, 160, 30) <= 5);
        QVERIFY(alphaAt(img, 70, 50) <= 5); // near the tips the lens is thin
        // Even-odd fill of a self-crossing bow tie keeps both lobes.
        TimelineData bow = solidTimeline();
        theClip(bow).masks.append(
            pathMask(bow, MaskShape::Polygon, {{-100, -50}, {100, 50}, {100, -50}, {-100, 50}}));
        const QImage b = render(bow);
        QVERIFY(alphaAt(b, 80, 90) >= 250);
        QVERIFY(alphaAt(b, 240, 90) >= 250);
        QVERIFY(alphaAt(b, 160, 50) <= 5);
    }

    void animatedMaskAtExactFrames()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = solidTimeline();
        MaskData m = rectMask(tl, -100, 0, 40, 40); // x 40..80
        m.params.animated(maskkeys::PosX, 0).setKey(0, -100.0);
        m.params.animated(maskkeys::PosX, 0).setKey(10, 100.0); // x 240..280 at frame 10
        theClip(tl).masks.append(m);
        // Shape keys: a triangle moving down by 40 px over the same frames.
        MaskData tri = pathMask(tl, MaskShape::Polygon, {{-20, -70}, {20, -70}, {0, -50}});
        tri.path.setKey(0);
        tri.path.setKey(10, MaskPath {{-20, -30}, {20, -30}, {0, -10}});
        theClip(tl).masks.append(tri);
        const QImage f0 = render(tl, 0);
        const QImage f5 = render(tl, 5);
        const QImage f10 = render(tl, 10);
        QVERIFY(alphaAt(f0, 60, 90) >= 250);
        QVERIFY(alphaAt(f0, 160, 90) <= 5);
        QVERIFY(alphaAt(f5, 160, 90) >= 250); // centre at 0 halfway
        QVERIFY(alphaAt(f5, 60, 90) <= 5);
        QVERIFY(alphaAt(f10, 260, 90) >= 250);
        QVERIFY(alphaAt(f0, 160, 25) >= 250); // triangle at y 20..40 → 40..60 → 60..80
        QVERIFY(alphaAt(f5, 160, 25) <= 5);
        QVERIFY(alphaAt(f5, 160, 45) >= 250);
        QVERIFY(alphaAt(f10, 160, 65) >= 250);
    }

    void masksFollowTheClipTransform()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = solidTimeline();
        theClip(tl).masks.append(rectMask(tl, 0, 0, 40, 40));
        theClip(tl).transform.setBase(keys::PosX, 100); // the masked picture moves along
        theClip(tl).transform.setBase(keys::Opacity, 50); // opacity after the mask
        const QImage img = render(tl);
        QVERIFY2(near(px(img, 260, 90), 128, 0, 0, 128, 2), describe(px(img, 260, 90)));
        QCOMPARE(alphaAt(img, 160, 90), 0);
        // Also with an effect: the mask is applied after the effect chain.
        EffectInstance fx;
        fx.id = tl.allocateId();
        fx.type = QStringLiteral("brightness_contrast");
        theClip(tl).effects.append(fx);
        const QImage withFx = render(tl);
        QVERIFY(alphaAt(withFx, 260, 90) >= 120);
        QCOMPARE(alphaAt(withFx, 160, 90), 0);
    }

    void manyMasksChain()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        // 5 analytic + 10 path masks: more than one combine chunk and more than four coverage
        // textures per chunk; each adds a 10 px square along the top row.
        TimelineData tl = solidTimeline();
        for (int i = 0; i < 15; ++i) {
            const double cx = -150 + i * 20;
            if (i % 3 != 0) {
                theClip(tl).masks.append(pathMask(tl, MaskShape::Polygon,
                    {{cx - 5, -75}, {cx + 5, -75}, {cx + 5, -65}, {cx - 5, -65}}));
            } else {
                theClip(tl).masks.append(rectMask(tl, cx, -70, 10, 10));
            }
        }
        const QImage img = render(tl);
        for (int i = 0; i < 15; ++i) {
            const int x = 160 - 150 + i * 20;
            QVERIFY2(alphaAt(img, x, 20) >= 250, QByteArray::number(i));
            QVERIFY(alphaAt(img, x + 10, 20) <= 5);
        }
    }

    void cpuFallback()
    {
        TimelineData tl = solidTimeline();
        theClip(tl).masks.append(ellipseMask(tl, 200, 100));
        MaskData hole = rectMask(tl, 0, 0, 20, 20, MaskMode::Subtract);
        theClip(tl).masks.append(hole);
        const QImage img = m_cpu->renderImage(snap(tl), 0);
        QVERIFY(!img.isNull());
        QCOMPARE(alphaAt(img, 160, 90), 0); // the hole
        QVERIFY(near(px(img, 200, 90), 255, 0, 0, 255, 1));
        QCOMPARE(alphaAt(img, 70, 45), 0);
        QCOMPARE(alphaAt(img, 300, 90), 0);
    }

    void timing1080p()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        // 1080p clip, unmasked vs four feathered masks (2 analytic + 2 bezier paths). On a real
        // GPU the mask pass costs well under 2 ms; under llvmpipe the numbers below are what the
        // CPU rasteriser needs and are reported, not asserted.
        TimelineData plain = solidTimeline(1920, 1080);
        TimelineData masked = plain;
        ClipData& c = theClip(masked);
        MaskData e = ellipseMask(masked, 900, 600);
        e.params.setBase(maskkeys::FeatherOut, 40);
        c.masks.append(e);
        MaskData r = rectMask(masked, 300, 200, 500, 300, MaskMode::Subtract);
        r.params.setBase(maskkeys::FeatherIn, 20);
        r.params.setBase(maskkeys::Rotation, 15);
        c.masks.append(r);
        MaskData b1 = pathMask(masked, MaskShape::Bezier,
            {{-600, 0, 0, 300, 0, -300, true}, {-200, 0, 0, -300, 0, 300, true}});
        b1.params.setBase(maskkeys::FeatherOut, 30);
        c.masks.append(b1);
        MaskData b2 = pathMask(masked, MaskShape::Bezier,
            {{200, -300, -100, 0, 100, 0, true}, {700, 0, 0, -150, 0, 150, true},
                {200, 300, 100, 0, -100, 0, true}});
        b2.params.setBase(maskkeys::FeatherIn, 15);
        b2.params.setBase(maskkeys::FeatherOut, 15);
        b2.mode = MaskMode::Difference;
        c.masks.append(b2);
        auto measure = [&](const TimelineData& tl) {
            const auto s = snap(tl);
            (void)m_gpu->render(s, 0, {}, Quality::Full, OutputTexture); // warm-up
            constexpr int kFrames = 12;
            double sum = 0;
            for (int i = 0; i < kFrames; ++i) {
                const RenderedFramePtr f = m_gpu->render(s, 0, {}, Quality::Full, OutputTexture);
                sum += f->renderMs;
            }
            return sum / kFrames;
        };
        // Four analytic feathered masks (one combine pass).
        TimelineData analytic = plain;
        for (int i = 0; i < 4; ++i) {
            MaskData a = i % 2 ? rectMask(analytic, -400 + i * 250, 0, 300, 400, MaskMode::Add)
                               : ellipseMask(analytic, 500, 300);
            a.params.setBase(maskkeys::FeatherOut, 25);
            a.params.setBase(maskkeys::FeatherIn, 10);
            theClip(analytic).masks.append(a);
        }
        const double plainMs = measure(plain);
        const double maskedMs = measure(masked);
        const double analyticMs = measure(analytic);
        qInfo("1080p solid clip on %s: %.2f ms unmasked; 4 feathered masks (2 analytic + 2 "
              "bezier): %.2f ms (+%.2f); 4 analytic feathered masks: %.2f ms (+%.2f)",
            qPrintable(m_gpu->backendDescription()), plainMs, maskedMs, maskedMs - plainMs,
            analyticMs, analyticMs - plainMs);
        QVERIFY(maskedMs > 0);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestMaskRender>(argc, argv);
}

#include "test_maskrender.moc"
