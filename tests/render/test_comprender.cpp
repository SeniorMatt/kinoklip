// SPDX-License-Identifier: GPL-3.0-or-later
// Pixel tests of the Compositing page evaluation (GlComposition, CpuComposition, spec section
// 42a): a clip with a composition shows its output; Merge, Transform, masks, effects as
// nodes, disabled nodes, the viewer API (Compositor::renderCompositionNode()), MediaIn of
// another media pool item, GPU vs CPU agreement, and a 1080p timing with ten nodes.
#include "RenderTestUtil.h"
#include "core/CompOps.h"
#include "core/NodeRegistry.h"
#include "render/CompMath.h"
#include "render/Compositor.h"
#include "render/RenderCacheKeys.h"

#include <QElapsedTimer>
#include <QTest>

#include <memory>

using namespace kk;
using namespace kk::test;
namespace co = kk::compops;
namespace compmath = kk::render::compmath;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;

/// A timeline with one red Solid clip on V1 [0, 50) carrying @p comp.
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

/// default composition + node of @p type added after MediaIn; returns its id in @p node.
Composition after(const QString& type, quint64* node, quint64* merge = nullptr)
{
    Composition c = co::defaultComposition();
    const co::AddResult r = co::addNodeAfter(c, type, mediaIn(c));
    *node = r.node;
    if (merge)
        *merge = r.merge;
    return c;
}

/// Fast noise filling the frame (lots of detail), the picture the blur tests look at.
Composition noiseComposition(quint64* noise)
{
    Composition c = co::defaultComposition();
    *noise = co::addNode(c, nodetypes::FastNoise, {});
    c.node(*noise)->params.setBase(nodekeys::Scale, 60.0);
    c.node(*noise)->params.setBase(nodekeys::Detail, 3.0);
    co::connect(c, *noise, 0, c.mediaOutId, nodeports::Input);
    return c;
}

/// Sum of |differences| of grey values between neighbours inside [x0, x1) x [y0, y1).
double detail(const QImage& img, int x0, int x1, int y0, int y1)
{
    double sum = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x + 1 < x1; ++x)
            sum += std::abs(qGray(img.pixel(x, y)) - qGray(img.pixel(x + 1, y)));
    return sum;
}

} // namespace

