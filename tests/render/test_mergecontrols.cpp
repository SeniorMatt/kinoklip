// SPDX-License-Identifier: GPL-3.0-or-later
// Pixel tests of the Merge and Transform controls of the Compositing page (spec section 88):
// Edges, Pivot, Invert Transform, Filter Method, Reference Size, Alpha Gain, Burn In,
// Subtractive / Additive, the Normal operators and the added apply modes. Every test looks at
// the GPU and the CPU renderer: they must show the same picture.
#include "RenderTestUtil.h"
#include "core/CompOps.h"
#include "core/NodeRegistry.h"
#include "render/CompMath.h"
#include "render/Compositor.h"

#include <QTest>

#include <memory>

using namespace kk;
using namespace kk::test;
namespace co = kk::compops;
namespace compmath = kk::render::compmath;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

TimelineData compTimeline(const Composition& comp, QColor color = QColor(255, 0, 0))
{
    TimelineData tl = emptyTimeline(kW, kH);
    ClipData c = solidClip(tl, color, 0, 50);
    c.composition = comp;
    addClip(tl, TrackType::Video, 0, c);
    return tl;
}

quint64 mediaIn(const Composition& c)
{
    for (const CompNode& n : c.nodes) {
        if (n.type == nodetypes::MediaIn)
            return n.id;
    }
    return 0;
}

/// A four corner gradient with four very different colours (the picture to move around).
void paintGradient(CompNode* n)
{
    n->params.setStatic(nodekeys::Mode, backgroundmodes::FourCorner);
    n->params.setStatic(nodekeys::Color, QColor(255, 0, 0));
    n->params.setStatic(nodekeys::Color2, QColor(0, 255, 0));
    n->params.setStatic(nodekeys::Color3, QColor(0, 0, 255));
    n->params.setStatic(nodekeys::Color4, QColor(255, 255, 0));
}

/// MediaIn (red) -> Merge with a gradient Background as the foreground.
Composition gradientMerge(quint64* merge)
{
    Composition c = co::defaultComposition();
    const co::AddResult r = co::addNodeAfter(c, nodetypes::Background, mediaIn(c));
    paintGradient(c.node(r.node));
    *merge = r.merge;
    return c;
}

/// MediaIn (red) -> Merge with a Background of @p color as the foreground.
Composition colorMerge(QColor color, quint64* merge)
{
    Composition c = co::defaultComposition();
    const co::AddResult r = co::addNodeAfter(c, nodetypes::Background, mediaIn(c));
    c.node(r.node)->params.setStatic(nodekeys::Color, color);
    *merge = r.merge;
    return c;
}

/// Gradient Background -> Transform -> MediaOut.
Composition gradientTransform(quint64* transform)
{
    Composition c = co::defaultComposition();
    const quint64 bg = co::addNode(c, nodetypes::Background, {});
    paintGradient(c.node(bg));
    *transform = co::addNode(c, nodetypes::Transform, {});
    co::connect(c, bg, 0, *transform, nodeports::Input);
    co::connect(c, *transform, 0, c.mediaOutId, nodeports::Input);
    return c;
}

/// Fast noise -> Transform -> MediaOut (lots of detail for the filter tests).
Composition noiseTransform(quint64* transform)
{
    Composition c = co::defaultComposition();
    const quint64 noise = co::addNode(c, nodetypes::FastNoise, {});
    c.node(noise)->params.setBase(nodekeys::Scale, 60.0);
    c.node(noise)->params.setBase(nodekeys::Detail, 3.0);
    *transform = co::addNode(c, nodetypes::Transform, {});
    co::connect(c, noise, 0, *transform, nodeports::Input);
    co::connect(c, *transform, 0, c.mediaOutId, nodeports::Input);
    return c;
}

double meanDifference(const QImage& a, const QImage& b)
{
    double sum = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const Px p = px(a, x, y), q = px(b, x, y);
            sum += std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b);
        }
    return sum / (3.0 * a.width() * a.height());
}

