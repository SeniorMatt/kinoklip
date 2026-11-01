// SPDX-License-Identifier: GPL-3.0-or-later
// Compositor benchmark at 1080p25 (prints ms/frame; asserts only that frames render):
//   1 video layer | 2 layers + gaussian blur | 3 layers + title,
// each as viewer path (texture only), viewer at Half quality, and export path (QImage readback,
// 3 requests in flight), plus the CPU fallback for comparison.
// Run on the real GPU with e.g. `QT_QPA_PLATFORM=xcb KK_NO_XVFB=1 DISPLAY=:0
// build/test_renderperf`; under ctest it runs under xvfb (llvmpipe, i.e. software OpenGL — numbers
// are not GPU numbers).
#include "RenderTestUtil.h"
#include "render/Compositor.h"

#include <QElapsedTimer>

#include <deque>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int kFrames = 100; ///< frames per measurement on a GPU (4 s of the 8 s fixture)
/// Frames per measurement on software OpenGL (llvmpipe under xvfb: ~10x slower; keeps the
/// ctest run well inside its timeout).
constexpr int kSoftwareFrames = 12;
const QString kVideo = QStringLiteral("perf");

/// The benchmark timelines (n = 1, 2, 3 layers).
TimelineData scenario(int layers)
{
    TimelineData tl = emptyTimeline(1920, 1080);
    tl.videoTracks.resize(3);
    for (TrackData& t : tl.videoTracks) {
        if (!t.id)
            t.id = tl.allocateId();
    }
    addClip(tl, TrackType::Video, 0, mediaClip(tl, kVideo, 0, 200, 0, 8'000'000));
    if (layers >= 2) {
        // Second layer: the same file 2 s later, as a picture-in-picture with a blurred base.
        ClipData pip = mediaClip(tl, kVideo, 0, 200, 2'000'000, 8'000'000);
        pip.transform.setBase(keys::ZoomX, 0.45);
        pip.transform.setBase(keys::ZoomY, 0.45);
        pip.transform.setBase(keys::PosX, 450);
        pip.transform.setBase(keys::PosY, 250);
        pip.transform.setBase(keys::Rotation, 3);
        addClip(tl, TrackType::Video, 1, pip);
        EffectInstance blur;
        blur.id = tl.allocateId();
        blur.type = QStringLiteral("gaussian_blur");
        blur.params.setBase(QStringLiteral("radius"), 12);
        tl.videoTracks[0].clips[0].effects.push_back(blur);
    }
    if (layers >= 3) {
        ClipData title = titleClip(tl, QStringLiteral("Kinoklip benchmark"), 0, 200, 96);
        title.title.setBase(keys::OutlineWidth, 4);
        title.title.setStatic(keys::Shadow, true);
        title.transform.setBase(keys::PosY, -380);
        addClip(tl, TrackType::Video, 2, title);
    }
    return tl;
}

} // namespace

class TestRenderPerf : public QObject
{
    Q_OBJECT

    std::unique_ptr<Compositor> m_gpu;
    QString m_skip;
    int m_frames = kFrames;

    double measure(Compositor& c, const std::shared_ptr<const TimelineData>& tl, unsigned outputs,
        Quality quality, int inFlight)
    {
        // Warm up (decoder open, shader first use, pools).
        (void)c.render(tl, 0, {}, quality, outputs);
        QElapsedTimer timer;
        timer.start();
        std::deque<std::future<RenderedFramePtr>> queue;
        int done = 0;
        for (int f = 1; f <= m_frames; ++f) {
            RenderRequest rq;
            rq.timeline = tl;
            rq.frame = f;
            rq.quality = quality;
            rq.outputs = outputs;
            queue.push_back(c.renderAsync(rq));
            while (int(queue.size()) >= inFlight) {
                const RenderedFramePtr r = queue.front().get();
                queue.pop_front();
                if (!r->ok || ((outputs & OutputImage) && r->image.isNull()))
                    return -1;
                ++done;
            }
        }
        while (!queue.empty()) {
            queue.front().get();
            queue.pop_front();
            ++done;
        }
        return timer.nsecsElapsed() / 1e6 / done;
    }

private slots:
    void initTestCase()
    {
        if (!ensureFixtures(&m_skip))
            return;
        m_gpu = std::make_unique<Compositor>();
        auto paths = std::make_shared<MediaPathMap>();
        paths->insert(kVideo, fixture("perf_1080p.mp4"));
        m_gpu->setMediaPaths(paths);
        const QString backend = m_gpu->backendDescription();
        if (backend.contains(QLatin1String("llvmpipe"))
            || backend.contains(QLatin1String("softpipe")))
            m_frames = kSoftwareFrames;
        qInfo("backend: %s, %d frames per measurement", qPrintable(backend), m_frames);
    }

    void benchmark_data()
    {
        QTest::addColumn<int>("layers");
        QTest::newRow("1 video layer") << 1;
        QTest::newRow("2 layers + blur") << 2;
        QTest::newRow("3 layers + title") << 3;
    }
    void benchmark()
    {
        QFETCH(int, layers);
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        if (!m_gpu->isGpu())
            QSKIP("no OpenGL");
        const auto tl = snap(scenario(layers));
        const double viewer = measure(*m_gpu, tl, OutputTexture, Quality::Full, 1);
        const double half = measure(*m_gpu, tl, OutputTexture, Quality::Half, 1);
        const double exportMs = measure(*m_gpu, tl, OutputImage, Quality::Full, 3);
        const double exportSync = measure(*m_gpu, tl, OutputImage, Quality::Full, 1);
        QVERIFY(viewer > 0 && half > 0 && exportMs > 0 && exportSync > 0);
        qInfo("1080p %-17s texture %6.2f ms | half %6.2f ms | readback x3 in flight %6.2f ms | "
              "readback sync %6.2f ms",
            QTest::currentDataTag(), viewer, half, exportMs, exportSync);
    }

    void cpuFallback()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        Compositor::Options options;
        options.forceCpu = true;
        Compositor cpu(options);
        auto paths = std::make_shared<MediaPathMap>();
        paths->insert(kVideo, fixture("perf_1080p.mp4"));
        cpu.setMediaPaths(paths);
        const auto tl = snap(scenario(1));
        const double ms = measure(cpu, tl, OutputImage, Quality::Full, 1);
        QVERIFY(ms > 0);
        qInfo("1080p CPU fallback, 1 video layer: %.2f ms/frame", ms);
    }

    void cleanupTestCase() { m_gpu.reset(); }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestRenderPerf>(argc, argv);
}

#include "test_renderperf.moc"
