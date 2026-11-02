// SPDX-License-Identifier: GPL-3.0-or-later
// Measurement: playback of a 3-layer 1080p60 timeline (software decoding) from the camera
// originals, from proxies and from the render cache. Prints fps / drops per mode; asserts only
// that frames were shown and that the cache was used. Slow (generates media, proxies and a
// cache), so it runs only with KK_PERF_PROXIES=1:
//   KK_PERF_PROXIES=1 build/test_proxyperf        (re-runs itself under xvfb-run for GL)
// Results are recorded in src/export/readme.md (Proxy media, optimized media, render cache).
#include "../render/RenderTestUtil.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "export/ProxyQueue.h"
#include "export/RenderCacheManager.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "render/ProxySelection.h"

#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int kSeconds = 4; ///< timeline length
constexpr int kFps = 60;
constexpr int kPlayMs = 3000; ///< measured play time per mode

} // namespace

class TestProxyPerf : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QString m_source;

    /// V1 full frame, V2 a 50 % picture-in-picture at 80 % opacity, V3 a 35 % PiP: the same
    /// 1080p60 file at three different offsets (three decoders).
    void buildTimeline(Project& project, const QString& id, int64_t durationUs)
    {
        project.beginEdit(QStringLiteral("perf"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 1920;
        tl.height = 1080;
        tl.fps = {kFps, 1};
        while (tl.videoTracks.size() < 3) {
            TrackData t;
            t.id = tl.allocateId();
            t.type = TrackType::Video;
            tl.videoTracks.push_back(t);
        }
        const int64_t frames = kSeconds * kFps;
        ClipData a = mediaClip(tl, id, 0, frames, 0, durationUs);
        ClipData b = mediaClip(tl, id, 0, frames, 1'000'000, durationUs);
        b.transform.setBase(keys::ZoomX, 0.5);
        b.transform.setBase(keys::ZoomY, 0.5);
        b.transform.setBase(keys::PosX, 400);
        b.transform.setBase(keys::Opacity, 80);
        ClipData c = mediaClip(tl, id, 0, frames, 2'000'000, durationUs);
        c.transform.setBase(keys::ZoomX, 0.35);
        c.transform.setBase(keys::ZoomY, 0.35);
        c.transform.setBase(keys::PosX, -500);
        c.transform.setBase(keys::PosY, -250);
        editops::overwriteClip(tl, {TrackType::Video, 0}, a);
        editops::overwriteClip(tl, {TrackType::Video, 1}, b);
        editops::overwriteClip(tl, {TrackType::Video, 2}, c);
        project.endEdit();
    }

    void measure(const char* label, Project& project, std::shared_ptr<const ProxyMap> proxies,
        std::shared_ptr<const RenderCacheView> cache)
    {
        project.timeline()->setPlayhead(0); // the previous engine wrote its position back
        PlaybackEngine::Options options;
        options.noAudio = true;
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        engine.setSubstitutes(std::move(proxies), nullptr);
        engine.setRenderCache(std::move(cache));
        int client = 0;
        engine.setOutputClient(&client, QSize(1000, 562), engine.compositor()->isGpu());
        engine.setQualityMode(PlaybackEngine::QualityMode::Full);
        QVERIFY(QTest::qWaitFor([&] { return engine.currentFrame() != nullptr; }, 20000));
        bool fromCache = false;
        connect(&engine, &PlaybackEngine::frameReady, this,
            [&fromCache](const RenderedFramePtr& f) { fromCache |= f && f->fromCache; });
        engine.play();
        QTest::qWait(300);
        engine.resetStats();
        QTest::qWait(kPlayMs);
        const PlaybackEngine::Stats s = engine.stats();
        engine.pause();
        qInfo("%-10s | %s | %.1f fps of %d, %lld presented, %lld dropped, render %.1f ms "
              "[decode %.1f, upload %.1f, composite %.1f, output %.1f]%s",
            label, qPrintable(engine.compositor()->backendDescription()), s.fps(), kFps,
            qlonglong(s.presented), qlonglong(s.dropped), s.averageRenderMs, s.averageDecodeMs,
            s.averageUploadMs, s.averageCompositeMs, s.averageOutputMs,
            fromCache ? " (from the render cache)" : "");
        QVERIFY(s.presented > 0);
        if (std::string_view(label) == "cache")
            QVERIFY(fromCache);
    }

private slots:
    void initTestCase()
    {
        if (qEnvironmentVariableIsEmpty("KK_PERF_PROXIES"))
            QSKIP("measurement; set KK_PERF_PROXIES=1 to run it");
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_proxyperf"));
        qputenv("KINOKLIP_HWACCEL", "off"); // software decoding, as on a laptop without VAAPI
        m_source = m_dir.filePath(QStringLiteral("rec_1080p60.mp4"));
        // A camera-like 1080p60 H.264 (High, CABAC, 2 s GOP with B-frames), 5 s.
        QProcess ff;
        ff.start(QStringLiteral("ffmpeg"),
            {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                QStringLiteral("-i"),
                QStringLiteral("testsrc2=size=1920x1080:rate=60,noise=alls=8:allf=t"),
                QStringLiteral("-t"), QStringLiteral("5"), QStringLiteral("-c:v"),
                QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("fast"),
                QStringLiteral("-g"), QStringLiteral("120"), QStringLiteral("-pix_fmt"),
                QStringLiteral("yuv420p"), m_source});
        QVERIFY(ff.waitForFinished(120000) && ff.exitCode() == 0);
    }

    void originalsProxiesCache()
    {
        Project project;
        const QString id = project.importMedia({m_source}).importedIds.value(0);
        QVERIFY(!id.isEmpty());
        const MediaItem item = *project.mediaPool()->item(id);
        buildTimeline(project, id, item.info.durationUs);

        measure("originals", project, nullptr, nullptr);

        // Proxies (defaults: Choose automatically = 960x540, H.264 fast decode).
        Compositor background;
        ProxyQueue queue(project.mediaLinks());
        queue.setCompositor(&background);
        ProxyJobSpec spec;
        QVERIFY(makeProxyJob(item, LinkKind::Proxy, project.cacheSettings(),
            m_dir.filePath(QStringLiteral("ProxyMedia")), &spec, nullptr));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(queue.enqueue(spec));
        QVERIFY(queue.waitForIdle(600'000));
        qInfo("proxy generation: %.1f s for %.1f s of 1080p60 (%s)", timer.elapsed() / 1000.0,
            item.info.durationUs / 1e6, qPrintable(spec.settings.video.codec));
        measure("proxies", project,
            makeProxyMap(project.mediaPool()->state(), *project.mediaLinks(),
                ProxyHandling::PreferProxies, true),
            nullptr);

        // Render cache (Smart: the whole timeline is a composite), from the originals.
        RenderCacheManager cache(&project, m_dir.filePath(QStringLiteral("RenderCache")));
        cache.setCompositor(&background);
        MediaCacheSettings s = project.cacheSettings();
        s.renderCacheMode = RenderCacheMode::Smart;
        s.backgroundCache = false;
        project.setCacheSettings(s);
        timer.restart();
        cache.renderAllNow();
        QVERIFY(cache.waitForIdle(1'200'000));
        qInfo("render cache: %.1f s to cache %d s of 1080p60 (DNxHR LB)", timer.elapsed() / 1000.0,
            kSeconds);
        measure("cache", project, nullptr, cache.view());
    }
};

int main(int argc, char** argv)
{
    return kk::test::glTestMain<TestProxyPerf>(argc, argv);
}

#include "test_proxyperf.moc"
