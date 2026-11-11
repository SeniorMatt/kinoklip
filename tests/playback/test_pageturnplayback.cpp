// SPDX-License-Identifier: GPL-3.0-or-later
// Playback while the timeline view page-turns (GitHub issue 2): a long, high-frame-rate, long-GOP
// clip is played by the real engine (real Compositor, fake sound card as master clock) under a
// real TimelineWidget zoomed so the playhead leaves the view during playback. The widget draws
// filmstrip thumbnails and waveforms from the real MediaCache, so a page turn asks the cache for
// the tiles of the newly shown page exactly as in the application.
//
// Per case it prints the frames shown per second, the dropped frames, the longest hold between
// two frames and the longest stall of the GUI thread. The assertion test compares relative
// behaviour (the view following the playhead against the same playback with the view fixed), so
// it holds on a CPU-only machine whatever its speed.
//
// The process is limited to three CPUs first (Linux): the question is what the thumbnails of the
// new page take from a decoder that has little headroom, as on a laptop with a 4K clip.
//
// KK_PAGETURN_MEDIA=file   use this clip (default: generated 1920x1080, 100 fps, GOP 600, 90 s)
// KK_PAGETURN_PAGE=8       seconds of timeline shown per page
// KK_PAGETURN_SECONDS=8    seconds played per case
// KK_PAGETURN_REPORT=1     run the full table of cases in reportTable()
#include "../media/MediaTestUtil.h"
#include "../timeline/TimelineTestUtils.h"
#include "PlaybackTestUtil.h"
#include "core/MediaPool.h"
#include "media/MediaCache.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <memory>

#ifdef Q_OS_LINUX
#include <sched.h>
#endif
#include <vector>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;

namespace {

constexpr int kFps = 100;

struct Case
{
    ClipThumbnails thumbs = ClipThumbnails::Filmstrip;
    bool waveforms = true;
    double speed = 1.0;
    bool follow = true; ///< the view page-turns with the playhead
};

struct Result
{
    int64_t presented = 0;
    int64_t dropped = 0;
    double seconds = 0.0;
    double longestHoldMs = 0.0;
    double longestGuiStallMs = 0.0;
    int thumbnailsDuringPlay = 0; ///< tiles the cache delivered while playing
    std::vector<int> perSecond;
    [[nodiscard]] double fps() const { return seconds > 0 ? presented / seconds : 0.0; }
};

QString describe(const Case& c)
{
    const char* thumbs = c.thumbs == ClipThumbnails::Filmstrip ? "filmstrip"
        : c.thumbs == ClipThumbnails::HeadTail                 ? "headtail "
                                                               : "nameonly ";
    return QStringLiteral("%1 wave=%2 speed=%3%% %4")
        .arg(QLatin1String(thumbs))
        .arg(c.waveforms ? "on " : "off")
        .arg(int(c.speed * 100))
        .arg(c.follow ? "page-turns" : "view fixed");
}

} // namespace

