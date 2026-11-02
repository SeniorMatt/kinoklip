// SPDX-License-Identifier: GPL-3.0-or-later
// Generators render deterministically and agree between the GPU shader, the CPU raster and the
// documented formulas (bar colours, grey steps, gradient ends, noise statistics); burnt-in
// captions appear where the style puts them and only when asked for.
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/Subtitles.h"
#include "render/Compositor.h"
#include "render/Generators.h"

using namespace kk;
using namespace kk::test;
using namespace kk::render;

namespace {

namespace g = generators;

ClipData generatorClip(TimelineData& tl, const QString& id, int64_t start, int64_t duration)
{
    ClipData c;
    c.id = tl.allocateId();
    c.kind = ClipKind::Generator;
    c.generator = id;
    c.title = EffectRegistry::instance().defaultParams(id);
    c.start = start;
    c.duration = duration;
    return c;
}

TimelineData single(const QString& id, int64_t duration = 100)
{
    TimelineData tl = emptyTimeline(320, 180);
    addClip(tl, TrackType::Video, 0, generatorClip(tl, id, 0, duration));
    return tl;
}

int maxDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return 255;
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            worst = std::max({worst, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
        }
    }
    return worst;
}

double meanGrey(const QImage& image)
{
    double sum = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            sum += qGray(image.pixel(x, y));
    return sum / (image.width() * image.height());
}

/// Bounding box of pixels brighter than @p threshold.
QRect brightBox(const QImage& image, int threshold)
{
    QRect box;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) > threshold)
                box |= QRect(x, y, 1, 1);
    return box;
}

} // namespace

