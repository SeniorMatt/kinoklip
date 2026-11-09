// SPDX-License-Identifier: GPL-3.0-or-later
// First play after import (docs/archive/polish-v3.md R2): in the real main window, the time from the
// import of a file to the first frame shown in the timeline viewer and to the first audio a
// (fake) sound card plays, and the longest stall of the GUI event loop on the way. A cold
// media cache (test mode paths) like a first import. The numbers go to the test log; the
// limits only catch gross regressions (a stall the user would notice as a hang).
//
// Media: KK_FIRSTPLAY_MEDIA (colon-separated paths) or h264_25fps.mp4 (A/V) plus the perf
// clips of scripts/make_perf_media.sh when they exist.
#include "../media/MediaTestUtil.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "media/HwAccel.h"
#include "playback/AudioOutput.h"
#include "playback/AudioRing.h"
#include "playback/PlaybackClock.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

using namespace kk;

namespace {

/// Sound card stand-in: pulls 10 ms periods in real time and remembers when the first
/// non-silent sample came out (the moment the user would hear the clip start).
class ListeningOutput final : public AudioOutput
{
public:
    ~ListeningOutput() override { stop(); }
    bool isAvailable() const override { return true; }
    QString description() const override { return QStringLiteral("listening"); }
    void setVolume(float) override { }
    bool start(AudioRing* ring) override
    {
        stop();
        m_ring = ring;
        m_startUs = monotonicUs();
        m_played = 0;
        m_quit = false;
        m_thread = std::thread([this] { run(); });
        return true;
    }
    void stop() override
    {
        if (m_thread.joinable()) {
            m_quit = true;
            m_thread.join();
        }
    }
    void playedFrames(int64_t* frames, int64_t* atUs) const override
    {
        *frames = m_played.load();
        *atUs = m_at.load();
    }
    /// monotonicUs() of the first non-silent sample, 0 = none yet.
    [[nodiscard]] int64_t firstSoundUs() const { return m_firstSound.load(); }
    /// Forgets the first sound (scrub snippets before the play).
    void resetFirstSound() { m_firstSound = 0; }

private:
    void run()
    {
        std::vector<float> buffer;
        int64_t pulled = 0;
        while (!m_quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const int64_t now = monotonicUs();
            const int64_t target = (now - m_startUs) * 48 / 1000;
            if (target > pulled) {
                buffer.assign(size_t(target - pulled) * 2, 0.0f);
                const size_t got = m_ring->read(buffer.data(), size_t(target - pulled));
                if (m_firstSound.load() == 0)
                    for (size_t i = 0; i < got * 2; ++i)
                        if (std::abs(buffer[i]) > 1e-4f) {
                            m_firstSound = now;
                            break;
                        }
                pulled = target;
            }
            m_played = pulled;
            m_at = now;
        }
    }
    AudioRing* m_ring = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_quit {false};
    std::atomic<int64_t> m_played {0};
    std::atomic<int64_t> m_at {0};
    std::atomic<int64_t> m_firstSound {0};
    int64_t m_startUs = 0;
};

/// Records the longest gap between two runs of the GUI event loop (a 1 ms timer).
class StallMonitor : public QObject
{
public:
    StallMonitor()
    {
        m_timer.setInterval(1);
        m_timer.setTimerType(Qt::PreciseTimer);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            const qint64 now = m_clock.nsecsElapsed();
            m_maxGapNs = std::max(m_maxGapNs, now - m_last);
            m_last = now;
        });
    }
    void start()
    {
        m_clock.start();
        m_last = 0;
        m_maxGapNs = 0;
        m_timer.start();
    }
    /// Longest gap in ms so far, including a synchronous call in progress at @p mark.
    double maxGapMs()
    {
        m_maxGapNs = std::max(m_maxGapNs, m_clock.nsecsElapsed() - m_last);
        return double(m_maxGapNs) / 1e6;
    }
    /// Counts the time of a synchronous GUI-thread call as a gap (the loop could not run).
    void markNow() { m_last = m_clock.nsecsElapsed(); }

private:
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_last = 0;
    qint64 m_maxGapNs = 0;
};

QStringList mediaFiles()
{
    const QString env = qEnvironmentVariable("KK_FIRSTPLAY_MEDIA");
    if (!env.isEmpty())
        return env.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    QStringList files {test::fixture("h264_25fps.mp4")};
    for (const char* name : {"screen_1080p60.mp4", "hevc_2160p30.mp4"}) {
        const QString path
            = QStringLiteral(KK_TEST_DATA_DIR "/generated/perf/") + QLatin1String(name);
        if (QFileInfo::exists(path))
            files.append(path);
    }
    return files;
}

} // namespace

