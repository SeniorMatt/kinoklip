// SPDX-License-Identifier: GPL-3.0-or-later
// Retimed clips in the compositor: exact source frame selection along a retime curve (GPU and
// CPU agree), frame blend weights, optical flow against a moving box, and nested clips.
#include "RenderTestUtil.h"
#include "core/ParamKeys.h"
#include "core/Retime.h"
#include "render/Compositor.h"

#include <QProcess>
#include <QTemporaryDir>

using namespace kk;
using namespace kk::test;

class TestRetimeRender : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;
    QString m_fixtureError;
    QTemporaryDir m_tmp;
    QString m_boxVideo; ///< white 40 px box moving 8 px per frame on black (empty = no ffmpeg)

    std::shared_ptr<MediaPathMap> media(const QString& path)
    {
        auto map = std::make_shared<MediaPathMap>();
        map->insert(QStringLiteral("v"), path);
        return map;
    }

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

    /// 0..10 at 100 %, 10..30 at 50 %, 30..40 reversed at 100 % (source 25 fps barcodes).
    static ClipData curvedClip(TimelineData& tl)
    {
        ClipData c = mediaClip(tl, QStringLiteral("v"), 0, 40, 0, 10'000'000);
        AnimatedValue& v = c.retime.animated(keys::RetimeFrame, 0.0);
        v.setKey(0, 0.0);
        v.setKey(10, 10.0);
        v.setKey(30, 20.0);
        v.setKey(40, 10.0);
        return c;
    }

    /// First x of row @p y brighter than mid grey, or -1.
    static int boxLeft(const QImage& image, int y)
    {
        for (int x = 0; x < image.width(); ++x) {
            if (qGray(image.pixel(x, y)) > 128)
                return x;
        }
        return -1;
    }

    /// Number of half-bright pixels (ghosts, soft edges) in row @p y of @p image.
    static int countMidGrey(const QImage& image, int y)
    {
        int n = 0;
        for (int x = 0; x < image.width(); ++x) {
            const int g = qGray(image.pixel(x, y));
            n += g > 60 && g < 190;
        }
        return n;
    }

