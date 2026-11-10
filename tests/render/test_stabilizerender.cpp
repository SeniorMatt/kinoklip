// SPDX-License-Identifier: GPL-3.0-or-later
// Pixel tests of Stabilization in the compositor (spec section 58): shaky footage (a PNG
// sequence with a known camera path) is measured by the Stabilizer and rendered by the GPU
// compositor and the CPU fallback. Alignment with the first frame, Strength 0, Zoom coverage,
// trim / reverse / speed keeping the right frames, clips without data, and GPU = CPU in
// perspective.
#include "../compositing/ShakyFootage.h"
#include "RenderTestUtil.h"
#include "compositing/tracking/StabilizeJob.h"
#include "core/MediaItem.h"
#include "core/Serialization.h"
#include "render/Compositor.h"

#include <QTemporaryDir>

#include <array>

using namespace kk;
using namespace kk::test;
namespace st = kk::stabilize;

namespace {

constexpr int kW = 320;
constexpr int kH = 180;
constexpr int kFrames = 32;

/// Mean absolute grey difference over the inner part of the frame (the corrected picture has a
/// moving empty band at its edges when Zoom is off).
double innerDifference(const QImage& a, const QImage& b)
{
    double sum = 0;
    int n = 0;
    for (int y = 24; y < kH - 24; ++y) {
        for (int x = 24; x < kW - 24; ++x) {
            sum += std::abs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y)));
            ++n;
        }
    }
    return sum / n;
}

int blackPixels(const QImage& image)
{
    int n = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x)
            n += qGray(image.pixel(x, y)) < 20;
    }
    return n;
}

/// A stored camera path of @p mode with one entry per pose (translation in the picture's width).
std::shared_ptr<const st::Data> makeData(
    st::Mode mode, const std::vector<std::array<float, 8>>& entries, int64_t first = 0)
{
    std::vector<float> path;
    for (const auto& e : entries) {
        for (int i = 0; i < st::paramCount(mode); ++i)
            path.push_back(e[size_t(i)]);
    }
    return std::make_shared<const st::Data>(
        mode, Rational {25, 1}, first, double(kH) / kW, std::move(path));
}

} // namespace

class TestStabilizeRender : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    std::unique_ptr<Compositor> m_cpu;
    QTemporaryDir m_tmp;
    QString m_sequence;
    std::vector<CameraPose> m_poses;
    std::shared_ptr<const st::Data> m_data; ///< Translation measurement of the sequence

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
    std::shared_ptr<MediaPathMap> media() const
    {
        auto map = std::make_shared<MediaPathMap>();
        map->insert(QStringLiteral("v"), m_sequence);
        return map;
    }

    /// A timeline with the sequence as one clip (@p configure may change it).
    template<typename Fn>
    std::shared_ptr<const TimelineData> shot(Fn&& configure, int duration = kFrames)
    {
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData c = mediaClip(tl, QStringLiteral("v"), 0, duration, 0, kFrames * 40000);
        configure(c);
        addClip(tl, TrackType::Video, 0, c);
        return snap(tl);
    }
    static void stabilized(
        ClipData& c, std::shared_ptr<const st::Data> data, bool lock = true, bool zoom = false)
    {
        c.stabilization.settings.enabled = true;
        c.stabilization.settings.mode = data->mode();
        c.stabilization.settings.cameraLock = lock;
        c.stabilization.settings.zoom = zoom;
        c.stabilization.data = std::move(data);
    }

