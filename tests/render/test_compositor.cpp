// SPDX-License-Identifier: GPL-3.0-or-later
// Golden-pixel tests of the Compositor (GPU backend, plus the CPU fallback where it applies):
// solids, transform, opacity, crop, titles, YUV → RGB (BT.601/709, limited/full, 8/10-bit),
// display rotation, transitions, effects, quality levels and zero-copy texture output.
// GPU cases skip without OpenGL; ctest re-runs this test under xvfb-run when available.
#include "RenderTestUtil.h"
#include "media/HwAccel.h"
#include "render/Compositor.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVersionFunctionsFactory>

using namespace kk;
using namespace kk::test;

namespace {

/// Mean grey value of @p image.
double meanGrey(const QImage& image)
{
    double sum = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            sum += qGray(image.pixel(x, y));
    return sum / (double(image.width()) * image.height());
}

/// Colour bars of the YUV fixtures: four 16 px wide bars in a 64×16 frame.
const int kBars[4][3] = {{192, 64, 32}, {30, 200, 90}, {250, 250, 250}, {20, 20, 180}};

/// Generates a lossless (FFV1) Matroska file with the colour bars converted to YUV by swscale with
/// the given matrix/range/pixel format and tagged accordingly (Matroska keeps the colour tags).
bool makeYuvFixture(const QString& path, const char* matrix, const char* range, const char* pixfmt,
    const char* colorspace)
{
    if (QFile::exists(path))
        return true;
    QDir().mkpath(QFileInfo(path).absolutePath());
    auto bars = [](int channel) {
        return QStringLiteral("if(lt(X,16),%1,if(lt(X,32),%2,if(lt(X,48),%3,%4)))")
            .arg(kBars[0][channel])
            .arg(kBars[1][channel])
            .arg(kBars[2][channel])
            .arg(kBars[3][channel]);
    };
    const QString filter = QStringLiteral("color=c=black:s=64x16:d=0.2:r=25,format=rgb24,"
                                          "geq=r='%1':g='%2':b='%3',"
                                          "scale=out_color_matrix=%4:out_range=%5,format=%6")
                               .arg(bars(0), bars(1), bars(2), QLatin1String(matrix),
                                   QLatin1String(range), QLatin1String(pixfmt));
    QProcess ffmpeg;
    ffmpeg.setProcessChannelMode(QProcess::ForwardedChannels);
    const QString tmp = path + QStringLiteral(".tmp.mkv");
    ffmpeg.start(QStringLiteral("ffmpeg"),
        {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i"), filter, QStringLiteral("-c:v"), QStringLiteral("ffv1"),
            QStringLiteral("-color_range"), QLatin1String(range), QStringLiteral("-colorspace"),
            QLatin1String(colorspace), tmp});
    if (!ffmpeg.waitForFinished(60000) || ffmpeg.exitCode() != 0)
        return false;
    return QFile::rename(tmp, path);
}

} // namespace

