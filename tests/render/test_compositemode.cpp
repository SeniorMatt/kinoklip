// SPDX-License-Identifier: GPL-3.0-or-later
// Pixel tests of the Inspector's Composite group (spec 24.19): a clip's Composite Mode blends
// it with the tracks below like the Compositing page's Merge node, with the same pixels on
// the GPU compositor and the CPU fallback; Normal and Opacity stay as before.
#include "RenderTestUtil.h"
#include "core/NodeRegistry.h"
#include "core/ParamKeys.h"
#include "render/CompMath.h"
#include "render/Compositor.h"

#include <QTest>

#include <memory>

using namespace kk;
using namespace kk::test;
namespace compmath = kk::render::compmath;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

const QColor kBelow(200, 100, 40);
const QColor kAbove(64, 200, 128);

/// V1: an orange Solid; V2: a green Solid with composite mode @p mode and opacity @p opacity
/// covering the right half only (moved by half the frame).
TimelineData twoTracks(const QString& mode, double opacity = 100.0)
{
    TimelineData tl = emptyTimeline(kW, kH);
    addClip(tl, TrackType::Video, 0, solidClip(tl, kBelow, 0, 50));
    ClipData top = solidClip(tl, kAbove, 0, 50);
    if (!mode.isEmpty())
        top.transform.setStatic(keys::CompositeMode, mode);
    top.transform.setBase(keys::Opacity, opacity);
    top.transform.setBase(keys::PosX, kW / 2.0);
    addClip(tl, TrackType::Video, 1, top);
    return tl;
}

/// The reference composite of the two colours (straight, opaque background).
Px reference(const QString& mode, double opacity)
{
    const float as = float(opacity / 100.0);
    const float b[4] = {kBelow.redF(), kBelow.greenF(), kBelow.blueF(), 1.0f};
    const float s[4]
        = {float(kAbove.redF()) * as, float(kAbove.greenF()) * as, float(kAbove.blueF()) * as, as};
    float o[4];
    compmath::blendPremultiplied(blendmodes::index(mode), b, s, o);
    const auto q = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return {q(o[0]), q(o[1]), q(o[2]), q(o[3])};
}

} // namespace

class TestCompositeMode : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

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

    // Spec 24.19: every composite mode gives the Merge node's formula on both backends; the
    // left half (nothing above) keeps the clip below unchanged.
    void spec24_19_compositeModesMatchMergeOnGpuAndCpu()
    {
        for (const QString& mode : blendmodes::compositeOrder()) {
            for (const double opacity : {100.0, 60.0}) {
                const TimelineData tl = twoTracks(mode, opacity);
                const QImage g = m_gpu->renderImage(snap(tl), 0);
                const QImage c = m_cpu->renderImage(snap(tl), 0);
                const Px want = reference(mode, opacity);
                const QByteArray what = (mode + QStringLiteral(" %1 %").arg(opacity)).toUtf8();
                const Px gp = px(g, 240, 90);
                const Px cp = px(c, 240, 90);
                QVERIFY2(near(gp, want.r, want.g, want.b, 255, 3),
                    what + " gpu " + describe(gp) + " want " + describe(want));
                QVERIFY2(near(cp, gp.r, gp.g, gp.b, gp.a, 3),
                    what + " cpu " + describe(cp) + " gpu " + describe(gp));
                QVERIFY2(near(px(g, 80, 90), kBelow.red(), kBelow.green(), kBelow.blue(), 255, 2),
                    what + " gpu outside " + describe(px(g, 80, 90)));
                QVERIFY2(near(px(c, 80, 90), kBelow.red(), kBelow.green(), kBelow.blue(), 255, 2),
                    what + " cpu outside " + describe(px(c, 80, 90)));
            }
        }
    }

    // No composite mode stored (every older project) is Normal: the same pixels.
    void spec24_19_absentModeIsNormal()
    {
        const TimelineData absent = twoTracks(QString(), 60.0);
        const TimelineData normal = twoTracks(blendmodes::Normal, 60.0);
        QCOMPARE(m_gpu->renderImage(snap(absent), 0), m_gpu->renderImage(snap(normal), 0));
        QCOMPARE(m_cpu->renderImage(snap(absent), 0), m_cpu->renderImage(snap(normal), 0));
    }

    // Multiply over a transparent area (nothing below) shows the clip itself.
    void spec24_19_modeOverNothingShowsTheClip()
    {
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData top = solidClip(tl, kAbove, 0, 50);
        top.transform.setStatic(keys::CompositeMode, blendmodes::Multiply);
        addClip(tl, TrackType::Video, 1, top);
        for (const QImage& img :
            {m_gpu->renderImage(snap(tl), 0), m_cpu->renderImage(snap(tl), 0)}) {
            const Px p = px(img, 160, 90);
            QVERIFY2(near(p, kAbove.red(), kAbove.green(), kAbove.blue(), 255, 2), describe(p));
        }
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCompositeMode>(argc, argv);
}
#include "test_compositemode.moc"