private slots:
    void initTestCase()
    {
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);
        if (!ensureFixtures(&m_fixtureError))
            qWarning("%s", qPrintable(m_fixtureError));
        // Moving box: frame n has the box at x = 8 n (lossless so edges stay sharp).
        const QString out = m_tmp.filePath(QStringLiteral("box.mkv"));
        QProcess ff;
        ff.start(QStringLiteral("ffmpeg"),
            {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                QStringLiteral("-i"), QStringLiteral("color=c=black:s=320x180:r=25:d=1"),
                QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                QStringLiteral("color=c=white:s=40x40:r=25:d=1"), QStringLiteral("-filter_complex"),
                QStringLiteral("[0][1]overlay=x='8*n':y=70"), QStringLiteral("-c:v"),
                QStringLiteral("ffv1"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv444p"),
                out});
        if (ff.waitForFinished(60000) && ff.exitCode() == 0)
            m_boxVideo = out;
    }

    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    void curveSelectsExactSourceFrames_data() { backendRows(); }
    void curveSelectsExactSourceFrames()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        if (!m_fixtureError.isEmpty())
            QSKIP(qPrintable(m_fixtureError));
        TimelineData tl = emptyTimeline(1280, 720);
        addClip(tl, TrackType::Video, 0, curvedClip(tl));
        c->setMediaPaths(media(fixture("h264_25fps.mp4")));
        const auto shot = snap(tl);
        const ClipData& clip = shot->videoTracks[0].clips[0];
        // Playback order, then seeks: the frame shown is always the one the curve maps to.
        for (const int f : {0, 1, 9, 10, 11, 12, 13, 29, 30, 31, 35, 39, 20, 3}) {
            const int expected = int(clip.sourceTimeUsAt(f, tl.fps) / 40000);
            QCOMPARE(readFrameIndex(c->renderImage(shot, f)), expected);
        }
    }

    void frameBlendMixesNeighbours()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        if (m_boxVideo.isEmpty())
            QSKIP("ffmpeg could not make the moving-box video");
        TimelineData tl = emptyTimeline(320, 180);
        tl.retimeProcess = RetimeProcess::FrameBlend;
        ClipData c = mediaClip(tl, QStringLiteral("v"), 0, 50, 0, 1'000'000);
        c.speed = 0.5;
        addClip(tl, TrackType::Video, 0, c);
        for (Compositor* comp : {m_gpu.get(), m_cpu.get()}) {
            comp->setMediaPaths(media(m_boxVideo));
            const auto shot = snap(tl);
            // Frame 10 = source frame 5 exactly: a clean box (40 px; it moves 8 px per frame).
            const QImage exact = comp->renderImage(shot, 10);
            const int x0 = boxLeft(exact, 90);
            QVERIFY(x0 >= 0);
            QCOMPARE(qGray(exact.pixel(x0 + 5, 90)), 255);
            QVERIFY(countMidGrey(exact, 90) <= 2);
            // Frame 11 = halfway to source frame 6: both boxes at half strength where they do
            // not overlap (x0 .. x0 + 8 and x0 + 40 .. x0 + 48).
            const QImage half = comp->renderImage(shot, 11);
            const int ghost = qGray(half.pixel(x0 + 4, 90));
            QVERIFY2(ghost > 100 && ghost < 160, QByteArray::number(ghost));
            QVERIFY2(std::abs(qGray(half.pixel(x0 + 44, 90)) - ghost) < 12, "ghosts differ");
            QCOMPARE(qGray(half.pixel(x0 + 20, 90)), 255); // overlap of both boxes
        }
    }

    void opticalFlowMovesTheBox()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        if (m_boxVideo.isEmpty())
            QSKIP("ffmpeg could not make the moving-box video");
        TimelineData tl = emptyTimeline(320, 180);
        ClipData c = mediaClip(tl, QStringLiteral("v"), 0, 50, 0, 1'000'000);
        c.speed = 0.5;
        c.retimeProcess = RetimeProcess::OpticalFlow;
        addClip(tl, TrackType::Video, 0, c);
        m_gpu->setMediaPaths(media(m_boxVideo));
        const auto shot = snap(tl);
        // Halfway between source frames 5 (box at x0) and 6 (x0 + 8): one box at x0 + 4, with
        // (almost) no half-strength ghost bands, unlike the blend.
        const int x0 = boxLeft(m_gpu->renderImage(shot, 10), 90);
        QVERIFY(x0 >= 0);
        const QImage flow = m_gpu->renderImage(shot, 11);
        QVERIFY2(countMidGrey(flow, 90) <= 4, QByteArray::number(countMidGrey(flow, 90)));
        QVERIFY(qGray(flow.pixel(x0 + 6, 90)) > 220);
        QVERIFY(qGray(flow.pixel(x0 + 42, 90)) > 220);
        QVERIFY(qGray(flow.pixel(x0, 90)) < 40); // left of the moved box
        QVERIFY(qGray(flow.pixel(x0 + 47, 90)) < 40); // right of it
        // Exact source frames are not touched by the interpolation.
        const QImage exact = m_gpu->renderImage(shot, 12);
        QCOMPARE(boxLeft(exact, 90), x0 + 8);
        QVERIFY(countMidGrey(exact, 90) <= 2);
    }

    void reverseAndFreezeFrames_data() { backendRows(); }
    void reverseAndFreezeFrames()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        if (!m_fixtureError.isEmpty())
            QSKIP(qPrintable(m_fixtureError));
        TimelineData tl = emptyTimeline(1280, 720);
        ClipData rev = mediaClip(tl, QStringLiteral("v"), 0, 20, 2'000'000, 10'000'000);
        rev.speed = -1.0; // shows source frames 50, 49, ...
        addClip(tl, TrackType::Video, 0, rev);
        ClipData freeze = mediaClip(tl, QStringLiteral("v"), 20, 20, 1'200'000, 10'000'000);
        freeze.speed = 0.0;
        addClip(tl, TrackType::Video, 0, freeze);
        c->setMediaPaths(media(fixture("h264_25fps.mp4")));
        const auto shot = snap(tl);
        QCOMPARE(readFrameIndex(c->renderImage(shot, 0)), 50);
        QCOMPARE(readFrameIndex(c->renderImage(shot, 1)), 49);
        QCOMPARE(readFrameIndex(c->renderImage(shot, 19)), 31);
        QCOMPARE(readFrameIndex(c->renderImage(shot, 20)), 30);
        QCOMPARE(readFrameIndex(c->renderImage(shot, 39)), 30);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestRetimeRender>(argc, argv);
}

#include "test_retimerender.moc"
