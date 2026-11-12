// SPDX-License-Identifier: GPL-3.0-or-later
// Memory of a render and after it (spec 85.5-85.7). On an integrated GPU the decoders' frame
// pools, render targets and textures are system memory that no process meter shows, so a long
// edit of many clips used to leave gigabytes behind: every clip's decoder stayed open (10 s of
// wall time is hundreds of timeline frames in a fast render) and, once the render was over,
// the render thread never looked at its decoders and targets again.
//
// A project of many clips (each a different file, as a real edit has), titles, a keyframed zoom,
// cross dissolves and a time effect is rendered like ExportJob does (3 requests in flight, image
// output). The decoders open at once stay under a bound while it renders, and a while after the
// last frame the render thread holds nothing: no decoder, no title texture, no pooled target.
// Set KK_RENDERMEM_LOG=1 to print the numbers (and the process memory) every 12 frames.
#include "RenderTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/VideoEffectKeys.h"
#include "render/Compositor.h"
#include "render/DecoderPool.h"
#include "render/GlKit.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include <deque>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int kClips = 14; ///< files in the edit (more than DecoderPool::kMaxOpen)
constexpr int kClipFrames = 12;
constexpr int kInFlight = 3;

/// "VmRSS" / "RssShmem" of this process in MiB (0 where /proc is not there).
double statusMiB(const char* key)
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    for (const QByteArray& line : f.readAll().split('\n')) {
        if (line.startsWith(key))
            return line.mid(int(qstrlen(key)) + 1).simplified().split(' ').value(0).toDouble()
                / 1024.0;
    }
    return 0;
}

TimelineData editTimeline()
{
    TimelineData tl = emptyTimeline(1280, 720);
    for (int i = 0; i < kClips; ++i) {
        ClipData clip = mediaClip(tl, QStringLiteral("m%1").arg(i), int64_t(i) * kClipFrames,
            kClipFrames, int64_t(i % 5) * 1'000'000, 5'000'000);
        // Keyframed zoom, as in the report (a new target size on every frame before spec 85.1).
        clip.transform.values[keys::ZoomX].setKey(0, 1.0);
        clip.transform.values[keys::ZoomX].setKey(kClipFrames, 1.6);
        clip.transform.values[keys::ZoomY].setKey(0, 1.0);
        clip.transform.values[keys::ZoomY].setKey(kClipFrames, 1.6);
        if (i > 0) {
            clip.transitionIn
                = EffectRegistry::instance().createTransition(QStringLiteral("cross_dissolve"), 6);
        }
        if (i % 3 == 0) {
            clip.effects.append(EffectRegistry::instance().createEffect(
                QLatin1String(fxid::Echo), tl.allocateId()));
        }
        addClip(tl, TrackType::Video, 0, clip);
        ClipData title = titleClip(
            tl, QStringLiteral("Clip %1").arg(i), int64_t(i) * kClipFrames, kClipFrames, 64);
        addClip(tl, TrackType::Video, 1, title);
    }
    return tl;
}

} // namespace

class TestRenderMemory : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_skip;

private slots:
    void initTestCase()
    {
        if (!ensureFixtures(&m_skip))
            return;
        QVERIFY(m_dir.isValid());
    }

    void rendersWithBoundedMemoryAndReleasesIt()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        Compositor::Options options;
        options.idleReleaseMs = 400;
        Compositor compositor(options);
        if (!compositor.isGpu())
            QSKIP("no OpenGL");

        // Every clip is its own file (links to one fixture), as the clips of a real edit are.
        auto paths = std::make_shared<MediaPathMap>();
        for (int i = 0; i < kClips; ++i) {
            const QString link = m_dir.filePath(QStringLiteral("clip%1.mp4").arg(i));
            QVERIFY(QFile::link(fixture("h264_25fps.mp4"), link));
            paths->insert(QStringLiteral("m%1").arg(i), link);
        }
        const bool log = qEnvironmentVariableIsSet("KK_RENDERMEM_LOG");
        const auto timeline = snap(editTimeline());
        const int total = kClips * kClipFrames;
        const QSize size(640, 360);

        int peakDecoders = 0;
        int64_t peakBytes = 0;
        std::deque<std::future<RenderedFramePtr>> inFlight;
        int requested = 0;
        for (int done = 0; done < total; ++done) {
            while (int(inFlight.size()) < kInFlight && requested < total) {
                RenderRequest rq {timeline, paths, requested++, size, Quality::Full, OutputImage};
                inFlight.push_back(compositor.renderAsync(std::move(rq)));
            }
            const RenderedFramePtr frame = inFlight.front().get();
            inFlight.pop_front();
            QVERIFY2(
                frame && frame->ok && !frame->image.isNull(), qPrintable(QString::number(done)));
            const render::ResourceStats s = compositor.resourceStats();
            peakDecoders = std::max(peakDecoders, s.decoders);
            peakBytes = std::max(peakBytes, s.targetBytes);
            if (log && done % 12 == 0) {
                qInfo("frame %3d: decoders %2d, titles %2d, targets %6.1f MiB, process %6.1f MiB "
                      "(shmem %.1f)",
                    done, s.decoders, s.titleTextures, double(s.targetBytes) / (1 << 20),
                    statusMiB("VmRSS:"), statusMiB("RssShmem:"));
            }
        }
        qInfo(
            "peak: %d decoders, %.1f MiB of targets", peakDecoders, double(peakBytes) / (1 << 20));
        // Open decoders are bounded (in use + reserved by the look-ahead on top of the limit).
        QVERIFY2(peakDecoders <= 11, // 8 open + 3 reserved by the look-ahead
            qPrintable(QString::number(peakDecoders)));
        QVERIFY(peakBytes <= render::TargetPool::processByteLimit());

        // The render is over: the render thread lets go of everything by itself.
        QElapsedTimer timer;
        timer.start();
        render::ResourceStats s = compositor.resourceStats();
        while ((s.decoders || s.titleTextures || s.targetBytes) && timer.elapsed() < 10'000) {
            QThread::msleep(100);
            s = compositor.resourceStats();
        }
        if (log) {
            qInfo("after: decoders %d, titles %d, targets %.1f MiB, process %.1f MiB", s.decoders,
                s.titleTextures, double(s.targetBytes) / (1 << 20), statusMiB("VmRSS:"));
        }
        QCOMPARE(s.decoders, 0);
        QCOMPARE(s.titleTextures, 0);
        QCOMPARE(s.targetBytes, 0);
        QCOMPARE(render::TargetPool::processBytes(), 0);

        // ... and the next frame still renders (everything is made again on demand).
        const RenderedFramePtr again
            = compositor.render(timeline, 5, size, Quality::Full, OutputImage);
        QVERIFY(again && again->ok && !again->image.isNull());
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestRenderMemory>(argc, argv);
}

#include "test_rendermemory.moc"