class TestPageTurnPlayback : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        limitCpus(3);
        initTimelineTestCase("test_pageturnplayback");
        m_path = qEnvironmentVariable("KK_PAGETURN_MEDIA");
        if (m_path.isEmpty()) {
            if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
                QSKIP("no ffmpeg");
            m_path = m_temp.path() + QStringLiteral("/pageturn.mp4");
            QProcess ffmpeg;
            ffmpeg.setProcessChannelMode(QProcess::ForwardedChannels);
            ffmpeg.start(QStringLiteral("ffmpeg"),
                {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                    "testsrc2=s=1920x1080:r=100:d=90", "-f", "lavfi", "-i",
                    "sine=frequency=440:d=90", "-c:v", "libx264", "-preset", "ultrafast", "-crf",
                    "26", "-g", "600", "-keyint_min", "600", "-sc_threshold", "0", "-pix_fmt",
                    "yuv420p", "-c:a", "aac", m_path});
            QVERIFY(ffmpeg.waitForFinished(5 * 60 * 1000) && ffmpeg.exitCode() == 0);
        }
        m_page = qEnvironmentVariable("KK_PAGETURN_PAGE", QStringLiteral("8")).toDouble();
        m_playSeconds = qEnvironmentVariable("KK_PAGETURN_SECONDS", QStringLiteral("8")).toDouble();
        m_compositor = std::make_shared<Compositor>();
    }

    void cleanupTestCase() { media::MediaCache::instance().shutdown(); }

    /// The assertion: a clip at 300 % with the filmstrip and waveform shown, the same playback
    /// with the view following the playhead (the new page's tiles are asked for while playing)
    /// must show nearly as many frames as with the view fixed. Before the fix the page turn
    /// cost a quarter of the frames (exact long-GOP seeks of the trickled tiles next to the
    /// playback decoder).
    void pageTurnDoesNotHurtPlayback()
    {
        const Result fixed = run({ClipThumbnails::Filmstrip, true, 3.0, false});
        const Result turning = run({ClipThumbnails::Filmstrip, true, 3.0, true});
        QVERIFY2(fixed.presented > 300, "the reference playback is too slow on this machine");
        QVERIFY2(turning.thumbnailsDuringPlay > 0, "the page turn asked for no tiles");
        // Two runs of the same fixed playback differ by about 1 %; the unfixed page turn lost
        // 6 - 25 % and 60 - 180 more frames.
        QVERIFY2(
            turning.presented >= fixed.presented * 0.97 && turning.dropped <= fixed.dropped + 30,
            qPrintable(QStringLiteral("page-turning view showed %1 frames (%2 dropped), fixed view "
                                      "%3 (%4 dropped)")
                           .arg(turning.presented)
                           .arg(turning.dropped)
                           .arg(fixed.presented)
                           .arg(fixed.dropped)));
    }

    void reportTable()
    {
        if (qEnvironmentVariableIsEmpty("KK_PAGETURN_REPORT"))
            QSKIP("set KK_PAGETURN_REPORT=1 for the table");
        for (const double speed : {1.0, 3.0}) {
            run({ClipThumbnails::Filmstrip, true, speed, false});
            run({ClipThumbnails::Filmstrip, true, speed, true});
            run({ClipThumbnails::None, true, speed, true});
            run({ClipThumbnails::Filmstrip, false, speed, true});
            run({ClipThumbnails::None, false, speed, true});
        }
    }