private slots:
    void initTestCase()
    {
        m_gpu = std::make_unique<Compositor>();
        Compositor::Options cpu;
        cpu.forceCpu = true;
        m_cpu = std::make_unique<Compositor>(cpu);

        ShakeSettings shake;
        shake.frames = kFrames;
        shake.panPerFrameX = 0.4;
        shake.jitterPx = 4.0;
        m_poses = shakyPath(shake);
        const QImage big = texturedPicture(900, 560, 5);
        std::vector<QImage> frames;
        for (const CameraPose& p : m_poses)
            frames.push_back(shakyFrame(big, kW, kH, p));
        m_sequence = writeSequence(m_tmp.filePath(QStringLiteral("seq")), frames);

        // Measure it the way the Stabilize button does.
        MediaItem item;
        item.id = QStringLiteral("v");
        item.path = m_sequence;
        item.info = media::probe(m_sequence);
        QVERIFY2(item.info.hasVideo, "the PNG sequence did not probe as video");
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = item.id;
        clip.duration = kFrames;
        QString why;
        const auto job
            = tracking::planStabilizeJob(clip, item, Rational {25, 1}, st::Mode::Translation, &why);
        QVERIFY2(job, qPrintable(why));
        QCOMPARE(job->request.firstFrame, int64_t(0));
        QCOMPARE(job->request.lastFrame, int64_t(kFrames - 1));
        const tracking::StabilizeOutcome out = tracking::runStabilizeJob(*job);
        QVERIFY2(out.data, qPrintable(out.error));
        QCOMPARE(out.lost, 0);
        m_data = out.data;
    }
    void cleanupTestCase()
    {
        m_gpu.reset();
        m_cpu.reset();
    }

    // The measured camera path is the true one (so the render below has something to align).
    void measuredPathMatchesTheTruth()
    {
        double worst = 0;
        for (int t = 0; t < kFrames; ++t) {
            worst = std::max(worst,
                std::abs(double(m_data->path()[size_t(2 * t)]) * kW - m_poses[size_t(t)].tx));
            worst = std::max(worst,
                std::abs(double(m_data->path()[size_t(2 * t + 1)]) * kW - m_poses[size_t(t)].ty));
        }
        QVERIFY2(worst < 0.3, qPrintable(QString::number(worst)));
    }

    // Translation mode with Camera Lock lines every frame up with the first one; without
    // stabilization the frames are far apart.
    void cameraLockAlignsTheFrames_data() { backendRows(); }
    void cameraLockAlignsTheFrames()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        c->setMediaPaths(media());
        const auto raw = shot([](ClipData&) {});
        const auto locked = shot([&](ClipData& cd) { stabilized(cd, m_data); });
        const QImage first = c->renderImage(raw, 0);
        for (const int t : {10, 17, 25, 31}) {
            const double before = innerDifference(first, c->renderImage(raw, t));
            const double after = innerDifference(first, c->renderImage(locked, t));
            QVERIFY2(
                before > 2.0, qPrintable(QString("frame %1: unstabilized %2").arg(t).arg(before)));
            QVERIFY2(after < 3.0 && after < before / 3.0,
                qPrintable(
                    QString("frame %1: stabilized %2, before %3").arg(t).arg(after).arg(before)));
        }
    }

    // Strength 0 is the unstabilized picture, bit for bit; so is a clip switched off, one that
    // was never analysed, and one whose Mode no longer matches its data.
    void strengthZeroAndInactiveStatesRenderAsBefore_data() { backendRows(); }
    void strengthZeroAndInactiveStatesRenderAsBefore()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        c->setMediaPaths(media());
        const auto raw = shot([](ClipData&) {});
        const auto zero = shot([&](ClipData& cd) {
            stabilized(cd, m_data);
            cd.stabilization.settings.strength = 0.0;
            cd.stabilization.settings.zoom = true;
        });
        const auto off = shot([&](ClipData& cd) {
            stabilized(cd, m_data);
            cd.stabilization.settings.enabled = false;
        });
        const auto neverAnalysed
            = shot([](ClipData& cd) { cd.stabilization.settings.enabled = true; });
        const auto staleMode = shot([&](ClipData& cd) {
            stabilized(cd, m_data);
            cd.stabilization.settings.mode = st::Mode::Perspective;
        });
        const auto full = shot([&](ClipData& cd) { stabilized(cd, m_data); });
        for (const int t : {0, 7, 19}) {
            const QImage expected = c->renderImage(raw, t);
            QVERIFY(c->renderImage(zero, t) == expected);
            QVERIFY(c->renderImage(off, t) == expected);
            QVERIFY(c->renderImage(neverAnalysed, t) == expected);
            QVERIFY(c->renderImage(staleMode, t) == expected);
        }
        QVERIFY(c->renderImage(full, 7) != c->renderImage(raw, 7)); // and it is not a no-op
    }

    // An old project (no stabilization key) renders as it did: the clip written and read again
    // without it gives the same pixels as the original.
    void oldProjectRendersTheSame_data() { backendRows(); }
    void oldProjectRendersTheSame()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        c->setMediaPaths(media());
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData clip = mediaClip(tl, QStringLiteral("v"), 0, kFrames, 0, kFrames * 40000);
        clip.transform.setBase(keys::Rotation, 3.0);
        QJsonObject json = serialization::clipToJson(clip);
        QVERIFY(!json.contains(QStringLiteral("stabilization")));
        ClipData loaded = serialization::clipFromJson(json);
        TimelineData tl2 = emptyTimeline(kW, kH);
        addClip(tl, TrackType::Video, 0, clip);
        addClip(tl2, TrackType::Video, 0, loaded);
        for (const int t : {0, 12})
            QVERIFY(c->renderImage(snap(tl), t) == c->renderImage(snap(tl2), t));
    }

    // Zoom on: no empty edge pixel in any frame; Zoom off: the frame size stays and the moving
    // empty edges show (the same picture as a locked render, scale 1).
    void zoomHidesTheEdges_data() { backendRows(); }
    void zoomHidesTheEdges()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        c->setMediaPaths(media());
        const auto withZoom = shot([&](ClipData& cd) { stabilized(cd, m_data, true, true); });
        const auto noZoom = shot([&](ClipData& cd) { stabilized(cd, m_data, true, false); });
        ClipData probe;
        stabilized(probe, m_data, true, true);
        const double zoom = st::zoomOf(probe);
        QVERIFY2(zoom > 1.03 && zoom < 1.2, qPrintable(QString::number(zoom)));
        int emptyWithoutZoom = 0;
        for (int t = 0; t < kFrames; ++t) {
            const QImage zoomed = c->renderImage(withZoom, t);
            QCOMPARE(zoomed.size(), QSize(kW, kH));
            QVERIFY2(blackPixels(zoomed) == 0,
                qPrintable(
                    QString("frame %1 has %2 empty pixels").arg(t).arg(blackPixels(zoomed))));
            const QImage plain = c->renderImage(noZoom, t);
            QCOMPARE(plain.size(), QSize(kW, kH));
            emptyWithoutZoom += blackPixels(plain);
        }
        QVERIFY2(emptyWithoutZoom > 2000, qPrintable(QString::number(emptyWithoutZoom)));
        // Zoom off keeps the scale: the picture at the first frame is the footage itself.
        QVERIFY(c->renderImage(noZoom, 0) == c->renderImage(shot([](ClipData&) {}), 0));
    }

    // Trim, speed and reverse keep the right source frames stabilized: the same source frame
    // gives the same pixels however the clip reaches it.
    void keysFollowTheSourceFrame_data() { backendRows(); }
    void keysFollowTheSourceFrame()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        c->setMediaPaths(media());
        const auto forward = shot([&](ClipData& cd) { stabilized(cd, m_data); });
        // Trimmed at the head by 5 frames.
        const auto trimmed = shot(
            [&](ClipData& cd) {
                stabilized(cd, m_data);
                cd.sourceInUs = 5 * 40000;
            },
            kFrames - 5);
        for (const int local : {0, 6, 20})
            QVERIFY2(c->renderImage(trimmed, local) == c->renderImage(forward, local + 5),
                qPrintable(QString("trim, local %1").arg(local)));
        // Double speed: local 4 shows source frame 8.
        const auto fast = shot(
            [&](ClipData& cd) {
                stabilized(cd, m_data);
                cd.speed = 2.0;
            },
            kFrames / 2);
        QVERIFY(c->renderImage(fast, 4) == c->renderImage(forward, 8));
        // Reverse from source frame 24: local 3 shows source frame 21.
        const auto reversed = shot(
            [&](ClipData& cd) {
                stabilized(cd, m_data);
                cd.speed = -1.0;
                cd.sourceInUs = 24 * 40000;
            },
            20);
        QVERIFY(c->renderImage(reversed, 3) == c->renderImage(forward, 21));
        QVERIFY(c->renderImage(reversed, 3) != c->renderImage(shot([](ClipData&) {}), 21));
    }

    // Exact geometry on a Solid: the picture moves by the correction, in the picture's units.
    void solidMovesByTheCorrection_data() { backendRows(); }
    void solidMovesByTheCorrection()
    {
        Compositor* c = backend();
        if (!c)
            QSKIP("no OpenGL");
        // The camera drifted right by 0.05 of the width (16 px) in source frame 1: undone, the
        // picture moves left by 16 px and an empty strip opens at the right edge.
        const auto data = makeData(st::Mode::Translation, {{0, 0}, {0.05f, 0}});
        const QColor colour(200, 60, 60);
        auto timeline = [&](bool zoom) {
            TimelineData tl = emptyTimeline(kW, kH);
            ClipData clip = solidClip(tl, colour, 0, 2);
            stabilized(clip, data, true, zoom);
            addClip(tl, TrackType::Video, 0, clip);
            return snap(tl);
        };
        const QImage plain = c->renderImage(timeline(false), 1);
        QVERIFY(near(px(plain, 100, 90), 200, 60, 60, 255, 3));
        QVERIFY(near(px(plain, 2, 90), 200, 60, 60, 255, 3));
        QVERIFY2(near(px(plain, 306, 90), 0, 0, 0, 0, 3), describe(px(plain, 306, 90)).constData());
        QVERIFY(near(px(plain, 300, 90), 200, 60, 60, 255, 3));
        // The first frame is untouched.
        QVERIFY(near(px(c->renderImage(timeline(false), 0), 318, 90), 200, 60, 60, 255, 3));
        // Cropping Ratio limits the zoom: a 48 px drift needs 1.43 times, 0.25 allows 1.25 times,
        // so the right edge (at 160 + 112 * 1.25 = 300) shows again.
        const auto far = makeData(st::Mode::Translation, {{0, 0}, {0.15f, 0}});
        TimelineData tl = emptyTimeline(kW, kH);
        ClipData clip = solidClip(tl, colour, 0, 2);
        stabilized(clip, far, true, true);
        clip.stabilization.settings.croppingRatio = 0.25;
        addClip(tl, TrackType::Video, 0, clip);
        const QImage limited = c->renderImage(snap(tl), 1);
        QVERIFY(near(px(limited, 290, 90), 200, 60, 60, 255, 6));
        QVERIFY2(
            near(px(limited, 310, 90), 0, 0, 0, 0, 3), describe(px(limited, 310, 90)).constData());
        // Zoom: 160 / (160 - 16) = 1.111; the whole frame is covered.
        const QImage zoomed = c->renderImage(timeline(true), 1);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const Px p = px(zoomed, x, y);
                QVERIFY2(near(p, 200, 60, 60, 255, 6),
                    qPrintable(QString("(%1,%2) %3").arg(x).arg(y).arg(describe(p).constData())));
            }
        }
    }

    // Perspective: a corrected picture in full perspective is the same on the GPU and the CPU.
    void perspectiveAgreesOnGpuAndCpu()
    {
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        // The camera tilted: a homography with a perspective part (stored entries h0..h7).
        const auto data = makeData(st::Mode::Perspective,
            {{1, 0, 0, 0, 1, 0, 0, 0}, {1.0f, 0.02f, 0.03f, -0.02f, 1.0f, 0.02f, 0.6f, -0.4f}});
        const auto lock = shot([&](ClipData& cd) { stabilized(cd, data); }, 2);
        const auto raw = shot([](ClipData&) {}, 2);
        m_gpu->setMediaPaths(media());
        m_cpu->setMediaPaths(media());
        const QImage gpu = m_gpu->renderImage(lock, 1);
        const QImage cpu = m_cpu->renderImage(lock, 1);
        QVERIFY2(innerDifference(gpu, cpu) < 4.0,
            qPrintable(QString::number(innerDifference(gpu, cpu))));
        QVERIFY(innerDifference(cpu, m_cpu->renderImage(raw, 1)) > 3.0); // it did warp
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestStabilizeRender>(argc, argv);
}
#include "test_stabilizerender.moc"
