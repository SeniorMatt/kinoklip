// SPDX-License-Identifier: GPL-3.0-or-later
// Memory soak (spec 103): the real main window with the real program engine and compositor
// plays a generated 1080p clip through many cycles of what a user does in a long session -- play,
// stop, seek, clip speed 100 % / 25 % / 12.5 %, a small and a large (full-screen) program viewer,
// every page, a second timeline opened and deleted -- and the counters that must come back to
// where they were are compared with their values after a warm-up:
//
//   RenderedFrame objects alive         (RenderedFrame::aliveCount)
//   GlOutput slots allocated / in use   (Compositor::resourceStats, the "GL textures" of the viewer)
//   open decoders of the render thread  (Compositor::resourceStats)
//   decoders idle in the MediaCache, tiles cached in the MediaCache
//   process memory                      (VmRSS in /proc/self/status)
//
// Bounds (documented in docs/contributing.md, "Memory leaks"): frames alive and slots in use
// stay within kFrameSlack of the baseline, slots allocated within kSlotSlack, render decoders at
// DecoderPool::kMaxOpen, idle decoders and cached tiles within a few of the baseline, and the
// process grows by less than kRssGrowthMiB over the measured cycles after the warm-up.
//
// Run alone:   ctest -L soak            (about 40-60 s offscreen, llvmpipe)
// Environment: KK_SOAK_CYCLES=n         cycles after the warm-up (default 10; 200 for an
//                                       overnight soak)
//              KK_SOAK_LOG=1            print the counters after every cycle
#include "../media/MediaTestUtil.h"
#include "../timeline/TimelineTestUtils.h"
#include "app/MainWindow.h"
#include "core/EditOps.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "media/MediaCache.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/StandardActions.h"

#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace kk;

namespace {

constexpr int kWarmupCycles = 3;
constexpr int kFrameSlack = 6; ///< frames alive / slots in use above the baseline (viewers, engine)
constexpr int kSlotSlack = 8; ///< output slots allocated above the baseline
constexpr int kDecoderSlack = 3; ///< idle MediaCache decoders above the baseline
constexpr int kTileSlack = 64; ///< cached tiles above the baseline (the cache has its own limit)
constexpr double kRssGrowthMiB = 250.0; ///< process growth over the measured cycles
constexpr int kFps = 25;

double rssMiB()
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    for (const QByteArray& line : f.readAll().split('\n'))
        if (line.startsWith("VmRSS:"))
            return line.mid(6).simplified().split(' ').value(0).toDouble() / 1024.0;
    return 0;
}

struct Counters
{
    int framesAlive = 0;
    int slots = 0;
    int slotsInUse = 0;
    int renderDecoders = 0;
    int idleDecoders = 0;
    int tiles = 0;
    double rssMiB = 0;

    QString text() const
    {
        return QStringLiteral("frames %1 | slots %2 (in use %3) | render decoders %4 | idle "
                              "decoders %5 | tiles %6 | RSS %7 MiB")
            .arg(framesAlive)
            .arg(slots)
            .arg(slotsInUse)
            .arg(renderDecoders)
            .arg(idleDecoders)
            .arg(tiles)
            .arg(rssMiB, 0, 'f', 0);
    }
};

} // namespace

