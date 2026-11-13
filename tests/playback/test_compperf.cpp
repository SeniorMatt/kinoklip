// SPDX-License-Identifier: GPL-3.0-or-later
// Measurement (issue #7): a long-GOP 4K HEVC 100 fps clip on a 24 fps timeline with a second clip
// above it (V2), played three ways, frames really shown per second counted at the viewer:
//   edit      the program viewer (the Edit page)
//   comp-old  what the Compositing viewer did before: the engine plays the timeline and the
//             viewer asks for its own node render after every frame, one at a time
//   comp-new  the engine renders the Compositing viewer's node itself (setCompositionView())
// Printed, never failing on speed. Slow (it makes the clip), so only with KK_PERF_COMP=1.
//   KK_COMP_SECONDS=<clip length, default 6>  KK_COMP_DIR=<folder of the generated clip>
//   KK_COMP_HW=1 allow hardware decoding  KK_COMP_NODES=0|1 a stored graph with a Blur node
#include "../render/RenderTestUtil.h"
#include "core/CompOps.h"
#include "core/EditOps.h"
#include "core/NodeRegistry.h"
#include "core/Project.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"

#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QSettings>
#include <QTimer>

using namespace kk;
using namespace kk::test;

class TestCompPerf : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIsEmpty("KK_PERF_COMP"))
            QSKIP("measurement; set KK_PERF_COMP=1 to run it");
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_compperf"));
        if (qEnvironmentVariableIsEmpty("KK_COMP_HW"))
            qputenv("KINOKLIP_HWACCEL", "off");
    }

    void measure()
    {
        const int seconds = std::max(2, qEnvironmentVariableIntValue("KK_COMP_SECONDS") ?: 6);
        const QString dir = qEnvironmentVariable("KK_COMP_DIR", QDir::tempPath());
        QDir().mkpath(dir);
        const QString src = QStringLiteral("%1/comp_4k_100fps_%2s.mp4").arg(dir).arg(seconds);
        if (!QFile::exists(src)) {
            QProcess ff;
            ff.start(QStringLiteral("ffmpeg"),
                {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                    "testsrc2=size=3840x2160:rate=100,noise=alls=6:allf=t", "-t",
                    QString::number(seconds), "-c:v", "libx265", "-preset", "veryfast", "-crf",
                    "26", "-g", "200", "-x265-params", "log-level=error:pools=2", "-pix_fmt",
                    "yuv420p", "-tag:v", "hvc1", src + ".tmp.mp4"});
            QVERIFY(ff.waitForFinished(3600000) && ff.exitCode() == 0);
            QVERIFY(QFile::rename(src + ".tmp.mp4", src));
        }
        Project project;
        const QString id = project.importMedia({src}).importedIds.value(0);
        QVERIFY(!id.isEmpty());
        const MediaItem item = *project.mediaPool()->item(id);
        quint64 clipId = 0;
        project.beginEdit(QStringLiteral("perf"));
        TimelineData& t = project.timeline()->mutableData();
        t.width = 1920;
        t.height = 1080;
        t.fps = {24, 1};
        ClipData a = mediaClip(t, id, 0, int64_t(seconds) * 24, 0, item.info.durationUs);
        clipId = editops::overwriteClip(t, {TrackType::Video, 0}, a);
        ClipData b = mediaClip(t, id, 0, int64_t(seconds) * 24, 0, item.info.durationUs);
        editops::overwriteClip(t, {TrackType::Video, 1}, b);
        if (qEnvironmentVariableIntValue("KK_COMP_NODES") != 0) {
            Composition c = compops::defaultComposition();
            editops::setClipComposition(t, clipId, c);
        }
        project.endEdit();

        PlaybackEngine::Options options;
        options.noAudio = true;
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        int client = 0;
        engine.setOutputClient(&client, QSize(1000, 562), engine.compositor()->isGpu());
        engine.setQualityMode(PlaybackEngine::QualityMode::Full);
        QVERIFY(QTest::qWaitFor([&] { return engine.currentFrame() != nullptr; }, 60000));
        const bool gpu = engine.compositor()->isGpu();
        const unsigned outputs = gpu ? OutputTexture : OutputImage;

        auto run = [&](const char* name, bool oldViewer) {
            int64_t shown = 0;
            bool inFlight = false;
            std::future<RenderedFramePtr> fut;
            QTimer poll;
            auto request = [&] {
                if (inFlight)
                    return;
                const ClipData* c = editops::clipById(project.timeline()->data(), clipId);
                inFlight = true;
                fut = engine.renderCompositionNode(project.timeline()->snapshot(), clipId, 0,
                    std::max<int64_t>(0, project.timeline()->playhead() - c->start),
                    QSize(1000, 562), outputs);
            };
            QObject::connect(&poll, &QTimer::timeout, [&] {
                if (inFlight && fut.valid()
                    && fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    inFlight = false;
                    const RenderedFramePtr f = fut.get();
                    if (f && f->ok)
                        ++shown;
                    request();
                }
            });
            QMetaObject::Connection conn;
            if (oldViewer) {
                poll.start(1);
                conn = QObject::connect(&engine, &PlaybackEngine::frameReady, [&] { request(); });
            } else {
                conn = QObject::connect(&engine, &PlaybackEngine::frameReady,
                    [&](const RenderedFramePtr& f) {
                        if (f && f->ok)
                            ++shown;
                    });
            }
            project.timeline()->setPlayhead(0);
            engine.seek(0);
            QTest::qWait(300);
            engine.play();
            QTest::qWait(500);
            engine.resetStats();
            shown = 0;
            QElapsedTimer timer;
            timer.start();
            QTest::qWait(3000);
            const double secs = timer.elapsed() / 1000.0;
            const PlaybackEngine::Stats s = engine.stats();
            engine.pause();
            QObject::disconnect(conn);
            poll.stop();
            qInfo("RESULT %-8s | %.1f frames/s shown at the viewer (engine: %.1f fps of 24, "
                  "%lld dropped, render %.1f ms [decode %.1f]) %s",
                name, double(shown) / secs, s.fps(), qlonglong(s.dropped), s.averageRenderMs,
                s.averageDecodeMs, qPrintable(engine.compositor()->backendDescription()));
        };
        // KK_COMP_ORDER=reverse runs the new Compositing path first (the machine's load drifts).
        const bool reverse = qEnvironmentVariable("KK_COMP_ORDER") == QLatin1String("reverse");
        auto compNew = [&] {
            engine.setCompositionView(&client, 0, clipId, 0);
            QTest::qWait(500);
            run("comp-new", false);
            engine.clearCompositionView(&client);
            QTest::qWait(500);
        };
        if (reverse)
            compNew();
        run("edit", false);
        if (!reverse)
            compNew();
        run("comp-old", true);
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestCompPerf>(argc, argv);
}

#include "test_compperf.moc"
