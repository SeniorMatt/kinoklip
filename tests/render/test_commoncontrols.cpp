// SPDX-License-Identifier: GPL-3.0-or-later
// Pixel tests of the Common Controls (Settings) of the Compositing page (spec section 91):
// Blend, Process Red / Green / Blue / Alpha, Apply Mask Inverted, Multiply By Mask and Process
// When Blend Is 0.0. Every test looks at the GPU and the CPU renderer: they must show the
// same picture.
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
namespace k = kk::nodekeys;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

TimelineData compTimeline(const Composition& comp, QColor color)
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

/// MediaIn (yellow) -> Color Corrector with Gain 0.5 (so (255, 255, 0) becomes (128, 128, 0)).
Composition darkened(quint64* node)
{
    Composition c = co::defaultComposition();
    *node = co::addNodeAfter(c, nodetypes::ColorCorrector, mediaIn(c)).node;
    c.node(*node)->params.setBase(k::Gain, 0.5);
    return c;
}

/// A Rectangle Mask over the left half, plugged into @p node's Effect Mask.
void maskLeftHalf(Composition& c, quint64 node)
{
    const quint64 rect = co::addNodeAfter(c, nodetypes::RectangleMask, node).node;
    c.node(rect)->params.setBase(k::CenterX, 0.25);
    c.node(rect)->params.setBase(k::Width, 0.5);
    c.node(rect)->params.setBase(k::Height, 1.0);
}

} // namespace

