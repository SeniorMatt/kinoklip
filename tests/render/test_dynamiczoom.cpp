// SPDX-License-Identifier: GPL-3.0-or-later
// Pixel tests of Dynamic Zoom (spec 24.20): a clip's framing rectangle fills the frame, moving
// from the start to the end rectangle with the chosen ease, with the same pixels on the GPU
// compositor and the CPU fallback; a request without Dynamic Zoom (the viewer's on-screen
// controls) shows the whole picture.
#include "RenderTestUtil.h"
#include "core/DynamicZoom.h"
#include "core/ParamKeys.h"
#include "render/Compositor.h"

#include <QTest>

#include <memory>

using namespace kk;
using namespace kk::test;
namespace dz = kk::dynamiczoom;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;
constexpr int64_t kLength = 51; // frames 0..50: frame 25 is half way

const QColor kColour(64, 200, 128);

/// A Solid cropped to its left half (x 0..160 on screen), Dynamic Zoom from the left half
/// (start: the solid fills the frame) to the whole frame (end: the solid covers the left half).
TimelineData zoomed(const QString& ease)
{
    TimelineData tl = emptyTimeline(kW, kH);
    ClipData clip = solidClip(tl, kColour, 0, kLength);
    clip.transform.setBase(keys::CropR, kW / 2.0);
    clip.transform.setStatic(keys::DynamicZoom, true);
    clip.transform.setStatic(keys::DynamicZoomEase, ease);
    dz::setStart(clip.transform, {0.25, 0.5, 0.5});
    dz::setEnd(clip.transform, {0.5, 0.5, 1.0});
    addClip(tl, TrackType::Video, 0, clip);
    return tl;
}

bool isColour(const Px& p)
{
    return near(p, kColour.red(), kColour.green(), kColour.blue(), 255, 3);
}

bool isEmpty(const Px& p)
{
    return p.r < 8 && p.g < 8 && p.b < 8;
}

} // namespace

class TestDynamicZoom : public QObject
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

    // The maths: defaults are a slow push-in, the eases, swap.
    void spec24_20_rectanglesAndEases()
    {
        ClipData clip;
        clip.duration = kLength;
        QCOMPARE(dz::start(clip.transform), dz::Rect {});
        QVERIFY(dz::end(clip.transform).size < 1.0); // push-in: the end shows less
        QCOMPARE(dz::end(clip.transform).cx, 0.5);
        QVERIFY(!dz::isEnabled(clip.transform));
        QVERIFY(dz::transform(clip, 10, QSizeF(kW, kH)).isIdentity());
        QCOMPARE(dz::eased(0.5, QStringLiteral("linear")), 0.5);
        QCOMPARE(dz::eased(0.5, QStringLiteral("easeIn")), 0.25);
        QCOMPARE(dz::eased(0.5, QStringLiteral("easeOut")), 0.75);
        QCOMPARE(dz::eased(0.25, QStringLiteral("easeInOut")), 0.25 * 0.25 * (3 - 0.5));
        dz::setStart(clip.transform, {0.2, 0.3, 0.4});
        dz::swap(clip.transform);
        QCOMPARE(dz::end(clip.transform), (dz::Rect {0.2, 0.3, 0.4}));
        QCOMPARE(dz::start(clip.transform), dz::defaultEnd());
        QCOMPARE(dz::at(clip, 0), dz::defaultEnd());
        QCOMPARE(dz::at(clip, kLength - 1), (dz::Rect {0.2, 0.3, 0.4}));
    }

    // The start rectangle fills the frame on the first frame, the end one on the last; half
    // way the edge of the solid sits where the linear (or eased) rectangle puts it; GPU and
    // CPU give the same pixels.
    void spec24_20_dynamicZoomRendersOnGpuAndCpu()
    {
        struct Case
        {
            QString ease;
            int64_t frame;
            int colourX; ///< a column inside the solid
            int emptyX; ///< a column right of its edge
        };
        // Half way: linear rect (0.375, 0.75) puts the edge at 160 + 40 / 0.75 = 213; ease in
        // (t = 0.25): rect (0.3125, 0.625) -> 160 + 60 / 0.625 = 256; ease out (t = 0.75): rect
        // (0.4375, 0.875) -> 160 + 20 / 0.875 = 183.
        const Case cases[] = {
            {QStringLiteral("linear"), 0, 300, -1},
            {QStringLiteral("linear"), kLength - 1, 150, 170},
            {QStringLiteral("linear"), 25, 205, 222},
            {QStringLiteral("easeIn"), 25, 248, 265},
            {QStringLiteral("easeOut"), 25, 175, 192},
        };
        for (const Case& c : cases) {
            const auto tl = snap(zoomed(c.ease));
            const QImage g = m_gpu->renderImage(tl, c.frame);
            const QImage cpu = m_cpu->renderImage(tl, c.frame);
            const QByteArray what = (c.ease + QStringLiteral(" @%1").arg(c.frame)).toUtf8();
            for (const QImage* img : {&g, &cpu}) {
                const char* backend = img == &g ? " gpu " : " cpu ";
                QVERIFY2(isColour(px(*img, c.colourX, 90)),
                    what + backend + describe(px(*img, c.colourX, 90)));
                if (c.emptyX >= 0)
                    QVERIFY2(isEmpty(px(*img, c.emptyX, 90)),
                        what + backend + describe(px(*img, c.emptyX, 90)));
            }
        }
        // Off: the cropped solid covers the left half only.
        TimelineData off = zoomed(QStringLiteral("linear"));
        off.videoTracks[0].clips[0].transform.setStatic(keys::DynamicZoom, false);
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            const QImage img = comp->renderImage(snap(off), 0);
            QVERIFY(isColour(px(img, 150, 90)));
            QVERIFY(isEmpty(px(img, 170, 90)));
        }
    }

    // A request without Dynamic Zoom (the viewer's on-screen Dynamic Zoom controls) renders
    // the whole picture, as if it were off.
    void spec24_20_requestWithoutDynamicZoom()
    {
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            RenderRequest rq;
            rq.timeline = snap(zoomed(QStringLiteral("linear")));
            rq.frame = 0;
            rq.outputs = OutputImage;
            rq.dynamicZoom = false;
            const RenderedFramePtr f = comp->renderAsync(rq).get();
            QVERIFY(f && f->ok);
            QVERIFY2(isColour(px(f->image, 150, 90)), describe(px(f->image, 150, 90)));
            QVERIFY2(isEmpty(px(f->image, 170, 90)), describe(px(f->image, 170, 90)));
        }
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestDynamicZoom>(argc, argv);
}
#include "test_dynamiczoom.moc"