class TestMemorySoak : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { kk::test::initTimelineTestCase("test_memorysoak"); }
    void cleanupTestCase() { media::MediaCache::instance().shutdown(); }

    void sessionCyclesReturnToBaseline()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("no ffmpeg");
        QTemporaryDir dir;
        const QString clip = dir.path() + QStringLiteral("/soak.mp4");
        QProcess ffmpeg;
        ffmpeg.setProcessChannelMode(QProcess::ForwardedChannels);
        ffmpeg.start(QStringLiteral("ffmpeg"),
            {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                "testsrc2=s=1920x1080:r=25:d=8", "-c:v", "libx264", "-preset", "ultrafast", "-crf",
                "28", "-g", "25", "-pix_fmt", "yuv420p", clip});
        QVERIFY(ffmpeg.waitForFinished(120000) && ffmpeg.exitCode() == 0);

        Project project;
        SelectionModel selection;
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        PlaybackEngine::Options options;
        options.noAudio = true;
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        QObject::connect(&engine, &PlaybackEngine::rateChanged, &engine,
            [](double rate) { media::MediaCache::instance().setPlaybackActive(rate != 0.0); });
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.playback = &engine;
        QObject::connect(&project, &Project::editCommitted, &engine, &PlaybackEngine::noteEditCommitted);

        const ImportResult imported = project.importMedia({clip});
        QVERIFY(imported.importedIds.size() == 1);
        const media::MediaInfo info = project.mediaPool()->item(imported.importedIds[0])->info;
        project.beginEdit(QStringLiteral("clip"));
        TimelineData& tl = project.timeline()->mutableData();
        tl = makeDefaultTimeline(QStringLiteral("t"), {kFps, 1}, 1920, 1080, 1, 1);
        ClipData c;
        c.id = tl.allocateId();
        c.mediaId = imported.importedIds[0];
        c.sourceDurationUs = info.durationUs;
        c.duration = int64_t(info.durationUs * kFps / 1000000);
        tl.videoTracks[0].clips.push_back(c);
        const quint64 clipId = c.id;
        const int64_t sourceFrames = c.duration;
        project.endEdit();

        MainWindowOptions wo;
        wo.autosave = false;
        wo.restoreLayout = false;
        wo.saveLayout = false;
        MainWindow window(ctx, wo);
        window.resize(1000, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const std::shared_ptr<Compositor> compositor = engine.compositor();
        QVERIFY(compositor);
        media::MediaCache& cache = media::MediaCache::instance();
        const bool log = qEnvironmentVariableIsSet("KK_SOAK_LOG");
        bool ok = false;
        const int measured
            = qEnvironmentVariable("KK_SOAK_CYCLES", QStringLiteral("10")).toInt(&ok);
        const int cycles = ok && measured > 0 ? measured : 10;

        const QStringList pages {"edit", "cut", "color", "compositing", "deliver"};
        const double speeds[] {1.0, 0.25, 0.125};
        const QAction* fullscreen = actions.action(QStringLiteral("view.fullscreen_viewer"));
        QVERIFY(fullscreen);

        auto setSpeed = [&](double speed) {
            project.beginEdit(QStringLiteral("Change Clip Speed"));
            ClipData* cd = editops::clipById(project.timeline()->mutableData(), clipId);
            if (cd) {
                cd->speed = speed;
                cd->duration = std::max<int64_t>(1, int64_t(double(sourceFrames) / speed));
            }
            project.endEdit();
        };
        auto settle = [&](int ms) {
            QTest::qWait(ms);
            QCoreApplication::processEvents();
        };
        auto measure = [&]() {
            Counters n;
            n.framesAlive = RenderedFrame::aliveCount();
            const render::ResourceStats s = compositor->resourceStats();
            n.slots = s.outputSlots;
            n.slotsInUse = s.outputSlotsInUse;
            n.renderDecoders = s.decoders;
            n.idleDecoders = cache.idleDecoderCount();
            n.tiles = cache.cachedImageCount();
            n.rssMiB = rssMiB();
            return n;
        };

        // One cycle = everything a long session does once.
        auto cycle = [&](int index) {
            const double speed = speeds[index % 3];
            setSpeed(speed);
            for (int p = 0; p < pages.size(); ++p) {
                window.setCurrentPage(pages[(p + index) % pages.size()]);
                settle(40);
                engine.seek(int64_t(index * 7 + p * 11) % 90);
                settle(40);
            }
            window.setCurrentPage(QStringLiteral("edit"));
            window.resize(1000, 640);
            settle(60);
            engine.play();
            settle(250);
            // Full-screen viewer and back, then a large and a small window (viewer resize).
            const_cast<QAction*>(fullscreen)->trigger();
            settle(150);
            window.resize(1900, 1000);
            settle(150);
            const_cast<QAction*>(fullscreen)->trigger();
            settle(100);
            window.resize(1000, 640);
            settle(100);
            engine.stop();
            engine.seek(int64_t(index * 13) % 60);
            settle(80);
            // A second timeline: open (tab), show its empty frame, back, delete.
            const quint64 first = project.activeTimelineUid();
            const quint64 second = project.createTimeline(QStringLiteral("Second"));
            QVERIFY(project.setActiveTimeline(second));
            settle(120);
            QVERIFY(project.setActiveTimeline(first));
            settle(80);
            QVERIFY(project.deleteTimeline(second));
            settle(80);
        };

        for (int i = 0; i < kWarmupCycles; ++i)
            cycle(i);
        engine.stop();
        window.setCurrentPage(QStringLiteral("edit"));
        settle(500);
        const Counters base = measure();
        qInfo().noquote() << "[soak] baseline after warm-up:" << base.text();

        QElapsedTimer timer;
        timer.start();
        Counters peak = base;
        for (int i = 0; i < cycles; ++i) {
            cycle(kWarmupCycles + i);
            if (QTest::currentTestFailed())
                return;
            if (log) {
                const Counters n = measure();
                qInfo().noquote() << "[soak] cycle" << i + 1 << n.text();
            }
            const Counters n = measure();
            peak.slots = std::max(peak.slots, n.slots);
            peak.renderDecoders = std::max(peak.renderDecoders, n.renderDecoders);
        }
        engine.stop();
        window.setCurrentPage(QStringLiteral("edit"));
        window.resize(1000, 640);
        settle(800); // the viewer drops its frames; the render thread publishes its stats
        const Counters end = measure();
        qInfo().noquote() << "[soak]" << cycles << "cycles in" << timer.elapsed() / 1000
                          << "s, end:" << end.text() << "| peak slots" << peak.slots;

        // Issue #8: RAM and VRAM grew without bound during slow-motion playback with
        // full-screen toggles. The fix agent removes these QEXPECT_FAILs when the cause is fixed.
        QVERIFY2(end.framesAlive <= base.framesAlive + kFrameSlack,
            qPrintable(QStringLiteral("RenderedFrame objects: %1 -> %2")
                           .arg(base.framesAlive)
                           .arg(end.framesAlive)));
        QVERIFY2(end.slotsInUse <= base.slotsInUse + kFrameSlack,
            qPrintable(QStringLiteral("output slots in use: %1 -> %2")
                           .arg(base.slotsInUse)
                           .arg(end.slotsInUse)));
        QVERIFY2(end.slots <= base.slots + kSlotSlack,
            qPrintable(QStringLiteral("output slots allocated: %1 -> %2")
                           .arg(base.slots)
                           .arg(end.slots)));
        QVERIFY2(peak.renderDecoders <= render::DecoderPool::kMaxOpen,
            qPrintable(QStringLiteral("render decoders open: %1").arg(peak.renderDecoders)));
        QVERIFY2(end.idleDecoders <= base.idleDecoders + kDecoderSlack,
            qPrintable(QStringLiteral("idle decoders: %1 -> %2")
                           .arg(base.idleDecoders)
                           .arg(end.idleDecoders)));
        QVERIFY2(end.tiles <= base.tiles + kTileSlack,
            qPrintable(QStringLiteral("cached tiles: %1 -> %2").arg(base.tiles).arg(end.tiles)));
        if (end.rssMiB > 0)
            QVERIFY2(end.rssMiB - base.rssMiB < kRssGrowthMiB,
                qPrintable(QStringLiteral("process memory grew %1 MiB (limit %2)")
                               .arg(end.rssMiB - base.rssMiB, 0, 'f', 0)
                               .arg(kRssGrowthMiB)));
    }
};

QTEST_MAIN(TestMemorySoak)
#include "test_memorysoak.moc"
