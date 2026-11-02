// SPDX-License-Identifier: GPL-3.0-or-later
// Sustained playback measurement: a 1080p25 timeline with two video layers (1080p H.264 under a
// 720p picture-in-picture with drop shadow) and a title, linked audio, played for several seconds
// by the PlaybackEngine with a fake sound card as master clock. Prints fps, dropped frames, the
// compositor time per frame and the quality Auto settled on, for a viewer-sized output (the
// Edit page viewer, ~1000x560 device pixels) and a full-HD output (full-screen viewer).
//
// ctest runs it on the offscreen platform (whatever OpenGL that has: EGL on the GPU here,
// otherwise the CPU fallback). Run it with llvmpipe or another driver explicitly with e.g.
//   QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/test_playbackperf
#include "../media/MediaTestUtil.h"
#include "PlaybackTestUtil.h"
#include "core/EffectRegistry.h"
#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "media/MediaInfo.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTest>

using namespace kk;

namespace {

constexpr int kPlaySeconds = 6;

void buildTimeline(Project& project)
{
    const ImportResult imported
        = project.importMedia({test::fixture("perf_1080p.mp4"), test::fixture("h264_25fps.mp4")});
    QCOMPARE(imported.importedIds.size(), 2);
    project.beginEdit(QStringLiteral("perf"));
    TimelineData& tl = project.timeline()->mutableData();
    tl = makeDefaultTimeline(QStringLiteral("perf"), {25, 1}, 1920, 1080);
    tl.videoTracks.push_back(TrackData {});
    tl.videoTracks.back().id = tl.allocateId();
    auto clip = [&](const QString& media, int64_t duration, int64_t durationUs) {
        ClipData c;
        c.id = tl.allocateId();
        c.mediaId = media;
        c.duration = duration;
        c.sourceDurationUs = durationUs;
        return c;
    };
    // V1: 1080p, 8 s.
    tl.videoTracks[0].clips.push_back(clip(imported.importedIds[0], 200, 8'000'000));
    // V2: 720p picture-in-picture with a drop shadow.
    ClipData pip = clip(imported.importedIds[1], 200, 10'000'000);
    pip.linkGroup = 1;
    pip.transform.setBase(keys::ZoomX, 0.4);
    pip.transform.setBase(keys::ZoomY, 0.4);
    pip.transform.setBase(keys::PosX, 500);
    pip.transform.setBase(keys::PosY, -250);
    const EffectRegistry& registry = EffectRegistry::instance();
    pip.effects.push_back(registry.createEffect(QStringLiteral("drop_shadow"), tl.allocateId()));
    tl.videoTracks[1].clips.push_back(pip);
    // V3: title.
    ClipData title;
    title.id = tl.allocateId();
    title.kind = ClipKind::Title;
    title.duration = 200;
    title.title = registry.defaultParams(QStringLiteral("text_title"));
    title.title.setStatic(keys::Text, QStringLiteral("Kinoklip playback"));
    title.title.setBase(keys::Size, 90);
    title.transform.setBase(keys::PosY, 350);
    tl.videoTracks[2].clips.push_back(title);
    // A1: the PiP's sound.
    ClipData audio = clip(imported.importedIds[1], 200, 10'000'000);
    audio.linkGroup = 1;
    tl.audioTracks[0].clips.push_back(audio);
    project.endEdit();
}

/// A timeline of the size and rate of @p path holding just that clip (and its sound).
/// @return the frame rate, or an invalid Rational on failure.
Rational buildSingleClipTimeline(Project& project, const QString& path)
{
    const ImportResult imported = project.importMedia({path});
    if (imported.importedIds.size() != 1)
        return {};
    const media::MediaInfo& info = project.mediaPool()->item(imported.importedIds[0])->info;
    const Rational fps = info.frameRate.isValid() ? info.frameRate : Rational {25, 1};
    project.beginEdit(QStringLiteral("perf"));
    TimelineData& tl = project.timeline()->mutableData();
    tl = makeDefaultTimeline(QStringLiteral("perf"), fps, info.displayWidth, info.displayHeight);
    ClipData c;
    c.id = tl.allocateId();
    c.mediaId = imported.importedIds[0];
    c.sourceDurationUs = info.durationUs;
    c.duration = std::max<int64_t>(1, usToFrames(info.durationUs, fps));
    tl.videoTracks[0].clips.push_back(c);
    if (info.hasAudio) {
        c.id = tl.allocateId();
        tl.audioTracks[0].clips.push_back(c);
    }
    project.endEdit();
    return fps;
}

QStringList perfMedia()
{
    QStringList paths {test::fixture("perf_1080p.mp4")};
    paths += qEnvironmentVariable("KK_PERF_MEDIA").split(QLatin1Char(':'), Qt::SkipEmptyParts);
    return paths;
}

} // namespace