QString say(const QString& what, const char* who, const Px& p)
{
    return what + QLatin1Char(' ') + QString::fromLatin1(who) + QLatin1Char(' ')
        + QString::fromLatin1(describe(p));
}

} // namespace

class TestMergeControls : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

    QImage gpu(const TimelineData& tl) { return m_gpu->renderImage(snap(tl), 0); }
    QImage cpu(const TimelineData& tl) { return m_cpu->renderImage(snap(tl), 0); }

    /// Both renderers on @p comp; @p check runs on each picture.
    template<typename Fn>
    void both(const Composition& comp, Fn&& check, QColor base = QColor(255, 0, 0))
    {
        const TimelineData tl = compTimeline(comp, base);
        check(gpu(tl), "gpu");
        check(cpu(tl), "cpu");
    }

private slots:
    void initTestCase()
    {
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
    }
    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    void mergeEdgesFillTheUncoveredStrip()
    {
        quint64 merge = 0;
        Composition c = gradientMerge(&merge);
        const TimelineData plain = compTimeline(c);
        const QImage refG = gpu(plain), refC = cpu(plain);
        c.node(merge)->params.setBase(nodekeys::CenterX, 0.75); // 80 px to the right
        // Output x 10 reads the picture at x -70: Canvas nothing (the red clip), Wrap x 250,
        // Duplicate x 0, Mirror x 69.
        const struct
        {
            QString mode;
            int source;
        } cases[] = {{edgemodes::Canvas, -1}, {edgemodes::Wrap, 250}, {edgemodes::Duplicate, 0},
            {edgemodes::Mirror, 69}};
        for (const auto& t : cases) {
            c.node(merge)->params.setStatic(nodekeys::Edges, t.mode);
            int i = 0;
            both(c, [&](const QImage& img, const char* who) {
                const QImage& ref = i++ == 0 ? refG : refC;
                const Px p = px(img, 10, 90);
                const Px want = t.source < 0 ? Px {255, 0, 0, 255} : px(ref, t.source, 90);
                QVERIFY2(near(p, want.r, want.g, want.b, 255, 4), qPrintable(say(t.mode, who, p)));
                // The moved picture itself is untouched inside the frame.
                const Px in = px(ref, 120, 90);
                QVERIFY2(near(px(img, 200, 90), in.r, in.g, in.b, 255, 4), who);
            });
        }
    }

    void transformEdgesFillTheUncoveredStrip()
    {
        quint64 xf = 0;
        Composition c = gradientTransform(&xf);
        const TimelineData plain = compTimeline(c);
        const QImage refG = gpu(plain), refC = cpu(plain);
        c.node(xf)->params.setBase(nodekeys::CenterX, 0.75);
        const struct
        {
            QString mode;
            int source;
        } cases[] = {{edgemodes::Canvas, -1}, {edgemodes::Wrap, 250}, {edgemodes::Duplicate, 0},
            {edgemodes::Mirror, 69}};
        for (const auto& t : cases) {
            c.node(xf)->params.setStatic(nodekeys::Edges, t.mode);
            int i = 0;
            both(c, [&](const QImage& img, const char* who) {
                const QImage& ref = i++ == 0 ? refG : refC;
                const Px p = px(img, 10, 90);
                if (t.source < 0) {
                    QCOMPARE(p.a, 0);
                    return;
                }
                const Px want = px(ref, t.source, 90);
                QVERIFY2(near(p, want.r, want.g, want.b, 255, 4), qPrintable(say(t.mode, who, p)));
            });
        }
    }

    void invertTransformUndoesTheMove()
    {
        ParamSet p;
        const QSizeF frame(kW, kH);
        p.setBase(nodekeys::CenterX, 0.7);
        p.setBase(nodekeys::Size, 0.6);
        p.setBase(nodekeys::Angle, 20.0);
        const QTransform m = compmath::mergeForegroundMatrix(p, 0, frame);
        const QTransform t = compmath::transformMatrix(p, 0, frame);
        p.setStatic(nodekeys::InvertTransform, true);
        const QPointF q(100, 70);
        QVERIFY((compmath::mergeForegroundMatrix(p, 0, frame).map(m.map(q)) - q).manhattanLength()
            < 1e-6);
        QVERIFY((compmath::transformMatrix(p, 0, frame).map(t.map(q)) - q).manhattanLength()
            < 1e-6);

        // On screen: Size 2 moved right, inverted: the picture shrinks to x 40..200.
        quint64 merge = 0;
        Composition c = colorMerge(QColor(Qt::white), &merge);
        CompNode* node = c.node(merge);
        node->params.setBase(nodekeys::Size, 2.0);
        node->params.setBase(nodekeys::CenterX, 0.75);
        node->params.setStatic(nodekeys::InvertTransform, true);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 100, 90), 255, 255, 255, 255, 2), who);
            QVERIFY2(near(px(img, 240, 90), 255, 0, 0, 255, 2), who);
            QVERIFY2(near(px(img, 20, 90), 255, 0, 0, 255, 2), who);
        });
    }

    void pivotMovesTheCentreOfScaling()
    {
        quint64 merge = 0;
        Composition c = colorMerge(QColor(Qt::white), &merge);
        CompNode* node = c.node(merge);
        node->params.setBase(nodekeys::Size, 0.5);
        node->params.setBase(nodekeys::PivotX, 0.0); // the picture's left edge stays put
        both(c, [](const QImage& img, const char* who) {
            // The pivot (left middle of the picture) sits at Center (the frame middle): the
            // picture covers x 160..240, y 45..135.
            QVERIFY2(near(px(img, 200, 90), 255, 255, 255, 255, 2), who);
            QVERIFY2(near(px(img, 120, 90), 255, 0, 0, 255, 2), who);
            QVERIFY2(near(px(img, 200, 20), 255, 0, 0, 255, 2), who);
        });
    }

    void referenceSizeScalesThePicture()
    {
        quint64 merge = 0;
        Composition c = colorMerge(QColor(Qt::white), &merge);
        CompNode* node = c.node(merge);
        // Using the frame format changes nothing, whatever the numbers say.
        node->params.setBase(nodekeys::RefWidth, 160.0);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 5, 5), 255, 255, 255, 255, 2), who);
        });
        node->params.setStatic(nodekeys::UseFrameFormat, false);
        node->params.setBase(nodekeys::RefHeight, 90.0);
        both(c, [](const QImage& img, const char* who) {
            // 160 x 90 about the middle: x 80..240, y 45..135.
            QVERIFY2(near(px(img, 160, 90), 255, 255, 255, 255, 2), who);
            QVERIFY2(near(px(img, 40, 90), 255, 0, 0, 255, 2), who);
            QVERIFY2(near(px(img, 160, 20), 255, 0, 0, 255, 2), who);
        });
    }

    void filterMethodsAgreeOnBothRenderers()
    {
        quint64 xf = 0;
        Composition c = noiseTransform(&xf);
        CompNode* n = c.node(xf);
        n->params.setBase(nodekeys::Angle, 12.0);
        n->params.setBase(nodekeys::Size, 0.8);
        n->params.setBase(nodekeys::CenterX, 0.53);
        n->params.setStatic(nodekeys::Edges, edgemodes::Mirror);
        QImage first;
        for (const QString& filter : filtermethods::ids()) {
            n->params.setStatic(nodekeys::FilterMethod, filter);
            const TimelineData tl = compTimeline(c);
            const QImage g = gpu(tl), s = cpu(tl);
            const double d = meanDifference(g, s);
            QVERIFY2(d < 2.5, qPrintable(filter + QStringLiteral(" differs by %1").arg(d)));
            if (first.isNull())
                first = g;
            else
                QVERIFY2(meanDifference(first, g) > 0.3, qPrintable(filter));
        }
    }

    void nearestKeepsTheOriginalPixels()
    {
        quint64 xf = 0;
        Composition c = noiseTransform(&xf);
        c.node(xf)->params.setStatic(nodekeys::FilterMethod, filtermethods::Nearest);
        c.node(xf)->params.setBase(nodekeys::CenterX, 0.5 + 7.0 / kW); // 7 px to the right
        const QImage moved = cpu(compTimeline(c));
        c.node(xf)->params.setBase(nodekeys::CenterX, 0.5);
        const QImage ref = cpu(compTimeline(c));
        for (int x = 20; x < 300; x += 17)
            QCOMPARE(px(moved, x + 7, 60).g, px(ref, x, 60).g);
        QVERIFY(meanDifference(moved, ref) > 1.0);
    }

    void alphaGainBurnInAndSubtractiveAdditive()
    {
        // Half transparent green over opaque red.
        quint64 merge = 0;
        Composition c = colorMerge(QColor(0, 255, 0, 128), &merge);
        ParamSet& p = c.node(merge)->params;
        const float s[4] = {0, 128 / 255.0f, 0, 128 / 255.0f};
        const float b[4] = {1, 0, 0, 1};
        const auto check = [&](const char* what) {
            float o[4];
            compmath::mergePixel(compmath::mergeParams(p, 0), b, s, o);
            const std::array<int, 4> e {int(o[0] * 255 + 0.5), int(o[1] * 255 + 0.5),
                int(o[2] * 255 + 0.5), int(o[3] * 255 + 0.5)};
            both(c, [&](const QImage& img, const char* who) {
                const Px g = px(img, 100, 100);
                QVERIFY2(near(g, e[0], e[1], e[2], e[3], 3), qPrintable(say(what, who, g)));
            });
            return e;
        };
        const auto normal = check("normal");
        QVERIFY(near({normal[0], normal[1], normal[2], 255}, 127, 128, 0, 255, 3));
        // Additive: the red stays whole under the green.
        p.setBase(nodekeys::SubAdd, 1.0);
        const auto additive = check("additive");
        QCOMPARE(additive[0], 255);
        // Burn In brings the darkening back.
        p.setBase(nodekeys::BurnIn, 1.0);
        QCOMPARE(check("burn in"), normal);
        // Alpha Gain 0 at Subtractive: the colour is added whole, the red is not darkened.
        p.setBase(nodekeys::SubAdd, 0.0);
        p.setBase(nodekeys::BurnIn, 0.0);
        p.setBase(nodekeys::AlphaGain, 0.0);
        const auto gain = check("alpha gain");
        QCOMPARE(gain[0], 255);
        QVERIFY(gain[1] > 100);
    }

    void normalOperators()
    {
        // Foreground alpha 50 %, background alpha 25 %: the operators differ everywhere.
        Composition c = co::defaultComposition();
        const quint64 bgNode = co::addNode(c, nodetypes::Background, {});
        const quint64 fgNode = co::addNode(c, nodetypes::Background, {});
        const quint64 merge = co::addNode(c, nodetypes::Merge, {});
        c.node(bgNode)->params.setStatic(nodekeys::Color, QColor(255, 0, 0, 64));
        c.node(fgNode)->params.setStatic(nodekeys::Color, QColor(0, 0, 255, 128));
        co::connect(c, bgNode, 0, merge, nodeports::Background);
        co::connect(c, fgNode, 0, merge, nodeports::Foreground);
        co::connect(c, merge, 0, c.mediaOutId, nodeports::Input);
        const float b[4] = {64 / 255.0f, 0, 0, 64 / 255.0f};
        const float s[4] = {0, 0, 128 / 255.0f, 128 / 255.0f};
        // Porter and Duff alphas: over .5 + .25 · .5, in .125, held out .375, atop .25
        // (.125 + .25 · .5), xor .5.
        const float wantAlpha[5] = {0.625f, 0.125f, 0.375f, 0.25f, 0.5f};
        for (int i = 0; i < 5; ++i) {
            const QString op = mergeops::ids()[i];
            c.node(merge)->params.setStatic(nodekeys::Operation, op);
            compmath::MergeParams mp;
            mp.operation = i;
            float o[4];
            compmath::mergePixel(mp, b, s, o);
            QVERIFY2(std::abs(o[3] - wantAlpha[i]) < 0.02f, qPrintable(op));
            both(c, [&](const QImage& img, const char* who) {
                const Px g = px(img, 100, 100);
                const int a = int(o[3] * 255 + 0.5);
                // The picture comes back premultiplied.
                const int rr = int(o[0] * 255 + 0.5);
                const int bb = int(o[2] * 255 + 0.5);
                QVERIFY2(std::abs(g.a - a) <= 3 && std::abs(g.r - rr) <= 12
                        && std::abs(g.b - bb) <= 12,
                    qPrintable(say(op + QStringLiteral(" alpha %1").arg(a), who, g)));
            });
        }
    }

    void addedApplyModesMatchTheReference()
    {
        QVERIFY(blendmodes::ids().size() >= 33); // more than 30 modes
        for (int i = 19; i < blendmodes::ids().size(); ++i) {
            const QString mode = blendmodes::ids()[i];
            quint64 merge = 0;
            Composition c = colorMerge(QColor(64, 200, 128, 255), &merge);
            c.node(merge)->params.setStatic(nodekeys::BlendMode, mode);
            const float cb[3] = {200 / 255.0f, 100 / 255.0f, 40 / 255.0f};
            const float cs[3] = {64 / 255.0f, 200 / 255.0f, 128 / 255.0f};
            float B[3];
            compmath::blendColor(i, cb, cs, B);
            both(
                c,
                [&](const QImage& img, const char* who) {
                    const Px g = px(img, 100, 100);
                    QVERIFY2(near(g, int(std::clamp(B[0], 0.f, 1.f) * 255 + 0.5),
                                 int(std::clamp(B[1], 0.f, 1.f) * 255 + 0.5),
                                 int(std::clamp(B[2], 0.f, 1.f) * 255 + 0.5), 255, 3),
                        qPrintable(say(mode, who, g)));
                },
                QColor(200, 100, 40));
        }
    }

    void nonSeparableModesFollowTheirDefinition()
    {
        // Luminosity keeps the source's luma on the backdrop's colour, Color the reverse.
        const float cb[3] = {0.8f, 0.2f, 0.1f};
        const float cs[3] = {0.1f, 0.3f, 0.9f};
        float lumOut[3], colOut[3];
        compmath::blendColor(blendmodes::index(blendmodes::Luminosity), cb, cs, lumOut);
        compmath::blendColor(blendmodes::index(blendmodes::Color), cb, cs, colOut);
        const auto luma = [](const float* c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; };
        QVERIFY(std::abs(luma(lumOut) - luma(cs)) < 1e-4f);
        QVERIFY(std::abs(luma(colOut) - luma(cb)) < 1e-4f);
        // Darker / Lighter Color pick a whole colour.
        float dark[3], light[3];
        compmath::blendColor(blendmodes::index(blendmodes::DarkerColor), cb, cs, dark);
        compmath::blendColor(blendmodes::index(blendmodes::LighterColor), cb, cs, light);
        QCOMPARE(dark[0], cs[0]);
        QCOMPARE(light[0], cb[0]);
    }

    void newControlsAreKeyframeable()
    {
        quint64 merge = 0;
        Composition c = colorMerge(QColor(0, 255, 0, 128), &merge);
        ParamSet& p = c.node(merge)->params;
        for (const QString& key : {nodekeys::AlphaGain, nodekeys::BurnIn, nodekeys::SubAdd,
                 nodekeys::PivotX, nodekeys::PivotY, nodekeys::RefWidth}) {
            const ParamDescriptor* d = NodeRegistry::instance().find(nodetypes::Merge)->param(key);
            QVERIFY2(d && d->isScalar() && d->animatable, qPrintable(key));
        }
        p.animated(nodekeys::AlphaGain, 1.0).setKey(0, 1.0);
        p.animated(nodekeys::AlphaGain, 1.0).setKey(20, 0.0);
        QCOMPARE(compmath::mergeParams(p, 0).alphaGain, 1.0f);
        QCOMPARE(compmath::mergeParams(p, 20).alphaGain, 0.0f);
        QVERIFY(std::abs(compmath::mergeParams(p, 10).alphaGain - 0.5f) < 0.01f);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestMergeControls>(argc, argv);
}
#include "test_mergecontrols.moc"
