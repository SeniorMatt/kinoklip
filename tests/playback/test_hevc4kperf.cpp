// SPDX-License-Identifier: GPL-3.0-or-later
// Measurement: playback of one 4K HEVC clip (libx265, 3840x2160) on a 4K or 1080p timeline,
// from the original or from a proxy, per stage (decode, upload, composite, output). Slow, so it
// runs only with KK_PERF_HEVC=1. Settings (environment):
//   KK_HEVC_FPS=25|60  KK_HEVC_BITS=8|10  KK_HEVC_TL=3840x2160|1920x1080  KK_HEVC_PROXY=0|1
//   KK_HEVC_DIR=<folder for the generated clip>  KK_HEVC_HW=1 (allow hardware decoding)
//   KK_HEVC_BGJOB=1  generate the clip's proxy in the background while it plays (the editor's
//                    proxy / render cache queues: does a background job disturb playback?)
// Run it under taskset to resemble a laptop.
#include "../render/RenderTestUtil.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "export/ProxyQueue.h"
#include "media/VideoDecoder.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "render/ProxySelection.h"

#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

using namespace kk;
using namespace kk::test;

class TestHevc4kPerf : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIsEmpty("KK_PERF_HEVC"))
            QSKIP("measurement; set KK_PERF_HEVC=1 to run it");
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_hevc4kperf"));
        if (qEnvironmentVariableIsEmpty("KK_HEVC_HW"))
            qputenv("KINOKLIP_HWACCEL", "off");
    }

    void play()
    {
        const int fps = qEnvironmentVariableIntValue("KK_HEVC_FPS")
            ? qEnvironmentVariableIntValue("KK_HEVC_FPS")
            : 25;
        const int bits = qEnvironmentVariableIntValue("KK_HEVC_BITS") == 10 ? 10 : 8;
        const bool proxy = qEnvironmentVariableIntValue("KK_HEVC_PROXY") != 0;
        const QStringList tl = qEnvironmentVariable("KK_HEVC_TL", "3840x2160").split('x');
        const int tw = tl.value(0).toInt(), th = tl.value(1).toInt();
        const QString dir = qEnvironmentVariable("KK_HEVC_DIR", QDir::tempPath());
        QDir().mkpath(dir);
        const QString src = QStringLiteral("%1/hevc4k_%2fps_%3bit.mp4").arg(dir).arg(fps).arg(bits);
        if (!QFile::exists(src)) {
            QProcess ff;
            ff.start(QStringLiteral("ffmpeg"),
                {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                    QStringLiteral("testsrc2=size=3840x2160:rate=%1,noise=alls=6:allf=t").arg(fps),
                    "-t", "6", "-c:v", "libx265", "-preset", "veryfast", "-crf", "24", "-g",
                    QString::number(fps * 2), "-x265-params", "log-level=error:pools=2", "-pix_fmt",
                    bits == 10 ? "yuv420p10le" : "yuv420p", "-tag:v", "hvc1", src + ".tmp.mp4"});
            QVERIFY(ff.waitForFinished(1800000) && ff.exitCode() == 0);
            QVERIFY(QFile::rename(src + ".tmp.mp4", src));
        }
        {
            // The decoder alone, frame after frame (what the render thread does per frame).
            media::VideoDecoder decoder;
            QVERIFY(decoder.open(src));
            QElapsedTimer timer;
            timer.start();
            const int64_t frameUs = 1'000'000 / fps;
            for (int k = 0; k < 150; ++k)
                QVERIFY(decoder.decodeFrame(k * frameUs + frameUs / 2));
            qInfo("RAW decodeFrame %dfps %dbit: %.1f fps (%s)", fps, bits,
                150000.0 / timer.elapsed(), qPrintable(hwAccelName(decoder.activeHwAccel())));
        }
        Project project;
        const QString id = project.importMedia({src}).importedIds.value(0);
        QVERIFY(!id.isEmpty());
        const MediaItem item = *project.mediaPool()->item(id);
        project.beginEdit(QStringLiteral("perf"));
        TimelineData& t = project.timeline()->mutableData();
        t.width = tw;
        t.height = th;
        t.fps = {fps, 1};
        ClipData a = mediaClip(t, id, 0, 5 * fps, 0, item.info.durationUs);
        editops::overwriteClip(t, {TrackType::Video, 0}, a);
        project.endEdit();

        std::shared_ptr<const ProxyMap> proxies;
        QTemporaryDir tmp;
        Compositor background(Compositor::Options {.background = true});
        ProxyQueue queue(project.mediaLinks());
        if (proxy) {
            queue.setCompositor(&background);
            ProxyJobSpec spec;
            QVERIFY(makeProxyJob(item, LinkKind::Proxy, project.cacheSettings(),
                tmp.filePath(QStringLiteral("ProxyMedia")), &spec, nullptr));
            QVERIFY(queue.enqueue(spec));
            QVERIFY(queue.waitForIdle(1200000));
            proxies = makeProxyMap(project.mediaPool()->state(), *project.mediaLinks(),
                ProxyHandling::PreferProxies, true);
        }

        if (qEnvironmentVariableIntValue("KK_HEVC_BGJOB") != 0 && !proxy) {
            queue.setCompositor(&background);
            ProxyJobSpec spec;
            QVERIFY(makeProxyJob(item, LinkKind::Proxy, project.cacheSettings(),
                tmp.filePath(QStringLiteral("ProxyMedia")), &spec, nullptr));
            QVERIFY(queue.enqueue(spec));
        }
        PlaybackEngine::Options options;
        options.noAudio = true;
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        engine.setSubstitutes(std::move(proxies), nullptr);
        int client = 0;
        engine.setOutputClient(&client, QSize(1000, 562), engine.compositor()->isGpu());
        engine.setQualityMode(PlaybackEngine::QualityMode::Full);
        QVERIFY(QTest::qWaitFor([&] { return engine.currentFrame() != nullptr; }, 30000));
        engine.play();
        QTest::qWait(500);
        engine.resetStats();
        QTest::qWait(3500);
        const PlaybackEngine::Stats s = engine.stats();
        engine.pause();
        qInfo(
            "RESULT %dfps %dbit tl=%dx%d proxy=%d | %s | %.1f fps of %d, %lld presented, "
            "%lld dropped, render %.1f ms [decode %.1f, upload %.1f, composite %.1f, output %.1f]",
            fps, bits, tw, th, int(proxy), qPrintable(engine.compositor()->backendDescription()),
            s.fps(), fps, qlonglong(s.presented), qlonglong(s.dropped), s.averageRenderMs,
            s.averageDecodeMs, s.averageUploadMs, s.averageCompositeMs, s.averageOutputMs);
        QVERIFY(s.presented > 0);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestHevc4kPerf>(argc, argv);
}

#include "test_hevc4kperf.moc"