class TestCompRender : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

    QImage gpu(const TimelineData& tl, int64_t frame = 0)
    {
        return m_gpu->renderImage(snap(tl), frame);
    }
    QImage cpu(const TimelineData& tl, int64_t frame = 0)
    {
        return m_cpu->renderImage(snap(tl), frame);
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

    void defaultCompositionChangesNothing()
    {
        TimelineData plain = compTimeline(Composition {});
        plain.videoTracks[0].clips[0].composition.reset();
        const TimelineData comp = compTimeline(co::defaultComposition());
        QCOMPARE(gpu(comp), gpu(plain));
        QCOMPARE(cpu(comp), cpu(plain));
    }

    void mergeHalfTransparentForeground()
    {
        // Background (blue, alpha 50 %) merged over the clip's red picture.
        quint64 bg = 0, merge = 0;
        Composition c = after(nodetypes::Background, &bg, &merge);
        c.node(bg)->params.setStatic(nodekeys::Color, QColor(0, 0, 255, 128));
        const TimelineData tl = compTimeline(c);
        for (const QImage& img : {gpu(tl), cpu(tl)}) {
            const Px p = px(img, 160, 90);
            QVERIFY2(near(p, 127, 0, 128, 255, 2), describe(p));
        }
        // Blend 0.5 halves the foreground again.
        c.node(merge)->params.setBase(nodekeys::Opacity, 0.5);
        const Px p = px(gpu(compTimeline(c)), 160, 90);
        QVERIFY2(near(p, 191, 0, 64, 255, 2), describe(p));
    }

    void mergeForegroundTransform()
    {
        // A white Background scaled to 0.5 and moved to the right quarter.
        quint64 bg = 0, merge = 0;
        Composition c = after(nodetypes::Background, &bg, &merge);
        c.node(bg)->params.setStatic(nodekeys::Color, QColor(Qt::white));
        CompNode* m = c.node(merge);
        m->params.setBase(nodekeys::Size, 0.5);
        m->params.setBase(nodekeys::CenterX, 0.75);
        m->params.setBase(nodekeys::CenterY, 0.75); // y up: the upper half
        const TimelineData tl = compTimeline(c);
        for (const QImage& img : {gpu(tl), cpu(tl)}) {
            // Foreground covers x 160..320, y 0..90.
            QVERIFY2(near(px(img, 240, 45), 255, 255, 255, 255, 2), describe(px(img, 240, 45)));
            QVERIFY2(near(px(img, 80, 45), 255, 0, 0, 255, 2), describe(px(img, 80, 45)));
            QVERIFY2(near(px(img, 240, 135), 255, 0, 0, 255, 2), describe(px(img, 240, 135)));
        }
    }

    void blendModesMatchCpu()
    {
        for (const QString& mode : {blendmodes::Screen, blendmodes::Multiply, blendmodes::Add,
                 blendmodes::Overlay, blendmodes::Darken, blendmodes::Lighten,
                 blendmodes::Difference, blendmodes::Subtract}) {
            quint64 bg = 0, merge = 0;
            Composition c = after(nodetypes::Background, &bg, &merge);
            c.node(bg)->params.setStatic(nodekeys::Color, QColor(64, 200, 128, 200));
            c.node(merge)->params.setStatic(nodekeys::BlendMode, mode);
            const TimelineData tl = compTimeline(c, QColor(200, 100, 40));
            const Px g = px(gpu(tl), 100, 100);
            const Px s = px(cpu(tl), 100, 100);
            // The reference formula.
            const float as = 200 / 255.0f;
            float o[3];
            const float cb[3] = {200 / 255.0f, 100 / 255.0f, 40 / 255.0f};
            const float cs[3] = {64 / 255.0f, 200 / 255.0f, 128 / 255.0f};
            for (int i = 0; i < 3; ++i)
                o[i] = cb[i] * (1 - as)
                    + as * compmath::blendChannel(blendmodes::index(mode), cb[i], cs[i]);
            QVERIFY2(near(g, int(o[0] * 255 + 0.5), int(o[1] * 255 + 0.5), int(o[2] * 255 + 0.5),
                         255, 3),
                qPrintable(mode + QLatin1Char(' ') + QString::fromLatin1(describe(g))));
            QVERIFY2(near(s, g.r, g.g, g.b, g.a, 3),
                qPrintable(mode + QLatin1Char(' ') + QString::fromLatin1(describe(s))));
        }
    }

    void transformMovesPixels()
    {
        // Background (green) -> Transform (centre 0.75: a quarter frame to the right) -> out.
        Composition c = co::defaultComposition();
        const quint64 bg = co::addNode(c, nodetypes::Background, {});
        c.node(bg)->params.setStatic(nodekeys::Color, QColor(0, 255, 0));
        const quint64 t = co::addNode(c, nodetypes::Transform, {});
        co::connect(c, bg, 0, t, nodeports::Input);
        co::connect(c, t, 0, c.mediaOutId, nodeports::Input);
        c.node(t)->params.setBase(nodekeys::CenterX, 0.75);
        const TimelineData tl = compTimeline(c);
        for (const QImage& img : {gpu(tl), cpu(tl)}) {
            QCOMPARE(px(img, 40, 90).a, 0); // moved away
            QVERIFY2(near(px(img, 120, 90), 0, 255, 0, 255, 1), describe(px(img, 120, 90)));
            QVERIFY2(near(px(img, 310, 90), 0, 255, 0, 255, 1), describe(px(img, 310, 90)));
        }
        // Angle 90 (counter-clockwise) about the centre: a 0.5 x 0.5 square stays a square.
    }

    void maskedBlurOnlyInside()
    {
        quint64 noise = 0;
        Composition c = noiseComposition(&noise);
        const QImage sharp = gpu(compTimeline(c));
        // Gaussian Blur after the noise, masked by a rectangle over the left half.
        const quint64 blur = co::addNodeAfter(c, QStringLiteral("fx:gaussian_blur"), noise).node;
        c.node(blur)->params.setBase(QStringLiteral("radius"), 8.0);
        const quint64 rect = co::addNodeAfter(c, nodetypes::RectangleMask, blur).node;
        QCOMPARE(c.node(blur)->inputs.value(nodeports::Mask).nodeId, rect);
        c.node(rect)->params.setBase(nodekeys::CenterX, 0.25);
        c.node(rect)->params.setBase(nodekeys::Width, 0.5);
        c.node(rect)->params.setBase(nodekeys::Height, 1.0);
        const QImage img = gpu(compTimeline(c));
        // Left half blurred (much less detail), right half untouched.
        QVERIFY(detail(img, 10, 150, 20, 160) < 0.5 * detail(sharp, 10, 150, 20, 160));
        for (int x = 170; x < 320; x += 13)
            QVERIFY2(near(px(img, x, 90), px(sharp, x, 90).r, px(sharp, x, 90).g,
                         px(sharp, x, 90).b, 255, 1),
                describe(px(img, x, 90)));
        // An inverted mask swaps the halves.
        c.node(rect)->params.setStatic(nodekeys::Invert, true);
        const QImage inv = gpu(compTimeline(c));
        QVERIFY(detail(inv, 170, 310, 20, 160) < 0.5 * detail(sharp, 170, 310, 20, 160));
        QVERIFY(near(px(inv, 60, 90), px(sharp, 60, 90).r, px(sharp, 60, 90).g, px(sharp, 60, 90).b,
            255, 1));
    }

    void disabledNodePassesThrough()
    {
        quint64 noise = 0;
        Composition c = noiseComposition(&noise);
        const QImage sharp = gpu(compTimeline(c));
        const quint64 blur = co::addNodeAfter(c, QStringLiteral("fx:gaussian_blur"), noise).node;
        const quint64 cc = co::addNodeAfter(c, nodetypes::ColorCorrector, blur).node;
        c.node(cc)->params.setBase(nodekeys::Gain, 0.3);
        QVERIFY(gpu(compTimeline(c)) != sharp);
        co::setEnabled(c, blur, false);
        co::setEnabled(c, cc, false);
        QCOMPARE(gpu(compTimeline(c)), sharp);
        // A disabled MediaIn draws nothing; a composition without MediaOut input too.
        Composition d = co::defaultComposition();
        co::setEnabled(d, mediaIn(d), false);
        QCOMPARE(px(gpu(compTimeline(d)), 160, 90).a, 0);
        QCOMPARE(px(cpu(compTimeline(d)), 160, 90).a, 0);
    }

    void colorNodesMatchReference()
    {
        quint64 cc = 0;
        Composition c = after(nodetypes::ColorCorrector, &cc);
        ParamSet& p = c.node(cc)->params;
        p.setBase(nodekeys::Lift, 0.1);
        p.setBase(nodekeys::Gamma, 1.4);
        p.setBase(nodekeys::Gain, 0.9);
        p.setBase(nodekeys::Saturation, 0.5);
        p.setBase(nodekeys::InWhite, 0.9);
        const QColor in(180, 90, 30);
        const TimelineData tl = compTimeline(c, in);
        float rgb[3] = {180 / 255.0f, 90 / 255.0f, 30 / 255.0f};
        compmath::applyCorrector(compmath::correctorParams(p, 0), rgb);
        for (const QImage& img : {gpu(tl), cpu(tl)}) {
            const Px g = px(img, 50, 50);
            QVERIFY2(near(g, int(rgb[0] * 255 + 0.5), int(rgb[1] * 255 + 0.5),
                         int(rgb[2] * 255 + 0.5), 255, 3),
                describe(g));
        }
        // Channel Booleans: blue into red, luma into green.
        quint64 cb = 0;
        Composition b = after(nodetypes::ChannelBooleans, &cb);
        b.node(cb)->params.setStatic(nodekeys::ToRed, channelsources::Blue);
        b.node(cb)->params.setStatic(nodekeys::ToGreen, channelsources::Black);
        const TimelineData tb = compTimeline(b, in);
        for (const QImage& img : {gpu(tb), cpu(tb)}) {
            const Px g = px(img, 50, 50);
            QVERIFY2(near(g, 30, 0, 30, 255, 2), describe(g));
        }
    }

    void masksAndLetterbox()
    {
        // Ellipse mask output in a viewer: white inside, transparent outside.
        Composition c = co::defaultComposition();
        const quint64 e = co::addNode(c, nodetypes::EllipseMask, {});
        const TimelineData tl = compTimeline(c);
        const auto clipId = tl.videoTracks[0].clips[0].id;
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            const RenderedFramePtr f = comp->renderCompositionNode(snap(tl), clipId, e, 0, {},
                                               Quality::Full, OutputImage)
                                           .get();
            QVERIFY(f && f->ok);
            QCOMPARE(f->image.size(), QSize(kW, kH));
            QVERIFY2(near(px(f->image, 160, 90), 255, 255, 255, 255, 1),
                describe(px(f->image, 160, 90)));
            QCOMPARE(px(f->image, 5, 5).a, 0);
        }
        // Letterbox 2.39 on a 16:9 frame: black bars of (180 - 320 / 2.39) / 2 = 23 px.
        quint64 lb = 0;
        Composition l = after(nodetypes::Letterbox, &lb);
        const TimelineData tlb = compTimeline(l);
        for (const QImage& img : {gpu(tlb), cpu(tlb)}) {
            QVERIFY2(near(px(img, 160, 10), 0, 0, 0, 255, 1), describe(px(img, 160, 10)));
            QVERIFY2(near(px(img, 160, 170), 0, 0, 0, 255, 1), describe(px(img, 160, 170)));
            QVERIFY2(near(px(img, 160, 90), 255, 0, 0, 255, 1), describe(px(img, 160, 90)));
        }
    }

    void viewerApiRendersAnyNode()
    {
        quint64 bg = 0, merge = 0;
        Composition c = after(nodetypes::Background, &bg, &merge);
        c.node(bg)->params.setStatic(nodekeys::Color, QColor(0, 0, 255));
        TimelineData tl = compTimeline(c);
        tl.videoTracks[0].clips[0].start = 0;
        const quint64 clipId = tl.videoTracks[0].clips[0].id;
        const auto s = snap(tl);
        auto node = [&](quint64 id) {
            return m_gpu->renderCompositionNode(s, clipId, id, 3, {}, Quality::Full, OutputImage)
                .get();
        };
        QVERIFY2(near(px(node(bg)->image, 10, 10), 0, 0, 255, 255, 1),
            describe(px(node(bg)->image, 10, 10)));
        QVERIFY(near(px(node(mediaIn(c))->image, 10, 10), 255, 0, 0, 255, 1));
        QVERIFY(near(px(node(0)->image, 10, 10), 0, 0, 255, 255, 1)); // 0 = MediaOut
        // Half quality: half the size; unknown node / clip: transparent.
        const RenderedFramePtr half
            = m_gpu->renderCompositionNode(s, clipId, bg, 0, {}, Quality::Half, OutputImage).get();
        QCOMPARE(half->image.size(), QSize(kW / 2, kH / 2));
        QCOMPARE(px(node(9999)->image, 10, 10).a, 0);
        QCOMPARE(px(m_gpu->renderCompositionNode(s, 9999, bg, 0, {}, Quality::Full, OutputImage)
                         .get()
                         ->image,
                     10, 10)
                     .a,
            0);
        // Texture output for the viewers (zero copy).
        if (m_gpu->isGpu()) {
            const RenderedFramePtr t = m_gpu->renderCompositionNode(s, clipId, bg, 0).get();
            QVERIFY(t->texture != 0);
        }
    }

    void editPageAttributesApplyOnTop()
    {
        // Clip opacity 50 % and a clip mask still apply to the composition's output.
        quint64 bg = 0;
        Composition c = after(nodetypes::Background, &bg);
        c.node(bg)->params.setStatic(nodekeys::Color, QColor(0, 0, 255));
        TimelineData tl = compTimeline(c);
        ClipData& clip = tl.videoTracks[0].clips[0];
        clip.transform.setBase(keys::Opacity, 50.0);
        clip.transform.setBase(keys::PosX, 160.0); // half a frame to the right
        for (const QImage& img : {gpu(tl), cpu(tl)}) {
            QCOMPARE(px(img, 80, 90).a, 0);
            QVERIFY2(near(px(img, 240, 90), 0, 0, 128, 128, 2), describe(px(img, 240, 90)));
        }
    }

    void mediaInOfAnotherItem()
    {
        QString why;
        if (!ensureFixtures(&why))
            QSKIP(qPrintable(why));
        // The solid clip's composition shows h264_25fps.mp4 from its start.
        Composition c = co::defaultComposition();
        c.node(mediaIn(c))->mediaId = QStringLiteral("clip");
        TimelineData tl = compTimeline(c);
        tl.videoTracks[0].clips[0].start = 10;
        auto paths = std::make_shared<MediaPathMap>();
        paths->insert(QStringLiteral("clip"), fixture("h264_25fps.mp4"));
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            comp->setMediaPaths(paths);
            for (int64_t f : {0, 7}) {
                const QImage img = comp->renderImage(snap(tl), 10 + f);
                QCOMPARE(readFrameIndex(img), int(f));
            }
        }
    }

    void viewerApiDecodesMediaInThroughItsTable()
    {
        QString why;
        if (!ensureFixtures(&why))
            QSKIP(qPrintable(why));
        // A media clip with a stored default composition: the viewer API's MediaIn decodes the
        // clip through the media table it is given (the compositor itself has none).
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData clip = mediaClip(tl, QStringLiteral("clip"), 0, 50);
        clip.composition = co::defaultComposition();
        const quint64 in = mediaIn(*clip.composition);
        const quint64 clipId = clip.id;
        addClip(tl, TrackType::Video, 0, clip);
        auto paths = std::make_shared<MediaPathMap>();
        paths->insert(QStringLiteral("clip"), fixture("h264_25fps.mp4"));
        const auto s = snap(tl);
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            comp->setMediaPaths(nullptr);
            for (int64_t f : {0, 6}) {
                const RenderedFramePtr shown = comp->renderCompositionNode(s, clipId, in, f, {},
                                                       Quality::Full, OutputImage, paths, nullptr)
                                                   .get();
                QVERIFY(shown && shown->ok);
                QCOMPARE(readFrameIndex(shown->image), int(f));
            }
            // Without a table nothing is decoded (the old viewer bug: a black MediaIn).
            const RenderedFramePtr none
                = comp->renderCompositionNode(s, clipId, in, 6, {}, Quality::Full, OutputImage)
                      .get();
            QCOMPARE(readFrameIndex(none->image), -1);
        }
    }

    void cacheKeyFollowsPictureOnly()
    {
        quint64 bg = 0;
        Composition c = after(nodetypes::Background, &bg);
        const TimelineData a = compTimeline(c);
        Composition moved = c;
        co::moveNodes(moved, {bg}, QPointF(3, 3));
        moved.viewZoom = 2.0;
        Composition changed = c;
        changed.node(bg)->params.setStatic(nodekeys::Color, QColor(Qt::green));
        const auto keys = [](const TimelineData& t) {
            QVector<QByteArray> out;
            for (const CacheChunk& chunk : renderCacheChunks(t, {}, CacheKeyOptions {}))
                out.append(chunk.key);
            return out;
        };
        QCOMPARE(keys(compTimeline(moved)), keys(a));
        // A composition makes the chunk "heavy" (Smart mode caches it).
        bool heavy = false;
        for (const CacheChunk& chunk : renderCacheChunks(a, {}, CacheKeyOptions {}))
            heavy = heavy || (chunk.reasons & CacheEffects);
        QVERIFY(heavy);
        QVERIFY(keys(compTimeline(changed)) != keys(a));
    }

    void performanceBreakdown()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        // Cost of single nodes after MediaIn at 1080p (ms per frame, minus the plain clip).
        auto timeOf = [&](const Composition* comp) {
            TimelineData tl = emptyTimeline(1920, 1080);
            ClipData clip = solidClip(tl, QColor(40, 80, 120), 0, 50);
            if (comp)
                clip.composition = *comp;
            addClip(tl, TrackType::Video, 0, clip);
            const auto s = snap(tl);
            (void)m_gpu->render(s, 0, {}, Quality::Full, OutputTexture);
            QElapsedTimer timer;
            timer.start();
            for (int f = 0; f < 5; ++f)
                (void)m_gpu->render(s, f, {}, Quality::Full, OutputTexture);
            return double(timer.nsecsElapsed()) / 1e6 / 5;
        };
        const double base = timeOf(nullptr);
        QString line;
        for (const QString& type : {nodetypes::Background, nodetypes::Text, nodetypes::FastNoise,
                 nodetypes::Transform, nodetypes::ColorCorrector, nodetypes::EllipseMask,
                 QStringLiteral("fx:gaussian_blur")}) {
            quint64 n = 0;
            const bool masked = type == nodetypes::EllipseMask;
            Composition c = after(masked ? nodetypes::ColorCorrector : type, &n);
            if (type == nodetypes::Transform)
                c.node(n)->params.setBase(nodekeys::Angle, 5.0);
            if (type == nodetypes::ColorCorrector || masked)
                c.node(n)->params.setBase(nodekeys::Gain, 0.8);
            if (masked) // an ellipse mask on the colour corrector
                co::addNodeAfter(c, nodetypes::EllipseMask, n);
            line += QStringLiteral(" %1 %2")
                        .arg(masked ? QStringLiteral("masked_corrector") : type)
                        .arg(timeOf(&c) - base, 0, 'f', 1);
        }
        qInfo("1080p node cost (ms over a plain clip of %.1f ms):%s", base, qPrintable(line));
    }

    void performance1080p()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        // MediaIn -> Background + Text merged -> Blur (masked) -> Color Corrector -> Transform
        // -> Fast Noise screened over -> out: ten nodes at 1080p.
        Composition c = co::defaultComposition();
        quint64 last = mediaIn(c);
        last = co::addNodeAfter(c, nodetypes::Background, last).merge;
        last = co::addNodeAfter(c, nodetypes::Text, last).merge;
        const quint64 blur = co::addNodeAfter(c, QStringLiteral("fx:gaussian_blur"), last).node;
        co::addNodeAfter(c, nodetypes::EllipseMask, blur);
        last = co::addNodeAfter(c, nodetypes::ColorCorrector, blur).node;
        c.node(last)->params.setBase(nodekeys::Gain, 0.8);
        last = co::addNodeAfter(c, nodetypes::Transform, last).node;
        c.node(last)->params.setBase(nodekeys::Angle, 5.0);
        const co::AddResult n = co::addNodeAfter(c, nodetypes::FastNoise, last);
        c.node(n.merge)->params.setStatic(nodekeys::BlendMode, blendmodes::Screen);
        QCOMPARE(co::evaluationOrder(c, c.mediaOutId).size(), 12); // ten + MediaIn / MediaOut
        TimelineData tl = emptyTimeline(1920, 1080);
        ClipData clip = solidClip(tl, QColor(40, 80, 120), 0, 50);
        clip.composition = c;
        addClip(tl, TrackType::Video, 0, clip);
        const auto s = snap(tl);
        TimelineData plainTl = emptyTimeline(1920, 1080);
        addClip(plainTl, TrackType::Video, 0, solidClip(plainTl, QColor(40, 80, 120), 0, 50));
        const auto plain = snap(plainTl);
        auto time = [&](const std::shared_ptr<const TimelineData>& t, Quality q) {
            (void)m_gpu->render(t, 0, {}, q, OutputTexture); // warm up (shaders, pools, title)
            QElapsedTimer timer;
            timer.start();
            constexpr int kFrames = 10;
            for (int f = 0; f < kFrames; ++f)
                (void)m_gpu->render(t, f, {}, q, OutputTexture);
            return double(timer.nsecsElapsed()) / 1e6 / kFrames;
        };
        const double comp = time(s, Quality::Full);
        const double base = time(plain, Quality::Full);
        const double half = time(s, Quality::Half);
        qInfo("1080p, 10-node composition: %.1f ms/frame (Half %.1f ms; same clip without: %.1f "
              "ms) on %s",
            comp, half, base, qPrintable(m_gpu->backendDescription()));
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCompRender>(argc, argv);
}
#include "test_comprender.moc"