class TestFirstPlay : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_firstplay"));
        QSettings().clear();
        // A cold cache: no thumbnails, waveforms or probes from an earlier run.
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).removeRecursively();
        initResources();
        theme::apply(*qApp);
    }

    void importToFirstFrameAndSound_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("atOnce");
        for (const QString& path : mediaFiles()) {
            const QString name = QFileInfo(path).fileName();
            // Space at once after the drop (worst case), or after the paused frame appeared.
            QTest::newRow(qPrintable(name + QStringLiteral(" space at once"))) << path << true;
            QTest::newRow(qPrintable(name + QStringLiteral(" space later"))) << path << false;
        }
    }

    void importToFirstFrameAndSound()
    {
        QFETCH(QString, path);
        QFETCH(bool, atOnce);
        if (!QFileInfo::exists(path))
            QSKIP("media missing (scripts/make_test_media.sh)");
        Project project;
        SelectionModel selection;
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        auto* device = new ListeningOutput;
        PlaybackEngine::Options options;
        options.audio = std::unique_ptr<AudioOutput>(device);
        PlaybackEngine playback(std::move(options));
        playback.setMediaPool(project.mediaPool());
        playback.setTimeline(project.timeline());
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        ctx.playback = &playback;
        MainWindowOptions wo;
        wo.autosave = false;
        wo.restoreLayout = false;
        wo.saveLayout = false;
        // Experiments: KK_FIRSTPLAY_NOTHUMBS=1 (no media cache work), =thumbs, =waves (one).
        if (const QString no = qEnvironmentVariable("KK_FIRSTPLAY_NOTHUMBS"); !no.isEmpty()) {
            if (no != QLatin1String("waves"))
                QSettings().setValue(QStringLiteral("timeline/thumbnails"), QStringLiteral("none"));
            if (no != QLatin1String("thumbs"))
                QSettings().setValue(QStringLiteral("timeline/waveforms"), false);
        } else {
            QSettings().remove(QStringLiteral("timeline/thumbnails"));
            QSettings().remove(QStringLiteral("timeline/waveforms"));
        }
        MainWindow window(ctx, wo);
        if (!qEnvironmentVariableIsSet("KK_FIRSTPLAY_COLD"))
            media::warmUpHwDecoding(); // what main() does after creating the window
        window.resize(1600, 1000);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();

        StallMonitor stalls;
        stalls.start();
        QTest::qWait(400); // settle (the user opens the import dialog)
        stalls.start();
        QElapsedTimer clock;
        clock.start();
        const int64_t t0 = monotonicUs();

        // Import (the File > Import path minus the file dialog) and append to the timeline.
        stalls.markNow();
        const ImportResult imported = project.importMedia({path});
        const double importMs = double(clock.nsecsElapsed()) / 1e6;
        QCOMPARE(imported.importedIds.size(), 1);
        const QString id = imported.importedIds.front();
        TimelineController* controller = window.editPage()->timeline()->controller();
        controller->editSources({SourceSelection {id, 0, 0}}, SourceEditMode::AppendAtEnd);
        QVERIFY(!project.timeline()->data().videoTracks[0].clips.empty());
        project.timeline()->setPlayhead(0);
        const double appendMs = double(clock.nsecsElapsed()) / 1e6;

        // The paused frame at the playhead (what the viewer shows after the drop).
        QSignalSpy frames(&playback, &PlaybackEngine::frameReady);
        double stillMs = -1;
        if (!atOnce) {
            QTRY_VERIFY_WITH_TIMEOUT(frames.count() >= 1, 10000);
            stillMs = double(monotonicUs() - t0) / 1000.0;
            QTest::qWait(700); // the user reaches for Space
            frames.clear();
        }
        const int droppedBefore = int(playback.stats().dropped);
        if (qEnvironmentVariableIsSet("KK_FIRSTPLAY_TRACE"))
            connect(
                &playback, &PlaybackEngine::frameReady, &window, [t0](const RenderedFramePtr& f) {
                    if (f)
                        qInfo("  frame %lld at %.1f ms: decode %.1f render %.1f",
                            (long long)f->frame, double(monotonicUs() - t0) / 1000.0, f->decodeMs,
                            f->renderMs);
                });
        device->resetFirstSound(); // the audio scrubbing snippet of setPlayhead()
        const int64_t playUs = monotonicUs();
        actions.action(QStringLiteral("transport.play_pause"))->trigger();
        const double playCallMs = double(monotonicUs() - playUs) / 1000.0;
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() >= 1, 10000);
        const double firstFrameMs = double(monotonicUs() - t0) / 1000.0;
        const double frameAfterPlayMs = double(monotonicUs() - playUs) / 1000.0;
        const bool hasAudio = project.mediaPool()->item(id)->info.hasAudio;
        if (hasAudio)
            QTRY_VERIFY_WITH_TIMEOUT(device->firstSoundUs() != 0, 10000);
        const double firstSoundMs = hasAudio ? double(device->firstSoundUs() - t0) / 1000.0 : -1.0;
        const double soundAfterPlayMs
            = hasAudio ? double(device->firstSoundUs() - playUs) / 1000.0 : -1.0;
        QTest::qWait(500); // the thumbnails and waveforms arriving while playing
        const double maxStallMs = stalls.maxGapMs();
        playback.pause();

        qInfo("first play %s (%s): import %.1f ms, append %.1f ms, paused frame %.1f ms after "
              "import, play() %.1f ms; first played frame %.1f ms after import (%.1f after "
              "play), first sound %.1f ms after import (%.1f after play); longest GUI stall "
              "%.1f ms; %d frames shown, %d dropped",
            qPrintable(QFileInfo(path).fileName()), atOnce ? "space at once" : "space later",
            importMs, appendMs - importMs, stillMs, playCallMs, firstFrameMs, frameAfterPlayMs,
            firstSoundMs, soundAfterPlayMs, maxStallMs, int(playback.stats().presented),
            int(playback.stats().dropped) - droppedBefore);
        // Gross regressions only (timing-sensitive under load).
        QVERIFY2(frameAfterPlayMs < 3000, "the first frame took seconds");
        QVERIFY2(maxStallMs < 1500, "the GUI froze for more than a second");
    }
};

QTEST_MAIN(TestFirstPlay)
#include "test_firstplay.moc"