private:
    /// Restricts the process (and the threads it starts later) to @p count of its CPUs.
    static void limitCpus(int count)
    {
#ifdef Q_OS_LINUX
        cpu_set_t all;
        CPU_ZERO(&all);
        if (sched_getaffinity(0, sizeof(all), &all) != 0 || CPU_COUNT(&all) <= count)
            return;
        cpu_set_t some;
        CPU_ZERO(&some);
        for (int cpu = 0, taken = 0; cpu < CPU_SETSIZE && taken < count; ++cpu) {
            if (CPU_ISSET(cpu, &all)) {
                CPU_SET(cpu, &some);
                ++taken;
            }
        }
        sched_setaffinity(0, sizeof(some), &some);
#else
        Q_UNUSED(count);
#endif
    }

    Result run(const Case& c)
    {
        TimelineFixture f;
        const ImportResult imported = f.project.importMedia({m_path});
        if (imported.importedIds.size() != 1) {
            QTest::qFail("import failed", __FILE__, __LINE__);
            return {};
        }
        const media::MediaInfo info = f.project.mediaPool()->item(imported.importedIds[0])->info;
        f.project.beginEdit(QStringLiteral("clip"));
        TimelineData& tl = f.project.timeline()->mutableData();
        tl = makeDefaultTimeline(
            QStringLiteral("t"), {kFps, 1}, info.displayWidth, info.displayHeight, 1, 1);
        ClipData clip;
        clip.id = tl.allocateId();
        clip.mediaId = imported.importedIds[0];
        clip.sourceDurationUs = info.durationUs;
        clip.speed = c.speed;
        clip.duration = std::max<int64_t>(
            1, int64_t(double(usToFrames(info.durationUs, Rational {kFps, 1})) / c.speed));
        clip.linkGroup = 1;
        tl.videoTracks[0].clips.push_back(clip);
        if (info.hasAudio) {
            clip.id = tl.allocateId();
            tl.audioTracks[0].clips.push_back(clip);
        }
        f.project.endEdit();

        // Every case starts with a cold cache, as a new page does in the application.
        media::MediaCache& cache = media::MediaCache::instance();
        cache.setPlaybackActive(false);
        m_cacheDirs.push_back(std::make_unique<QTemporaryDir>());
        cache.setCacheDirectory(m_cacheDirs.back()->path());
        cache.clearMemoryCache();

        PlaybackEngine::Options options;
        options.compositor = m_compositor;
        options.audio = std::make_unique<FakeAudioOutput>();
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(f.project.mediaPool());
        engine.setTimeline(f.project.timeline());
        f.ctx.playback = &engine;
        connect(&engine, &PlaybackEngine::rateChanged, &engine,
            [](double rate) { media::MediaCache::instance().setPlaybackActive(rate != 0.0); });

        TimelineController controller(f.ctx);
        TimelineWidget view(f.ctx, &controller);
        controller.connectActions();
        view.resize(1400, 260);
        view.show();
        if (!QTest::qWaitForWindowExposed(&view)) {
            QTest::qFail("not exposed", __FILE__, __LINE__);
            return {};
        }
        view.setClipThumbnails(c.thumbs);
        view.setShowWaveforms(c.waveforms);
        const double pageFrames = m_page * kFps;
        view.setPixelsPerFrame(view.layout().trackAreaWidth() / pageFrames, kHeaderWidth);
        view.scrollToFrame(0);
        view.setFollowPlayhead(c.follow);

        // Warm the page that is shown (the user looked at it before pressing play).
        f.project.timeline()->setPlayhead(int64_t(pageFrames * 0.5));
        waitQuiet(view);
        // The playhead starts a second before the right edge.
        f.project.timeline()->setPlayhead(int64_t(pageFrames) - kFps);
        view.scrollToFrame(0);
        QTest::qWait(200);

        // Frame times, GUI stalls.
        std::vector<double> shown;
        QElapsedTimer clock;
        connect(&engine, &PlaybackEngine::frameReady, &engine,
            [&] { shown.push_back(clock.nsecsElapsed() / 1e6); });
        double longestStall = 0.0;
        double lastTick = 0.0;
        QTimer ticker;
        ticker.setInterval(2);
        ticker.setTimerType(Qt::PreciseTimer);
        connect(&ticker, &QTimer::timeout, &ticker, [&] {
            const double now = clock.nsecsElapsed() / 1e6;
            if (lastTick > 0.0)
                longestStall = std::max(longestStall, now - lastTick - 2.0);
            lastTick = now;
        });

        int thumbnails = 0;
        const QMetaObject::Connection counter = connect(&media::MediaCache::instance(),
            &media::MediaCache::thumbnailReady, this, [&] { ++thumbnails; });
        engine.resetStats();
        clock.start();
        ticker.start();
        engine.play();
        QTest::qWait(int(m_playSeconds * 1000));
        engine.pause();
        ticker.stop();
        disconnect(counter);
        const PlaybackEngine::Stats stats = engine.stats();

        Result r;
        r.presented = stats.presented;
        r.dropped = stats.dropped;
        r.seconds = stats.playSeconds;
        r.longestGuiStallMs = longestStall;
        r.thumbnailsDuringPlay = thumbnails;
        r.perSecond.assign(size_t(m_playSeconds), 0);
        for (size_t i = 0; i < shown.size(); ++i) {
            const size_t second = size_t(shown[i] / 1000.0);
            if (second < r.perSecond.size())
                ++r.perSecond[second];
            if (i > 0 && shown[i] > 500.0) // ignore start-up
                r.longestHoldMs = std::max(r.longestHoldMs, shown[i] - shown[i - 1]);
        }
        QString perSecond;
        for (const int n : r.perSecond)
            perSecond += QStringLiteral(" %1").arg(n);
        qInfo().noquote() << QStringLiteral("[pageturn] %1 | fps %2 shown %3 dropped %4 hold %5 ms "
                                            "gui-stall %6 ms tiles %8 | per second:%7")
                                 .arg(describe(c))
                                 .arg(r.fps(), 0, 'f', 1)
                                 .arg(r.presented)
                                 .arg(r.dropped)
                                 .arg(r.longestHoldMs, 0, 'f', 0)
                                 .arg(r.longestGuiStallMs, 0, 'f', 0)
                                 .arg(perSecond)
                                 .arg(r.thumbnailsDuringPlay);
        cache.setPlaybackActive(false);
        engine.setTimeline(nullptr);
        return r;
    }

    /// Repaints @p view until the cache has delivered everything it asked for.
    void waitQuiet(TimelineWidget& view)
    {
        media::MediaCache& cache = media::MediaCache::instance();
        QElapsedTimer quiet;
        quiet.start();
        QElapsedTimer total;
        total.start();
        while (quiet.elapsed() < 700 && total.elapsed() < 120000) {
            view.viewport()->repaint();
            QTest::qWait(50);
            if (cache.pendingImageCount() > 0)
                quiet.restart();
        }
    }

    QTemporaryDir m_temp;
    QString m_path;
    double m_page = 20.0;
    double m_playSeconds = 8.0;
    std::shared_ptr<Compositor> m_compositor;
    std::vector<std::unique_ptr<QTemporaryDir>> m_cacheDirs;
};

QTEST_MAIN(TestPageTurnPlayback)
#include "test_pageturnplayback.moc"