class TestCompositor : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;
    QString m_fixtureError;

    Compositor& gpu() { return *m_gpu; }

    /// Renders with the backend selected by the current data row ("gpu"/"cpu").
    QImage renderWith(Compositor& c, const TimelineData& tl, int64_t frame, QSize size = {},
        Quality q = Quality::Full)
    {
        return c.renderImage(snap(tl), frame, size, q);
    }

    std::shared_ptr<MediaPathMap> media(const QString& id, const QString& path)
    {
        auto map = std::make_shared<MediaPathMap>();
        map->insert(id, path);
        return map;
    }

    /// Adds "gpu" and "cpu" rows; the test body picks the compositor with backend().
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
        qInfo("GPU compositor: %s", qPrintable(m_gpu->backendDescription()));
        if (!ensureFixtures(&m_fixtureError))
            qWarning("%s", qPrintable(m_fixtureError));
    }

    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    void invalidRequest()
    {
        RenderRequest rq; // no timeline
        const RenderedFramePtr f = gpu().renderAsync(rq).get();
        QVERIFY(f);
        QVERIFY(!f->ok);
    }

    void emptyTimelineIsTransparent_data() { backendRows(); }
    void emptyTimelineIsTransparent()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        const QImage img = renderWith(*c, emptyTimeline(), 0);
        QCOMPARE(img.size(), QSize(320, 180));
        QCOMPARE(img.format(), QImage::Format_ARGB32_Premultiplied);
        QVERIFY(near(px(img, 10, 10), 0, 0, 0, 0, 0));
    }

    void solidFullFrame_data() { backendRows(); }
    void solidFullFrame()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline();
        addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(200, 100, 50), 0, 50));
        const QImage img = renderWith(*c, tl, 10);
        for (const QPoint p : {QPoint(0, 0), QPoint(319, 179), QPoint(160, 90)})
            QVERIFY2(
                near(px(img, p.x(), p.y()), 200, 100, 50, 255, 1), describe(px(img, p.x(), p.y())));
        // Outside the clip's range: nothing.
        QVERIFY(near(px(renderWith(*c, tl, 50), 5, 5), 0, 0, 0, 0, 0));
    }

    void transformZoomPosition_data() { backendRows(); }
    void transformZoomPosition()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline();
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 50));
        ClipData white = solidClip(tl, Qt::white, 0, 50);
        white.transform.setBase(keys::ZoomX, 0.5);
        white.transform.setBase(keys::ZoomY, 0.5);
        white.transform.setBase(keys::PosX, 40);
        white.transform.setBase(keys::PosY, -20);
        addClip(tl, TrackType::Video, 1, white);
        // Rendered at 2× the timeline resolution: position scales with the output.
        // White rect (timeline px): centre (200, 70), 160×90 → x 120..280, y 25..115.
        const QImage img = renderWith(*c, tl, 0, QSize(640, 360));
        auto at = [&](int x, int y) { return px(img, 2 * x, 2 * y); };
        QVERIFY2(near(at(200, 70), 255, 255, 255, 255, 1), describe(at(200, 70)));
        QVERIFY(near(at(122, 27), 255, 255, 255, 255, 1));
        QVERIFY(near(at(278, 113), 255, 255, 255, 255, 1));
        QVERIFY(near(at(117, 70), 0, 0, 0, 255, 1));
        QVERIFY(near(at(283, 70), 0, 0, 0, 255, 1));
        QVERIFY(near(at(200, 22), 0, 0, 0, 255, 1));
        QVERIFY(near(at(200, 118), 0, 0, 0, 255, 1));
    }

    void transformRotationAnchor_data() { backendRows(); }
    void transformRotationAnchor()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline();
        ClipData bar = solidClip(tl, Qt::white, 0, 50);
        bar.transform.setBase(keys::ZoomX, 0.5); // 160 × 45 bar ...
        bar.transform.setBase(keys::ZoomY, 0.25);
        bar.transform.setBase(keys::Rotation, 90); // ... standing upright: 45 × 160
        addClip(tl, TrackType::Video, 0, bar);
        QImage img = renderWith(*c, tl, 0);
        QVERIFY(near(px(img, 160, 20), 255, 255, 255, 255, 2));
        QVERIFY(near(px(img, 160, 160), 255, 255, 255, 255, 2));
        QVERIFY(near(px(img, 120, 90), 0, 0, 0, 0, 0));
        QVERIFY(near(px(img, 200, 90), 0, 0, 0, 0, 0));

        // Anchor alone never moves an unrotated picture; with rotation it is the pivot.
        tl.videoTracks[0].clips[0].transform.setBase(keys::Rotation, 0);
        tl.videoTracks[0].clips[0].transform.setBase(keys::AnchorX, 80);
        img = renderWith(*c, tl, 0);
        QVERIFY(near(px(img, 160, 90), 255, 255, 255, 255, 2)); // still centred
        tl.videoTracks[0].clips[0].transform.setBase(keys::Rotation, 180);
        img = renderWith(*c, tl, 0);
        // Rotated 180° about x = 160 + 80: the bar now spans x 240..400 → visible 240..320.
        QVERIFY(near(px(img, 300, 90), 255, 255, 255, 255, 2));
        QVERIFY(near(px(img, 160, 90), 0, 0, 0, 0, 0));
    }

    void opacity_data() { backendRows(); }
    void opacity()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline();
        ClipData white = solidClip(tl, Qt::white, 0, 50);
        white.transform.setBase(keys::Opacity, 50);
        addClip(tl, TrackType::Video, 1, white);
        // Over nothing: premultiplied half-transparent white.
        QImage img = renderWith(*c, tl, 0);
        QVERIFY2(near(px(img, 100, 100), 128, 128, 128, 128, 2), describe(px(img, 100, 100)));
        // Over red on V1: 50 % blend.
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::red, 0, 50));
        img = renderWith(*c, tl, 0);
        QVERIFY2(near(px(img, 100, 100), 255, 128, 128, 255, 2), describe(px(img, 100, 100)));
        // Disabled clip / disabled track contribute nothing.
        tl.videoTracks[1].clips[0].enabled = false;
        QVERIFY(near(px(renderWith(*c, tl, 0), 100, 100), 255, 0, 0, 255, 1));
        tl.videoTracks[1].clips[0].enabled = true;
        tl.videoTracks[0].enabled = false;
        QVERIFY(near(px(renderWith(*c, tl, 0), 100, 100), 128, 128, 128, 128, 2));
    }

    void keyframedOpacity()
    {
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline();
        ClipData white = solidClip(tl, Qt::white, 100, 50);
        white.transform.animated(keys::Opacity, 100).setKey(0, 0);
        white.transform.animated(keys::Opacity, 100).setKey(10, 100);
        addClip(tl, TrackType::Video, 0, white);
        // Keyframes are clip-local: frame 105 = local 5 = 50 %.
        QVERIFY2(near(px(renderWith(gpu(), tl, 105), 5, 5), 128, 128, 128, 128, 2),
            describe(px(renderWith(gpu(), tl, 105), 5, 5)));
    }

    void crop_data() { backendRows(); }
    void crop()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline();
        ClipData white = solidClip(tl, Qt::white, 0, 50);
        white.transform.setBase(keys::CropL, 80);
        white.transform.setBase(keys::CropB, 30);
        addClip(tl, TrackType::Video, 0, white);
        const QImage img = renderWith(*c, tl, 0);
        QVERIFY(near(px(img, 70, 50), 0, 0, 0, 0, 0));
        QVERIFY(near(px(img, 90, 50), 255, 255, 255, 255, 1));
        QVERIFY(near(px(img, 200, 160), 0, 0, 0, 0, 0));
        QVERIFY(near(px(img, 200, 140), 255, 255, 255, 255, 1));
    }

    void titlePresence_data() { backendRows(); }
    void titlePresence()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline(640, 360);
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 50));
        ClipData title = titleClip(tl, QStringLiteral("HELLO"), 0, 50, 80);
        title.title.setBase(keys::OutlineWidth, 3);
        title.title.setStatic(keys::OutlineColor, QColor(Qt::red));
        addClip(tl, TrackType::Video, 1, title);
        const QImage img = renderWith(*c, tl, 0);
        int white = 0, red = 0, outside = 0;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const Px p = px(img, x, y);
                const bool inCentre = std::abs(x - 320) < 200 && std::abs(y - 180) < 70;
                if (p.r > 200 && p.g > 200 && p.b > 200)
                    (inCentre ? white : outside)++;
                if (p.r > 200 && p.g < 60 && p.b < 60 && inCentre)
                    ++red;
            }
        }
        QVERIFY2(white > 1500, QByteArray::number(white));
        QVERIFY2(red > 300, QByteArray::number(red)); // outline drawn
        QCOMPARE(outside, 0); // text block centred
    }

    /// Bounding box of the white pixels of @p img (empty: none).
    static QRect whiteBox(const QImage& img)
    {
        QRect box;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const Px p = px(img, x, y);
                if (p.r > 200 && p.g > 200 && p.b > 200)
                    box = box.united(QRect(x, y, 1, 1));
            }
        }
        return box;
    }

    /// Mean absolute difference per channel of two images of the same size.
    static double meanDifference(const QImage& a, const QImage& b)
    {
        double sum = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                const Px p = px(a, x, y);
                const Px q = px(b, x, y);
                sum += std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b);
            }
        }
        return sum / (3.0 * a.width() * a.height());
    }

    // Spec 14.7: a title's Position X / Y place its text block (0..1, Y from the bottom), and
    // Anchor says which side of the block sits at Position X; GPU and CPU agree.
    void spec14_7_titlePositionAndAnchor_data() { backendRows(); }
    void spec14_7_titlePositionAndAnchor()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline(640, 360);
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 50));
        ClipData title = titleClip(tl, QStringLiteral("HELLO"), 0, 50, 48);
        title.title.setBase(keys::TextPosX, 0.1);
        title.title.setBase(keys::TextPosY, 0.2);
        title.title.setStatic(keys::Anchor, QStringLiteral("left"));
        addClip(tl, TrackType::Video, 1, title);
        const QRect left = whiteBox(renderWith(*c, tl, 0));
        QVERIFY(!left.isEmpty());
        QVERIFY2(left.left() >= 64 && left.left() <= 72, qPrintable(QString::number(left.left())));
        QVERIFY2(
            std::abs(left.center().y() - 288) <= 6, qPrintable(QString::number(left.center().y())));

        tl.videoTracks[1].clips[0].title.setStatic(keys::Anchor, QStringLiteral("right"));
        tl.videoTracks[1].clips[0].title.setBase(keys::TextPosX, 0.9);
        const QRect right = whiteBox(renderWith(*c, tl, 0));
        QVERIFY2(right.right() <= 576 && right.right() >= 568,
            qPrintable(QString::number(right.right())));
        QCOMPARE(right.width(), left.width());

        // Defaults (0.5 / 0.5, centre): the block is centred, as titles always were.
        ClipData& t = tl.videoTracks[1].clips[0];
        t.title.values.remove(keys::TextPosX);
        t.title.values.remove(keys::TextPosY);
        t.title.statics.remove(keys::Anchor);
        const QRect centred = whiteBox(renderWith(*c, tl, 0));
        QVERIFY(std::abs(centred.center().x() - 320) <= 3);
        QVERIFY(std::abs(centred.center().y() - 180) <= 6);

        // The GPU and the CPU paths draw the same pixels.
        if (m_gpu->isGpu()) {
            t.title.setBase(keys::TextPosX, 0.25);
            t.title.setBase(keys::TextPosY, 0.7);
            t.title.setStatic(keys::BgEnabled, true);
            const QImage g = renderWith(gpu(), tl, 0);
            const QImage cpuImg = renderWith(*m_cpu, tl, 0);
            QCOMPARE(whiteBox(g), whiteBox(cpuImg));
            QVERIFY2(
                meanDifference(g, cpuImg) < 1.0, QByteArray::number(meanDifference(g, cpuImg)));
        }
    }

    // Spec 14.8: a Scroll title rolls up through the frame over the clip's duration: just
    // below the bottom edge on the first frame, centred half-way, just above the top edge on
    // the last frame; GPU and CPU agree.
    void spec14_8_scrollRollsOverTheClip_data() { backendRows(); }
    void spec14_8_scrollRollsOverTheClip()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline(640, 360);
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::black, 0, 101));
        ClipData title = titleClip(tl, QStringLiteral("ONE\nTWO\nTHREE"), 0, 101, 40);
        title.title.setStatic(keys::Scroll, true);
        addClip(tl, TrackType::Video, 1, title);
        QVERIFY(whiteBox(renderWith(*c, tl, 0)).isEmpty());
        QVERIFY(whiteBox(renderWith(*c, tl, 100)).isEmpty());
        const QRect quarter = whiteBox(renderWith(*c, tl, 25));
        const QRect half = whiteBox(renderWith(*c, tl, 50));
        const QRect late = whiteBox(renderWith(*c, tl, 75));
        QVERIFY(!half.isEmpty());
        QVERIFY2(
            std::abs(half.center().y() - 180) <= 8, qPrintable(QString::number(half.center().y())));
        QVERIFY(quarter.center().y() > half.center().y() + 50); // still low: moving up
        QVERIFY(late.center().y() < half.center().y() - 50);
        // Position Y does not matter for a scroll.
        tl.videoTracks[1].clips[0].title.setBase(keys::TextPosY, 0.9);
        QCOMPARE(whiteBox(renderWith(*c, tl, 50)), half);
        if (m_gpu->isGpu()) {
            const QImage g = renderWith(gpu(), tl, 30);
            const QImage cpuImg = renderWith(*m_cpu, tl, 30);
            QCOMPARE(whiteBox(g), whiteBox(cpuImg));
            QVERIFY2(
                meanDifference(g, cpuImg) < 1.0, QByteArray::number(meanDifference(g, cpuImg)));
        }
    }

    void titleCacheIsStable()
    {
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline();
        addClip(tl, TrackType::Video, 0, titleClip(tl, QStringLiteral("Static"), 0, 100, 40));
        const auto shot = snap(tl);
        const QImage first = gpu().renderImage(shot, 0);
        // Same style on later frames hits the cache; the pixels must be identical.
        QElapsedTimer t;
        t.start();
        for (int f = 1; f < 20; ++f)
            QCOMPARE(gpu().renderImage(shot, f), first);
        qInfo("static title: %.2f ms/frame incl. readback", t.elapsed() / 19.0);
    }

    void yuvToRgb_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("matrix");
        QTest::addColumn<QString>("range");
        QTest::addColumn<QString>("pixfmt");
        QTest::addColumn<QString>("colorspace");
        QTest::addColumn<int>("tolerance");
        QTest::newRow("709 limited 420p")
            << "bars_709_tv_420p.mkv" << "bt709" << "tv" << "yuv420p" << "bt709" << 3;
        QTest::newRow("709 full 444p")
            << "bars_709_pc_444p.mkv" << "bt709" << "pc" << "yuv444p" << "bt709" << 3;
        QTest::newRow("709 limited 420p10")
            << "bars_709_tv_420p10.mkv" << "bt709" << "tv" << "yuv420p10le" << "bt709" << 3;
        QTest::newRow("601 limited 422p")
            << "bars_601_tv_422p.mkv" << "bt601" << "tv" << "yuv422p" << "smpte170m" << 3;
    }
    void yuvToRgb()
    {
        QFETCH(QString, file);
        QFETCH(QString, matrix);
        QFETCH(QString, range);
        QFETCH(QString, pixfmt);
        QFETCH(QString, colorspace);
        QFETCH(int, tolerance);
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg CLI not available");
        const QString path = fixtureDir() + QStringLiteral("/render/") + file;
        QVERIFY(makeYuvFixture(path, qPrintable(matrix), qPrintable(range), qPrintable(pixfmt),
            qPrintable(colorspace)));
        TimelineData tl = emptyTimeline(64, 16);
        addClip(tl, TrackType::Video, 0, mediaClip(tl, QStringLiteral("m"), 0, 5));
        gpu().setMediaPaths(media(QStringLiteral("m"), path));
        const QImage img = renderWith(gpu(), tl, 0);
        for (int bar = 0; bar < 4; ++bar) {
            const Px p = px(img, bar * 16 + 8, 8);
            QVERIFY2(near(p, kBars[bar][0], kBars[bar][1], kBars[bar][2], 255, tolerance),
                describe(p) + " bar " + QByteArray::number(bar));
        }
    }

    void displayRotation_data() { backendRows(); }
    void displayRotation()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        if (!m_fixtureError.isEmpty())
            QSKIP(qPrintable(m_fixtureError));
        // Coded 640×360 with a red box at the coded top-left, displayed rotated 90° clockwise:
        // a 360×640 portrait picture with the box (90×160) at the top-right.
        TimelineData tl = emptyTimeline(360, 640);
        addClip(tl, TrackType::Video, 0, mediaClip(tl, QStringLiteral("r"), 0, 25));
        c->setMediaPaths(media(QStringLiteral("r"), fixture("rotated_portrait.mp4")));
        const QImage img = renderWith(*c, tl, 0);
        const Px box = px(img, 360 - 45, 80);
        const Px blue = px(img, 45, 80);
        QVERIFY2(box.r > 180 && box.b < 80, describe(box));
        QVERIFY2(blue.b > 180 && blue.r < 80, describe(blue));
    }

    void mediaFrameAccuracy_data() { backendRows(); }
    void mediaFrameAccuracy()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        if (!m_fixtureError.isEmpty())
            QSKIP(qPrintable(m_fixtureError));
        TimelineData tl = emptyTimeline(1280, 720);
        // Source in at 1 s (frame 25 of the 25 fps fixture), placed at timeline frame 100.
        addClip(tl, TrackType::Video, 0,
            mediaClip(tl, QStringLiteral("v"), 100, 100, 1'000'000, 10'000'000));
        c->setMediaPaths(media(QStringLiteral("v"), fixture("h264_25fps.mp4")));
        const auto shot = snap(tl);
        for (const int f : {100, 101, 102, 110, 140, 105}) // sequential, then a seek back
            QCOMPARE(readFrameIndex(c->renderImage(shot, f)), 25 + (f - 100));
    }

    void hardwareAndSoftwareDecodePaths_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<bool>("hardware");
        QTest::newRow("h264 8-bit, hw (NV12)") << "h264_25fps.mp4" << true;
        QTest::newRow("h264 8-bit, sw (yuv420p)") << "h264_25fps.mp4" << false;
        QTest::newRow("hevc 10-bit, hw (P010)") << "hevc10.mp4" << true;
        QTest::newRow("hevc 10-bit, sw (yuv420p10)") << "hevc10.mp4" << false;
    }
    void hardwareAndSoftwareDecodePaths()
    {
        QFETCH(QString, file);
        QFETCH(bool, hardware);
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        if (!m_fixtureError.isEmpty())
            QSKIP(qPrintable(m_fixtureError));
        // The decoder preference applies to decoders opened afterwards; a fresh compositor has
        // an empty decoder pool. Without a usable GPU decoder "hw" silently decodes in software.
        media::setPreferredHwAccel(hardware ? media::HwAccelMode::Auto : media::HwAccelMode::Off);
        Compositor compositor;
        compositor.setMediaPaths(media(QStringLiteral("v"), fixture(qPrintable(file))));
        TimelineData tl = emptyTimeline(1280, 720);
        addClip(tl, TrackType::Video, 0, mediaClip(tl, QStringLiteral("v"), 0, 75, 0, 3'000'000));
        const auto shot = snap(tl);
        for (const int f : {0, 1, 2, 40}) {
            const QImage img = compositor.renderImage(shot, f);
            QCOMPARE(readFrameIndex(img), f);
            // testsrc2 is mostly saturated colour: an all-grey picture would mean broken chroma.
            int coloured = 0;
            for (int x = 20; x < 1280; x += 40) {
                const Px p = px(img, x, 400);
                coloured += std::max({p.r, p.g, p.b}) - std::min({p.r, p.g, p.b}) > 100;
            }
            QVERIFY2(coloured > 8, QByteArray::number(coloured));
        }
        media::setPreferredHwAccel(media::HwAccelMode::Auto);
    }

    void sameSourceTwice()
    {
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        if (!m_fixtureError.isEmpty())
            QSKIP(qPrintable(m_fixtureError));
        // Two clips of the same file at different source times visible together (PiP): each
        // gets its own decoder, both stay frame-exact.
        TimelineData tl = emptyTimeline(1280, 720);
        addClip(tl, TrackType::Video, 0, mediaClip(tl, QStringLiteral("v"), 0, 100, 0, 10'000'000));
        ClipData pip = mediaClip(tl, QStringLiteral("v"), 0, 100, 4'000'000, 10'000'000);
        pip.transform.setBase(keys::ZoomX, 0.5);
        pip.transform.setBase(keys::ZoomY, 0.5);
        pip.transform.setBase(keys::PosY, 180); // lower half: keeps V1's barcode visible
        addClip(tl, TrackType::Video, 1, pip);
        gpu().setMediaPaths(media(QStringLiteral("v"), fixture("h264_25fps.mp4")));
        const auto shot = snap(tl);
        for (int f = 0; f < 6; ++f) {
            const QImage img = gpu().renderImage(shot, f);
            QCOMPARE(readFrameIndex(img), f);
            // PiP barcode: top 1/8 of the PiP (half size, centred horizontally, at y 360..720).
            QCOMPARE(readFrameIndex(img.copy(320, 360, 640, 360)), 100 + f);
        }
    }

    void crossDissolveMidpoint_data() { backendRows(); }
    void crossDissolveMidpoint()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline();
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::red, 0, 50));
        ClipData blue = solidClip(tl, Qt::blue, 50, 50);
        blue.transitionIn = TransitionData {QStringLiteral("cross_dissolve"), 25, {}};
        addClip(tl, TrackType::Video, 0, blue);
        // Window [38, 63): progress at the cut = 13 / 26 = 0.5.
        QVERIFY2(near(px(renderWith(*c, tl, 50), 50, 50), 128, 0, 128, 255, 2),
            describe(px(renderWith(*c, tl, 50), 50, 50)));
        QVERIFY(near(px(renderWith(*c, tl, 37), 50, 50), 255, 0, 0, 255, 0));
        QVERIFY(near(px(renderWith(*c, tl, 63), 50, 50), 0, 0, 255, 255, 0));
        const Px early = px(renderWith(*c, tl, 40), 50, 50);
        QVERIFY2(early.r > 200 && early.b > 10 && early.b < 60, describe(early));
    }

    void fadeAtTrackEnds()
    {
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline();
        ClipData white = solidClip(tl, Qt::white, 10, 50);
        white.transitionIn = TransitionData {QStringLiteral("cross_dissolve"), 9, {}};
        white.transitionOut = TransitionData {QStringLiteral("cross_dissolve"), 9, {}};
        addClip(tl, TrackType::Video, 0, white);
        // Fade in over [10, 19): frame 14 → progress 5/10.
        QVERIFY2(near(px(renderWith(gpu(), tl, 14), 5, 5), 128, 128, 128, 128, 2),
            describe(px(renderWith(gpu(), tl, 14), 5, 5)));
        // Fade out over [51, 60): frame 55 → progress 5/10 of the way to transparent.
        QVERIFY(near(px(renderWith(gpu(), tl, 55), 5, 5), 128, 128, 128, 128, 2));
        QVERIFY(near(px(renderWith(gpu(), tl, 30), 5, 5), 255, 255, 255, 255, 0));
    }

    void otherTransitions()
    {
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline();
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::red, 0, 50));
        ClipData blue = solidClip(tl, Qt::blue, 50, 50);
        blue.transitionIn = TransitionData {QStringLiteral("wipe"), 25, {}};
        blue.transitionIn->params.setStatic(QStringLiteral("direction"), QStringLiteral("left"));
        blue.transitionIn->params.setBase(QStringLiteral("softness"), 0);
        addClip(tl, TrackType::Video, 0, blue);
        // Wipe left at 50 %: the incoming picture covers the right half.
        QImage img = renderWith(gpu(), tl, 50);
        QVERIFY2(near(px(img, 40, 90), 255, 0, 0, 255, 1), describe(px(img, 40, 90)));
        QVERIFY2(near(px(img, 280, 90), 0, 0, 255, 255, 1), describe(px(img, 280, 90)));

        tl.videoTracks[0].clips[1].transitionIn->type = QStringLiteral("push");
        img = renderWith(gpu(), tl, 50);
        QVERIFY(near(px(img, 40, 90), 255, 0, 0, 255, 1));
        QVERIFY(near(px(img, 280, 90), 0, 0, 255, 255, 1));

        tl.videoTracks[0].clips[1].transitionIn->type = QStringLiteral("dip_to_color");
        tl.videoTracks[0].clips[1].transitionIn->params.setStatic(
            QStringLiteral("color"), QColor(Qt::white));
        QVERIFY2(near(px(renderWith(gpu(), tl, 50), 160, 90), 255, 255, 255, 255, 2),
            describe(px(renderWith(gpu(), tl, 50), 160, 90)));

        tl.videoTracks[0].clips[1].transitionIn->type = QStringLiteral("no_such_transition");
        QVERIFY(near(px(renderWith(gpu(), tl, 50), 160, 90), 128, 0, 128, 255, 2)); // dissolve
    }

    void blurReducesHighFrequencies()
    {
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        if (!m_fixtureError.isEmpty())
            QSKIP(qPrintable(m_fixtureError));
        TimelineData tl = emptyTimeline(640, 360);
        addClip(tl, TrackType::Video, 0,
            mediaClip(tl, QStringLiteral("v"), 0, 100, 2'000'000, 10'000'000));
        gpu().setMediaPaths(media(QStringLiteral("v"), fixture("h264_25fps.mp4")));
        const double sharp = highFrequencyEnergy(renderWith(gpu(), tl, 0));
        EffectInstance blur;
        blur.id = tl.allocateId();
        blur.type = QStringLiteral("gaussian_blur");
        blur.params.setBase(QStringLiteral("radius"), 8);
        tl.videoTracks[0].clips[0].effects.push_back(blur);
        const QImage blurredImg = renderWith(gpu(), tl, 0);
        const double blurred = highFrequencyEnergy(blurredImg);
        qInfo("HF energy: sharp %.0f, blurred %.0f (%.2f)", sharp, blurred, blurred / sharp);
        QVERIFY(blurred < 0.4 * sharp);
        // A huge radius takes the downscaled path and still produces a plausible image.
        tl.videoTracks[0].clips[0].effects[0].params.setBase(QStringLiteral("radius"), 100);
        const QImage huge = renderWith(gpu(), tl, 0);
        QVERIFY(highFrequencyEnergy(huge) < 0.5 * blurred);
        QVERIFY(std::abs(meanGrey(huge) - meanGrey(blurredImg)) < 25);
        // Radius scales with the output size: half quality ≈ same relative smoothing.
        const QImage half = gpu().renderImage(snap(tl), 0, {}, Quality::Half);
        QCOMPARE(half.size(), QSize(320, 180));
    }

    void colourEffects()
    {
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline();
        addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(200, 100, 50), 0, 50));
        auto withEffect
            = [&](const char* type, std::initializer_list<std::pair<const char*, double>> values) {
                  TimelineData t = tl;
                  EffectInstance e;
                  e.id = t.allocateId();
                  e.type = QLatin1String(type);
                  for (const auto& [k, v] : values)
                      e.params.setBase(QLatin1String(k), v);
                  t.videoTracks[0].clips[0].effects.push_back(e);
                  return renderWith(gpu(), t, 0);
              };
        const Px grey = px(withEffect("saturation", {{"saturation", 0}}), 10, 10);
        QVERIFY2(std::abs(grey.r - grey.g) <= 1 && std::abs(grey.g - grey.b) <= 1, describe(grey));
        const Px bright = px(withEffect("brightness_contrast", {{"brightness", 40}}), 10, 10);
        QVERIFY2(near(bright, 251, 151, 101, 255, 2), describe(bright));
        const Px flat = px(withEffect("brightness_contrast", {{"contrast", -100}}), 10, 10);
        QVERIFY2(near(flat, 128, 128, 128, 255, 2), describe(flat));
        const QImage vignette = withEffect("vignette", {{"amount", 100}, {"size", 20}});
        QVERIFY(qGray(vignette.pixel(2, 2)) < 20);
        QVERIFY(near(px(vignette, 160, 90), 200, 100, 50, 255, 2));
        // Effects that are a no-op with their parameters are skipped entirely.
        QVERIFY(
            near(px(withEffect("gaussian_blur", {{"radius", 0}}), 10, 10), 200, 100, 50, 255, 1));
    }

    void geometricEffects()
    {
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        if (!m_fixtureError.isEmpty())
            QSKIP(qPrintable(m_fixtureError));
        TimelineData tl = emptyTimeline(640, 360);
        addClip(tl, TrackType::Video, 0, mediaClip(tl, QStringLiteral("v"), 0, 100, 0, 10'000'000));
        gpu().setMediaPaths(media(QStringLiteral("v"), fixture("h264_25fps.mp4")));
        const QImage plain = renderWith(gpu(), tl, 0);
        auto withEffect
            = [&](const char* type, std::initializer_list<std::pair<const char*, double>> values) {
                  TimelineData t = tl;
                  EffectInstance e;
                  e.id = t.allocateId();
                  e.type = QLatin1String(type);
                  for (const auto& [k, v] : values)
                      e.params.setBase(QLatin1String(k), v);
                  t.videoTracks[0].clips[0].effects.push_back(e);
                  return renderWith(gpu(), t, 0);
              };
        const QImage mirrored = withEffect("mirror", {});
        int matches = 0;
        for (int x = 0; x < 640; x += 37)
            matches += near(px(mirrored, x, 200), px(plain, 639 - x, 200).r,
                px(plain, 639 - x, 200).g, px(plain, 639 - x, 200).b, 255, 6);
        QVERIFY2(matches >= 16, QByteArray::number(matches));
        const QImage mosaic = withEffect("mosaic", {{"blockSize", 40}});
        QVERIFY(near(px(mosaic, 85, 205), px(mosaic, 115, 235).r, px(mosaic, 115, 235).g,
            px(mosaic, 115, 235).b, 255, 1)); // same 40 px block (80..120)
        QVERIFY(near(px(mosaic, 121, 205), px(mosaic, 159, 239).r, px(mosaic, 159, 239).g,
            px(mosaic, 159, 239).b, 255, 1)); // next block (120..160)
        const QImage sharp = withEffect("sharpen", {{"amount", 100}, {"radius", 2}});
        QVERIFY(highFrequencyEnergy(sharp) > 1.15 * highFrequencyEnergy(plain));
    }

    void dropShadowOnTitle()
    {
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline(640, 360);
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::white, 0, 50));
        ClipData title = titleClip(tl, QStringLiteral("I"), 0, 50, 200);
        title.title.setStatic(keys::Color, QColor(Qt::blue));
        addClip(tl, TrackType::Video, 1, title);
        const QImage plain = renderWith(gpu(), tl, 0);
        EffectInstance shadow;
        shadow.id = tl.allocateId();
        shadow.type = QStringLiteral("drop_shadow");
        shadow.params.setBase(QStringLiteral("distance"), 20);
        shadow.params.setBase(QStringLiteral("blur"), 0);
        shadow.params.setBase(QStringLiteral("opacity"), 100);
        tl.videoTracks[1].clips[0].effects.push_back(shadow);
        const QImage shadowed = renderWith(gpu(), tl, 0);
        // Count black (shadow) pixels: none before, plenty after, all down-right of the text.
        int before = 0, after = 0;
        for (int y = 0; y < 360; ++y) {
            for (int x = 0; x < 640; ++x) {
                before += qGray(plain.pixel(x, y)) < 30;
                after += qGray(shadowed.pixel(x, y)) < 30 && qBlue(shadowed.pixel(x, y)) < 30;
            }
        }
        QCOMPARE(before, 0);
        QVERIFY2(after > 500, QByteArray::number(after));
    }

    void qualityLevels()
    {
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline(1920, 1080);
        addClip(tl, TrackType::Video, 0, solidClip(tl, Qt::green, 0, 50));
        const auto shot = snap(tl);
        QCOMPARE(gpu().renderImage(shot, 0, {}, Quality::Half).size(), QSize(960, 540));
        QCOMPARE(gpu().renderImage(shot, 0, {}, Quality::Quarter).size(), QSize(480, 270));
        QCOMPARE(
            gpu().renderImage(shot, 0, QSize(640, 360), Quality::Half).size(), QSize(320, 180));
    }

    void textureOutputIsShared()
    {
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        TimelineData tl = emptyTimeline(64, 32);
        addClip(tl, TrackType::Video, 0, solidClip(tl, QColor(10, 220, 30), 0, 50));
        const RenderedFramePtr f
            = gpu().render(snap(tl), 0, {}, Quality::Full, OutputTexture | OutputImage);
        QVERIFY(f->ok);
        QVERIFY(f->texture != 0);
        QVERIFY(f->fence != nullptr);
        QVERIFY(near(px(f->image, 3, 3), 10, 220, 30, 255, 1));

        // Read the texture back in a different context of the share group (= the viewer).
        QOffscreenSurface surface;
        surface.setFormat(QSurfaceFormat::defaultFormat());
        surface.create();
        QOpenGLContext ctx;
        ctx.setShareContext(QOpenGLContext::globalShareContext());
        QVERIFY(ctx.create());
        QVERIFY(ctx.makeCurrent(&surface));
        auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(&ctx);
        QVERIFY(gl && gl->initializeOpenGLFunctions());
        gl->glWaitSync(static_cast<GLsync>(f->fence), 0, GL_TIMEOUT_IGNORED);
        GLuint fbo = 0;
        gl->glGenFramebuffers(1, &fbo);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl->glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->texture, 0);
        QCOMPARE(gl->glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        uchar rgba[4] = {};
        gl->glReadPixels(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        gl->glDeleteFramebuffers(1, &fbo);
        ctx.doneCurrent();
        QVERIFY2(std::abs(rgba[0] - 10) <= 1 && std::abs(rgba[1] - 220) <= 1 && rgba[3] == 255,
            QByteArray::number(rgba[0]) + "," + QByteArray::number(rgba[1]));
    }

    void pipelinedReadback()
    {
        if (!gpu().isGpu())
            QSKIP("no OpenGL");
        // Several requests in flight (export pattern): results arrive in order and correct.
        TimelineData tl = emptyTimeline();
        ClipData white = solidClip(tl, Qt::white, 0, 100);
        white.transform.animated(keys::Opacity, 100).setKey(0, 0);
        white.transform.animated(keys::Opacity, 100).setKey(100, 100);
        addClip(tl, TrackType::Video, 0, white);
        const auto shot = snap(tl);
        std::vector<std::future<RenderedFramePtr>> futures;
        for (int f = 0; f < 100; f += 10) {
            RenderRequest rq;
            rq.timeline = shot;
            rq.frame = f;
            rq.outputs = OutputImage;
            futures.push_back(gpu().renderAsync(rq));
        }
        for (int i = 0; i < int(futures.size()); ++i) {
            const RenderedFramePtr r = futures[size_t(i)].get();
            QCOMPARE(r->frame, int64_t(i * 10));
            QVERIFY2(std::abs(qAlpha(r->image.pixel(1, 1)) - int(std::lround(i * 25.5))) <= 2,
                QByteArray::number(qAlpha(r->image.pixel(1, 1))) + " frame "
                    + QByteArray::number(i * 10));
        }
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCompositor>(argc, argv);
}

#include "test_compositor.moc"