class TestCommonControls : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;
    const QColor m_yellow = QColor(255, 255, 0);

    template<typename Fn>
    void both(const Composition& comp, Fn&& check)
    {
        const TimelineData tl = compTimeline(comp, m_yellow);
        check(m_gpu->renderImage(snap(tl), 0), "gpu");
        check(m_cpu->renderImage(snap(tl), 0), "cpu");
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

    void defaultsAddNoKeys()
    {
        for (const NodeDescriptor& d : NodeRegistry::instance().all()) {
            const ParamSet p = NodeRegistry::instance().defaultParams(d.id);
            for (const ParamDescriptor& c : NodeRegistry::instance().commonControls(d.id))
                QVERIFY2(p.staticValue(c.key).isNull() && p.value(c.key, 0, -7) == -7,
                    qPrintable(d.id + c.key));
        }
        QVERIFY(NodeRegistry::instance().commonControls(nodetypes::MediaOut).isEmpty());
        QVERIFY(NodeRegistry::instance().commonControls(nodetypes::RectangleMask).isEmpty());
        // Merge has its own Blend: no second one.
        for (const ParamDescriptor& c : NodeRegistry::instance().commonControls(nodetypes::Merge))
            QVERIFY(c.key != k::CommonBlend);
        QVERIFY(!compmath::commonParams(CompNode(), 0).needsPass());
    }

    void blendMixesInputAndResult()
    {
        quint64 cc = 0;
        Composition c = darkened(&cc);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 100, 90), 128, 128, 0, 255, 2), who);
        });
        c.node(cc)->params.setBase(k::CommonBlend, 0.5);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 100, 90), 191, 191, 0, 255, 3), who);
        });
        c.node(cc)->params.setBase(k::CommonBlend, 0.0);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 100, 90), 255, 255, 0, 255, 2), who);
        });
    }

    void unprocessedChannelsComeFromTheInput()
    {
        quint64 cc = 0;
        Composition c = darkened(&cc);
        c.node(cc)->params.setStatic(k::CommonRed, false);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 100, 90), 255, 128, 0, 255, 2), who);
        });
        c.node(cc)->params.setStatic(k::CommonRed, true);
        c.node(cc)->params.setStatic(k::CommonGreen, false);
        c.node(cc)->params.setStatic(k::CommonBlue, false);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 100, 90), 128, 255, 0, 255, 2), who);
        });
    }

    void unprocessedAlphaKeepsTheInputAlpha()
    {
        // A Background has no input (transparent): with Alpha off it stays transparent, so
        // the Merge shows only the clip.
        Composition c = co::defaultComposition();
        const co::AddResult r = co::addNodeAfter(c, nodetypes::Background, mediaIn(c));
        c.node(r.node)->params.setStatic(k::Color, QColor(0, 255, 0));
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 100, 90), 0, 255, 0, 255, 2), who);
        });
        c.node(r.node)->params.setStatic(k::CommonAlpha, false);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 100, 90), 255, 255, 0, 255, 2), who);
        });
    }

    void maskLimitsTheEffectAndInvertSwapsHalves()
    {
        quint64 cc = 0;
        Composition c = darkened(&cc);
        maskLeftHalf(c, cc);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 40, 90), 128, 128, 0, 255, 2), who);
            QVERIFY2(near(px(img, 280, 90), 255, 255, 0, 255, 2), who);
        });
        c.node(cc)->params.setStatic(k::MaskInverted, true);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 40, 90), 255, 255, 0, 255, 2), who);
            QVERIFY2(near(px(img, 280, 90), 128, 128, 0, 255, 2), who);
        });
    }

    void multiplyByMaskClearsOutsideTheMask()
    {
        quint64 cc = 0;
        Composition c = darkened(&cc);
        maskLeftHalf(c, cc);
        c.node(cc)->params.setStatic(k::MultiplyByMask, true);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 40, 90), 128, 128, 0, 255, 2), who);
            QCOMPARE(px(img, 280, 90).a, 0);
        });
        c.node(cc)->params.setStatic(k::MaskInverted, true);
        both(c, [](const QImage& img, const char* who) {
            QCOMPARE(px(img, 40, 90).a, 0);
            QVERIFY2(near(px(img, 280, 90), 128, 128, 0, 255, 2), who);
        });
    }

    void mergeUsesItsMaskInvertedToo()
    {
        Composition c = co::defaultComposition();
        const co::AddResult r = co::addNodeAfter(c, nodetypes::Background, mediaIn(c));
        c.node(r.node)->params.setStatic(k::Color, QColor(0, 255, 0));
        maskLeftHalf(c, r.merge);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 40, 90), 0, 255, 0, 255, 2), who);
            QVERIFY2(near(px(img, 280, 90), 255, 255, 0, 255, 2), who);
        });
        c.node(r.merge)->params.setStatic(k::MaskInverted, true);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 40, 90), 255, 255, 0, 255, 2), who);
            QVERIFY2(near(px(img, 280, 90), 0, 255, 0, 255, 2), who);
        });
    }

    void processWhenBlendIsZeroOffSkipsTheTool()
    {
        quint64 cc = 0;
        Composition c = darkened(&cc);
        c.node(cc)->params.setBase(k::CommonBlend, 0.0);
        QVERIFY(!compmath::commonParams(*c.node(cc), 0).skipped());
        c.node(cc)->params.setStatic(k::ProcessAtZero, false);
        QVERIFY(compmath::commonParams(*c.node(cc), 0).skipped());
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 100, 90), 255, 255, 0, 255, 2), who);
        });
        // Not at zero: the tool works.
        c.node(cc)->params.setBase(k::CommonBlend, 1.0);
        both(c, [](const QImage& img, const char* who) {
            QVERIFY2(near(px(img, 100, 90), 128, 128, 0, 255, 2), who);
        });
    }

    void blendIsKeyframeable()
    {
        quint64 cc = 0;
        Composition c = darkened(&cc);
        ParamSet& p = c.node(cc)->params;
        const ParamDescriptor d
            = NodeRegistry::instance().commonControls(nodetypes::ColorCorrector).first();
        QVERIFY(d.key == k::CommonBlend && d.isScalar() && d.animatable);
        p.animated(k::CommonBlend, 1.0).setKey(0, 1.0);
        p.animated(k::CommonBlend, 1.0).setKey(20, 0.0);
        QVERIFY(std::abs(compmath::commonParams(*c.node(cc), 10).blend - 0.5f) < 0.01f);
    }

    void pixelMath()
    {
        compmath::CommonParams cp;
        cp.blend = 0.5f;
        cp.channels[0] = false;
        const float in[4] = {0.8f, 0.4f, 0.0f, 0.8f}; // straight (1, 0.5, 0), alpha 0.8
        const float res[4] = {0.2f, 0.2f, 0.2f, 0.4f}; // straight 0.5 all, alpha 0.4
        float o[4];
        compmath::applyCommon(cp, in, res, nullptr, o);
        // Red from the input (straight 1 x alpha 0.4), green / blue from the result; half way.
        QVERIFY(std::abs(o[0] - 0.6f) < 1e-5f);
        QVERIFY(std::abs(o[1] - 0.3f) < 1e-5f);
        QVERIFY(std::abs(o[2] - 0.1f) < 1e-5f);
        QVERIFY(std::abs(o[3] - 0.6f) < 1e-5f);
        const float m = 0.25f;
        cp = compmath::CommonParams();
        cp.multiplyByMask = true;
        compmath::applyCommon(cp, in, res, &m, o);
        QVERIFY(std::abs(o[3] - 0.1f) < 1e-5f);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCommonControls>(argc, argv);
}
#include "test_commoncontrols.moc"