class TestGenerators : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;

    void backendRows()
    {
        QTest::addColumn<bool>("useGpu");
        QTest::newRow("gpu") << true;
        QTest::newRow("cpu") << false;
    }
    Compositor* backend()
    {
        QFETCH(bool, useGpu);
        if (useGpu && !m_gpu->isGpu())
            return nullptr;
        return useGpu ? m_gpu.get() : m_cpu.get();
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

    void everyGeneratorIsRegistered()
    {
        for (const QString& id : {g::FourColorGradient, g::LinearGradient, g::RadialGradient,
                 g::ColorBars, g::GreyScale, g::Noise, g::Countdown, g::Timecode}) {
            const EffectDescriptor* d = EffectRegistry::instance().find(id);
            QVERIFY2(d && d->category == EffectCategory::Generator, qPrintable(id));
            QCOMPARE(d->storage, QStringLiteral("title"));
        }
        const EffectDescriptor* adj = EffectRegistry::instance().find(g::AdjustmentClip);
        QVERIFY(adj && adj->category == EffectCategory::Effects);
    }

    /// The same frame twice gives identical pixels, and GPU and CPU agree.
    void deterministic_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<int>("tolerance");
        QTest::newRow("four") << g::FourColorGradient << 2;
        QTest::newRow("linear") << g::LinearGradient << 2;
        QTest::newRow("radial") << g::RadialGradient << 2;
        QTest::newRow("bars") << g::ColorBars << 1;
        QTest::newRow("grey") << g::GreyScale << 1;
        QTest::newRow("noise") << g::Noise << 1;
        QTest::newRow("countdown") << g::Countdown << 255; // text antialiasing differs
        QTest::newRow("timecode") << g::Timecode << 255;
    }
    void deterministic()
    {
        QFETCH(QString, id);
        QFETCH(int, tolerance);
        const auto tl = snap(single(id));
        const QImage cpu1 = m_cpu->renderImage(tl, 7);
        const QImage cpu2 = m_cpu->renderImage(tl, 7);
        QCOMPARE(maxDifference(cpu1, cpu2), 0);
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        const QImage gpu1 = m_gpu->renderImage(tl, 7);
        const QImage gpu2 = m_gpu->renderImage(tl, 7);
        QCOMPARE(maxDifference(gpu1, gpu2), 0);
        QVERIFY(!gpu1.convertToFormat(QImage::Format_Alpha8).isNull()
            && gpu1.pixelColor(0, 0).isValid());
        bool visible = false; // draws something
        for (int y = 0; y < gpu1.height() && !visible; ++y)
            for (int x = 0; x < gpu1.width() && !visible; ++x)
                visible = qAlpha(gpu1.pixel(x, y)) > 0;
        QVERIFY(visible);
        if (tolerance < 255) {
            // Compare away from bar/step boundaries' single-pixel rounding.
            int off = 0;
            for (int y = 0; y < 180; ++y)
                for (int x = 0; x < 320; ++x)
                    if (maxDifference(gpu1.copy(x, y, 1, 1), cpu1.copy(x, y, 1, 1)) > tolerance)
                        ++off;
            QVERIFY2(off < 320 * 180 / 100, qPrintable(QString::number(off)));
        }
    }

    void colorBars_data() { backendRows(); }
    void colorBars()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData tl = single(g::ColorBars);
        QImage img = c->renderImage(snap(tl), 0);
        // Top row: 75 % white, yellow, cyan, green, magenta, red, blue (bar centres).
        const int expected[7][3] = {{191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
            {191, 0, 191}, {191, 0, 0}, {0, 0, 191}};
        for (int i = 0; i < 7; ++i) {
            const Px p = px(img, int((i + 0.5) * 320 / 7), 40);
            QVERIFY2(near(p, expected[i][0], expected[i][1], expected[i][2], 255, 1), describe(p));
        }
        QVERIFY(near(px(img, 20, 128), 0, 0, 191, 255, 1)); // reverse band: blue first
        QVERIFY(near(px(img, 75, 160), 255, 255, 255, 255, 1)); // bottom row: 100 % white
        QCOMPARE(colorBarsAt(0.1, 0.1, false), qRgb(191, 191, 191));
        tl.videoTracks[0].clips[0].title.setStatic(g::Style, QStringLiteral("full"));
        img = c->renderImage(snap(tl), 0);
        QVERIFY(near(px(img, 20, 90), 255, 255, 255, 255, 1));
        QVERIFY(near(px(img, 300, 90), 0, 0, 0, 255, 1));
    }

    void greyScaleAndGradients_data() { backendRows(); }
    void greyScaleAndGradients()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData grey = single(g::GreyScale);
        grey.videoTracks[0].clips[0].title.setBase(g::Steps, 5);
        QImage img = c->renderImage(snap(grey), 0);
        for (int i = 0; i < 5; ++i) {
            const int level = int(std::lround(255.0 * i / 4));
            QVERIFY2(near(px(img, 32 + i * 64, 90), level, level, level, 255, 1),
                describe(px(img, 32 + i * 64, 90)));
        }
        TimelineData four = single(g::FourColorGradient);
        img = c->renderImage(snap(four), 0);
        QVERIFY2(near(px(img, 0, 0), 0xd9, 0x4f, 0x4f, 255, 4), describe(px(img, 0, 0)));
        QVERIFY2(near(px(img, 319, 179), 0x4f, 0xb0, 0x70, 255, 4), describe(px(img, 319, 179)));
        TimelineData radial = single(g::RadialGradient);
        radial.videoTracks[0].clips[0].title.setBase(g::Radius, 100);
        img = c->renderImage(snap(radial), 0);
        QVERIFY(qGray(img.pixel(160, 90)) > 250); // inner white
        QVERIFY(qGray(img.pixel(5, 5)) < 5); // outer black beyond the radius
        // Animated parameter: the linear gradient's angle is keyframeable.
        TimelineData linear = single(g::LinearGradient);
        AnimatedValue& angle = linear.videoTracks[0].clips[0].title.animated(g::Angle, 0);
        angle.setKey(0, 0.0);
        angle.setKey(10, 90.0);
        const QImage a = c->renderImage(snap(linear), 0);
        const QImage b = c->renderImage(snap(linear), 10);
        QVERIFY(qGray(a.pixel(10, 90)) < qGray(a.pixel(310, 90))); // left -> right
        QVERIFY(qGray(b.pixel(160, 10)) < qGray(b.pixel(160, 170))); // top -> bottom
    }

    void noise_data() { backendRows(); }
    void noise()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData tl = single(g::Noise);
        const QImage f0 = c->renderImage(snap(tl), 0);
        const QImage f1 = c->renderImage(snap(tl), 1);
        QVERIFY(std::abs(meanGrey(f0) - 127.5) < 6); // brightness 50 %
        QVERIFY(maxDifference(f0, f1) > 100); // animated: a new pattern every frame
        tl.videoTracks[0].clips[0].title.setStatic(g::Animated, false);
        QCOMPARE(maxDifference(c->renderImage(snap(tl), 0), c->renderImage(snap(tl), 5)), 0);
        TimelineData other = tl;
        other.videoTracks[0].clips[0].title.setBase(g::Seed, 7);
        QVERIFY(maxDifference(c->renderImage(snap(tl), 0), c->renderImage(snap(other), 0)) > 100);
        QCOMPARE(noiseHash(1, 2, 3), noiseHash(1, 2, 3));
        QVERIFY(noiseHash(1, 2, 3) != noiseHash(2, 1, 3));
    }

    void countdownAndTimecode_data() { backendRows(); }
    void countdownAndTimecode()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        // 3 s countdown at 25 fps: "3" in the first second, "1" in the last.
        TimelineData tl = single(g::Countdown, 75);
        const auto s = snap(tl);
        const QImage first = c->renderImage(s, 0);
        const QImage last = c->renderImage(s, 60);
        QVERIFY(maxDifference(first, last) > 100);
        // Same number, different sweep -> pictures differ (the hand moves).
        QVERIFY(maxDifference(c->renderImage(s, 2), c->renderImage(s, 20)) > 50);
        const GeneratorFrame gf = resolveGenerator(tl.videoTracks[0].clips[0], 60, 60, tl.fps);
        QCOMPARE(gf.textStyle.text, QStringLiteral("1"));
        QCOMPARE(resolveGenerator(tl.videoTracks[0].clips[0], 0, 0, tl.fps).textStyle.text,
            QStringLiteral("3"));

        TimelineData tc = single(g::Timecode);
        tc.videoTracks[0].clips[0].title.setBase(keys::Size, 24);
        const QImage t = c->renderImage(snap(tc), 30);
        // The text block sits below the centre (39 % of the height).
        const QRect box = brightBox(t, 200);
        QVERIFY2(!box.isEmpty() && box.center().y() > 120,
            qPrintable(QStringLiteral("%1,%2").arg(box.center().x()).arg(box.center().y())));
        QCOMPARE(resolveGenerator(tc.videoTracks[0].clips[0], 30, 30, tc.fps).textStyle.text,
            QStringLiteral("00:00:01:05"));
        QCOMPARE(generatorTimecode(25 * 3600 + 1, {25, 1}), QStringLiteral("01:00:00:01"));
    }

    // ---- Captions ----

    void burnInCaptions_data() { backendRows(); }
    void burnInCaptions()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline(320, 180);
        TrackData st = subtitles::makeSubtitleTrack();
        st.id = tl.allocateId();
        st.subtitleStyle.setBase(keys::Size, 30);
        st.subtitleStyle.setStatic(keys::Shadow, false);
        ClipData caption = subtitles::makeCaption(QStringLiteral("Hello"), 10, 20);
        caption.id = tl.allocateId();
        st.clips.push_back(caption);
        tl.subtitleTracks.push_back(st);
        const QImage on = c->renderImage(snap(tl), 15);
        const QRect box = brightBox(on, 200);
        QVERIFY(!box.isEmpty());
        // Bottom centre, above the 7 % margin.
        QVERIFY2(
            std::abs(box.center().x() - 160) < 8, qPrintable(QString::number(box.center().x())));
        QVERIFY2(box.bottom() <= 180 - 180 * 0.07 + 3 && box.top() > 120,
            qPrintable(QStringLiteral("%1-%2").arg(box.top()).arg(box.bottom())));
        // Outside the caption, with the track hidden, or with burn-in off: nothing.
        QVERIFY(brightBox(c->renderImage(snap(tl), 31), 200).isEmpty());
        TimelineData hidden = tl;
        hidden.subtitleTracks[0].enabled = false;
        QVERIFY(brightBox(c->renderImage(snap(hidden), 15), 200).isEmpty());
        // Top position, left aligned (a caption with its own style overrides the track style).
        TimelineData top = tl;
        top.subtitleTracks[0].clips[0].title.setStatic(subtitles::Position, QStringLiteral("top"));
        // ... ignored until the caption has its own style
        QCOMPARE(brightBox(c->renderImage(snap(top), 15), 200), box);
        top.subtitleTracks[0].clips[0].title.setStatic(subtitles::OwnStyle, true);
        top.subtitleTracks[0].clips[0].title.setStatic(keys::Align, QStringLiteral("left"));
        const QRect topBox = brightBox(c->renderImage(snap(top), 15), 200);
        QVERIFY2(topBox.top() < 40 && topBox.left() < 60,
            qPrintable(QStringLiteral("%1,%2").arg(topBox.left()).arg(topBox.top())));
    }

    void burnInSwitch()
    {
        TimelineData tl = emptyTimeline(320, 180);
        TrackData st = subtitles::makeSubtitleTrack();
        st.id = tl.allocateId();
        ClipData caption = subtitles::makeCaption(QStringLiteral("Hello"), 0, 20);
        caption.id = tl.allocateId();
        st.clips.push_back(caption);
        tl.subtitleTracks.push_back(st);
        for (Compositor* c : {m_gpu.get(), m_cpu.get()}) {
            RenderRequest rq;
            rq.timeline = snap(tl);
            rq.frame = 5;
            rq.outputs = OutputImage;
            rq.subtitles = false;
            const RenderedFramePtr f = c->renderAsync(rq).get();
            QVERIFY(f && f->ok);
            QVERIFY(brightBox(f->image, 200).isEmpty());
        }
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestGenerators>(argc, argv);
}

#include "test_generators.moc"