class TestPlaybackPerf : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    /// Seek-to-first-frame latency while playing (alpha.2 feedback 3): clicks on the ruler
    /// (jumps of a few seconds, Timeline::setPlayhead as the timeline view does) and a drag
    /// (a move every 16 ms). Latency = from the playhead change until a frame within half a
    /// second after the target is on screen.
    void seekWhilePlaying_data()
    {
        QTest::addColumn<QString>("path");
        for (const QString& p : perfMedia())
            QTest::newRow(qPrintable(QFileInfo(p).fileName())) << p;
    }

    void seekWhilePlaying()
    {
        QFETCH(QString, path);
        QString why;
        if (!test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Project project;
        const Rational fps = buildSingleClipTimeline(project, path);
        QVERIFY(fps.isValid());
        const int64_t frames = project.timeline()->snapshot()->duration();
        PlaybackEngine::Options options;
        options.audio = std::make_unique<test::FakeAudioOutput>(10, 40);
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        int client = 0;
        engine.setOutputClient(&client, QSize(1000, 562), engine.compositor()->isGpu());
        QVERIFY(QTest::qWaitFor([&] { return engine.currentFrame() != nullptr; }, 10000));
        int64_t lastShown = -1;
        QElapsedTimer clock;
        clock.start();
        qint64 shownAtNs = 0;
        QObject::connect(
            &engine, &PlaybackEngine::frameReady, &engine, [&](const RenderedFramePtr& f) {
                if (f && f->ok) {
                    lastShown = f->frame;
                    shownAtNs = clock.nsecsElapsed();
                }
            });
        const int64_t window = std::max<int64_t>(2, int64_t(fps.toDouble() / 2));
        // Waits for a frame in [target, target + window]; returns the latency in ms.
        auto waitShown = [&](int64_t target, qint64 sinceNs) {
            const bool ok = QTest::qWaitFor(
                [&] { return lastShown >= target && lastShown <= target + window; }, 3000);
            return ok ? std::max(0.0, double(shownAtNs - sinceNs) / 1e6) : 3000.0;
        };
        engine.play();
        QTest::qWait(600);

        // Clicks: jump around the clip (forward and back, across GOPs).
        const double spots[] = {0.7, 0.2, 0.55, 0.1, 0.85, 0.4};
        double clickSum = 0.0;
        double clickMax = 0.0;
        for (double spot : spots) {
            const int64_t target = int64_t(spot * double(frames));
            const qint64 t0 = clock.nsecsElapsed();
            project.timeline()->setPlayhead(target);
            const double ms = waitShown(target, t0);
            clickSum += ms;
            clickMax = std::max(clickMax, ms);
            QTest::qWait(500);
            QVERIFY(engine.isPlaying());
        }

        // Drag: 40 moves 16 ms apart over ~3 s of material.
        const int64_t from = frames / 4;
        const int64_t step = std::max<int64_t>(1, int64_t(fps.toDouble() * 3.0) / 40);
        int64_t target = from;
        int shownDuringDrag = 0;
        const QMetaObject::Connection counter
            = QObject::connect(&engine, &PlaybackEngine::frameReady, &engine,
                [&](const RenderedFramePtr&) { ++shownDuringDrag; });
        for (int i = 0; i < 40; ++i) {
            target = std::min(frames - 1, from + i * step);
            project.timeline()->setPlayhead(target);
            QTest::qWait(16);
        }
        QObject::disconnect(counter);
        const qint64 t0 = clock.nsecsElapsed();
        const double dragMs = waitShown(target, t0);
        QTest::qWait(1000);
        const bool playing = engine.isPlaying();
        const int64_t after = engine.position();
        engine.pause();
        qInfo("%s | %s | click seek to frame on screen: avg %.0f ms, max %.0f ms | drag: %d "
              "frames shown during 40 moves, last position on screen %.0f ms after the drag | "
              "1 s later at frame %lld (from %lld)",
            qPrintable(engine.compositor()->backendDescription()), QTest::currentDataTag(),
            clickSum / double(std::size(spots)), clickMax, shownDuringDrag, dragMs,
            qlonglong(after), qlonglong(target));
        QVERIFY(playing);
        QVERIFY(after > target); // playback continued from the drag position
    }

    /// One clip filling a timeline of its own size and rate, played at Full (the default)
    /// in the Edit-page viewer size: the plain "can it play this file" number, with the time
    /// per stage. perf_1080p.mp4 always; more files with KK_PERF_MEDIA=/path/a.mp4:/path/b.mov
    /// (scripts/make_perf_media.sh writes screen recordings and a 4K HEVC clip); other
    /// qualities with KK_PERF_QUALITY=auto|full|half|quarter.
    void singleClipPlayback_data()
    {
        QTest::addColumn<QString>("path");
        QTest::newRow("perf_1080p.mp4") << test::fixture("perf_1080p.mp4");
        for (const QString& p :
            qEnvironmentVariable("KK_PERF_MEDIA").split(QLatin1Char(':'), Qt::SkipEmptyParts))
            QTest::newRow(qPrintable(QFileInfo(p).fileName())) << p;
    }

    void singleClipPlayback()
    {
        QFETCH(QString, path);
        QString why;
        if (!test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Project project;
        const ImportResult imported = project.importMedia({path});
        QCOMPARE(imported.importedIds.size(), 1);
        const media::MediaInfo& info = project.mediaPool()->item(imported.importedIds[0])->info;
        const Rational fps = info.frameRate.isValid() ? info.frameRate : Rational {25, 1};
        project.beginEdit(QStringLiteral("perf"));
        TimelineData& tl = project.timeline()->mutableData();
        tl = makeDefaultTimeline(
            QStringLiteral("perf"), fps, info.displayWidth, info.displayHeight);
        ClipData c;
        c.id = tl.allocateId();
        c.mediaId = imported.importedIds[0];
        c.sourceDurationUs = info.durationUs;
        c.duration = std::max<int64_t>(1, usToFrames(info.durationUs, fps));
        tl.videoTracks[0].clips.push_back(c);
        if (info.hasAudio) {
            c.id = tl.allocateId();
            tl.audioTracks[0].clips.push_back(c);
        }
        project.endEdit();

        auto fake = std::make_unique<test::FakeAudioOutput>(10, 40);
        PlaybackEngine::Options options;
        options.audio = std::move(fake);
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        int client = 0;
        engine.setOutputClient(&client, QSize(1000, 562), engine.compositor()->isGpu());
        const QStringList modes {QStringLiteral("auto"), QStringLiteral("full"),
            QStringLiteral("half"), QStringLiteral("quarter")};
        const int mode = modes.indexOf(qEnvironmentVariable("KK_PERF_QUALITY", modes[1]));
        engine.setQualityMode(PlaybackEngine::QualityMode(std::max(0, mode)));
        QVERIFY(QTest::qWaitFor([&] { return engine.currentFrame() != nullptr; }, 10000));
        engine.play();
        QTest::qWait(500);
        engine.resetStats();
        const int seconds = int(std::min<int64_t>(kPlaySeconds, info.durationUs / 1'000'000 - 1));
        QTest::qWait(std::max(1, seconds) * 1000);
        const PlaybackEngine::Stats s = engine.stats();
        const Quality used = engine.currentQuality();
        const QSize rendered = engine.renderSize();
        engine.pause();
        qInfo("%s | %s %dx%d %.3f fps %s | %.2f fps of %.3f, %lld dropped, render %.1f ms/frame "
              "[decode %.1f upload %.1f composite %.1f output %.1f], quality %.2f at %dx%d",
            qPrintable(engine.compositor()->backendDescription()), QTest::currentDataTag(),
            info.width, info.height, fps.toDouble(), qPrintable(info.videoCodec), s.fps(),
            fps.toDouble(), qlonglong(s.dropped), s.averageRenderMs, s.averageDecodeMs,
            s.averageUploadMs, s.averageCompositeMs, s.averageOutputMs, qualityFactor(used),
            rendered.width(), rendered.height());
        QVERIFY(s.presented > 0);
    }

    void sustainedPlayback_data()
    {
        QTest::addColumn<QSize>("viewer");
        QTest::addColumn<int>("quality");
        QTest::newRow("edit viewer 1000x562, Auto")
            << QSize(1000, 562) << int(PlaybackEngine::QualityMode::Auto);
        QTest::newRow("edit viewer 1000x562, Full")
            << QSize(1000, 562) << int(PlaybackEngine::QualityMode::Full);
        QTest::newRow("full screen 1920x1080, Auto")
            << QSize(1920, 1080) << int(PlaybackEngine::QualityMode::Auto);
    }

    void sustainedPlayback()
    {
        QFETCH(QSize, viewer);
        QFETCH(int, quality);
        QString why;
        if (!test::ensureFixtures(&why)) {
            QSKIP(qPrintable(why));
        }
        Project project;
        buildTimeline(project);
        auto fake = std::make_unique<test::FakeAudioOutput>(10, 40);
        test::FakeAudioOutput* device = fake.get();
        PlaybackEngine::Options options;
        options.audio = std::move(fake);
        PlaybackEngine engine(std::move(options));
        engine.setMediaPool(project.mediaPool());
        engine.setTimeline(project.timeline());
        int client = 0;
        const bool gpu = engine.compositor()->isGpu();
        engine.setOutputClient(&client, viewer, gpu);
        engine.setQualityMode(PlaybackEngine::QualityMode(quality));
        // Warm up: open decoders, compile shaders, rasterise the title.
        QVERIFY(QTest::qWaitFor([&] { return engine.currentFrame() != nullptr; }, 10000));

        engine.play();
        QTest::qWait(500); // decoders reach steady state
        engine.resetStats();
        QTest::qWait(kPlaySeconds * 1000);
        const PlaybackEngine::Stats s = engine.stats();
        const Quality used = engine.currentQuality();
        engine.pause();
        const double expected = s.playSeconds * 25.0;
        qInfo("%s | %s | output %dx%d | %.2f fps (%lld presented of %.0f due), %lld dropped, "
              "render %.1f ms/frame, quality %.2f, audio master %d, A/V error %.3f frames, "
              "underrun %lld",
            qPrintable(engine.compositor()->backendDescription()), QTest::currentDataTag(),
            engine.outputSize().width(), engine.outputSize().height(), s.fps(),
            qlonglong(s.presented), expected, qlonglong(s.dropped), s.averageRenderMs,
            qualityFactor(used), int(s.audioMaster), s.avOffsetFrames,
            qlonglong(device->underrunFrames()));
        QVERIFY(s.presented > 0);
        QVERIFY(s.audioMaster);
        if (gpu && qEnvironmentVariableIsEmpty("KK_PERF_NO_ASSERT")) {
            // On a real GPU this timeline must play without drops.
            QVERIFY2(s.dropped <= expected * 0.02, "more than 2 % dropped on the GPU backend");
        }
    }
};

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QGuiApplication app(argc, argv);
    TestPlaybackPerf test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_playbackperf.moc"
