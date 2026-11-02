// SPDX-License-Identifier: GPL-3.0-or-later
// PlaybackEngine against a real Compositor (CPU fallback under ctest's offscreen platform) and a
// fake sound card: transport state machine, J/K/L, loop, play in-to-out, end handling, playhead
// write-back, scrub coalescing, re-render on edits, audio-master clock and A/V offset bound.
#include "PlaybackTestUtil.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

using namespace kk;

namespace {

/// 320x180 25 fps timeline: a red solid on V1 over [0, frames), a blue one on V2 over
/// [blueFrom, frames) so frames differ visibly.
TimelineData solidTimeline(int64_t frames, int64_t blueFrom = 1'000'000)
{
    TimelineData tl = makeDefaultTimeline(QStringLiteral("t"), {25, 1}, 320, 180);
    ClipData red;
    red.id = tl.allocateId();
    red.kind = ClipKind::Solid;
    red.solidColor = Qt::red;
    red.start = 0;
    red.duration = frames;
    tl.videoTracks[0].clips.push_back(red);
    if (blueFrom < frames) {
        ClipData blue = red;
        blue.id = tl.allocateId();
        blue.solidColor = Qt::blue;
        blue.start = blueFrom;
        blue.duration = frames - blueFrom;
        tl.videoTracks[1].clips.push_back(blue);
    }
    return tl;
}

/// One compositor for all cases (released in main() before the application object).
std::shared_ptr<Compositor> g_compositor;

std::shared_ptr<Compositor> sharedCompositor()
{
    if (!g_compositor) {
        g_compositor = std::make_shared<Compositor>();
    }
    return g_compositor;
}

std::unique_ptr<PlaybackEngine> makeEngine(std::unique_ptr<AudioOutput> audio = {})
{
    PlaybackEngine::Options options;
    options.compositor = sharedCompositor();
    options.noAudio = !audio;
    options.audio = std::move(audio);
    return std::make_unique<PlaybackEngine>(std::move(options));
}

/// Waits until @p pred is true (max @p ms).
template<typename Pred>
bool waitFor(Pred pred, int ms = 3000)
{
    return QTest::qWaitFor(pred, ms);
}

QRgb centre(const RenderedFramePtr& f)
{
    return f && !f->image.isNull() ? f->image.pixel(f->image.width() / 2, f->image.height() / 2)
                                   : 0;
}

} // namespace

class TestPlaybackEngine : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void stateMachineAndShuttle()
    {
        Timeline tl;
        tl.setData(solidTimeline(500));
        auto e = makeEngine();
        e->setTimeline(&tl);
        QSignalSpy states(e.get(), &PlaybackEngine::stateChanged);
        QCOMPARE(e->state(), PlaybackEngine::State::Stopped);
        e->play();
        QCOMPARE(e->state(), PlaybackEngine::State::Playing);
        QCOMPARE(e->rate(), 1.0);
        e->shuttleForward(); // already at 1x: L doubles
        QCOMPARE(e->rate(), 2.0);
        QCOMPARE(e->state(), PlaybackEngine::State::Shuttling);
        e->shuttleForward();
        e->shuttleForward();
        e->shuttleForward();
        QCOMPARE(e->rate(), 8.0);
        e->shuttleReverse();
        QCOMPARE(e->rate(), -1.0);
        e->shuttleReverse();
        QCOMPARE(e->rate(), -2.0);
        e->shuttleStop();
        QCOMPARE(e->state(), PlaybackEngine::State::Stopped);
        e->togglePlay();
        QVERIFY(e->isPlaying());
        e->togglePlay();
        QVERIFY(!e->isPlaying());
        e->setRate(100);
        QCOMPARE(e->rate(), 8.0); // clamped
        e->stop();
        QCOMPARE(e->rate(), 0.0);
        QVERIFY(states.count() >= 6);
    }

    void kHeldStepsAndSlowPlays()
    {
        Timeline tl;
        tl.setData(solidTimeline(500));
        tl.setPlayhead(100);
        auto e = makeEngine();
        e->setTimeline(&tl);
        e->setShuttleKeyHeld(ShuttleControl::Key::K, true);
        e->shuttleStop();
        e->shuttleForward(); // K+L tap
        QCOMPARE(e->position(), int64_t(101));
        QVERIFY(!e->isPlaying());
        e->setShuttleKeyHeld(ShuttleControl::Key::J, true);
        e->shuttleReverse(); // K+J: step back, and keep holding J -> slow reverse
        QCOMPARE(e->position(), int64_t(100));
        QVERIFY(waitFor([&] { return e->rate() == -ShuttleControl::kSlowRate; }, 1500));
        e->setShuttleKeyHeld(ShuttleControl::Key::J, false);
        QCOMPARE(e->rate(), 0.0);
        e->setShuttleKeyHeld(ShuttleControl::Key::K, false);
    }

    void playsForwardAndWritesPlayhead()
    {
        Timeline tl;
        tl.setData(solidTimeline(500, 10));
        auto e = makeEngine();
        e->setTimeline(&tl);
        QSignalSpy frames(e.get(), &PlaybackEngine::frameReady);
        e->play();
        QTest::qWait(600); // ~15 frames at 25 fps
        const int64_t pos = e->position();
        QVERIFY2(pos >= 10 && pos <= 20, qPrintable(QString::number(pos)));
        QVERIFY(std::abs(tl.playhead() - pos) <= 2); // throttled write-back
        QVERIFY(frames.count() >= 8);
        e->pause();
        QCOMPARE(tl.playhead(), e->position()); // exact once paused
        // The paused still is re-rendered for exactly that frame.
        QVERIFY(waitFor(
            [&] { return e->currentFrame() && e->currentFrame()->frame == e->position(); }));
        QCOMPARE(qBlue(centre(e->currentFrame())), 255);
        const auto s = e->stats();
        QVERIFY(s.presented >= 8);
        QVERIFY(!s.audioMaster);
        qInfo("presented %lld, dropped %lld, %.1f fps", qlonglong(s.presented),
            qlonglong(s.dropped), s.fps());
    }

    void stopsAtEndAndRestarts()
    {
        Timeline tl;
        tl.setData(solidTimeline(12));
        tl.setPlayhead(6);
        auto e = makeEngine();
        e->setTimeline(&tl);
        e->play();
        QVERIFY(waitFor([&] { return !e->isPlaying(); }, 2000));
        QCOMPARE(e->position(), int64_t(11)); // last frame
        // Play at the end starts over.
        e->play();
        QVERIFY(e->position() <= 1);
        e->setRate(-2.0);
        QVERIFY(waitFor([&] { return !e->isPlaying(); }, 2000));
        QCOMPARE(e->position(), int64_t(0));
    }

    void loopsOverInOut()
    {
        Timeline tl;
        TimelineData d = solidTimeline(200);
        d.markIn = 20;
        d.markOut = 26; // 6 frames = 240 ms
        tl.setData(d);
        tl.setPlayhead(100); // outside: loop playback jumps to the in mark
        auto e = makeEngine();
        e->setTimeline(&tl);
        QSignalSpy loop(e.get(), &PlaybackEngine::loopChanged);
        e->setLoop(true);
        QCOMPARE(loop.count(), 1);
        QVector<int64_t> seen;
        connect(
            e.get(), &PlaybackEngine::positionChanged, this, [&](int64_t f) { seen.push_back(f); });
        e->play();
        QTest::qWait(700); // ~ 3 loops
        QVERIFY(e->isPlaying());
        e->pause();
        QVERIFY(!seen.isEmpty());
        int wraps = 0;
        for (int i = 0; i < seen.size(); ++i) {
            QVERIFY2(seen[i] >= 20 && seen[i] < 26, qPrintable(QString::number(seen[i])));
            if (i > 0 && seen[i] < seen[i - 1]) {
                ++wraps;
            }
        }
        QVERIFY2(wraps >= 2, qPrintable(QString::number(wraps)));
    }

    void playInToOutStopsAtOut()
    {
        Timeline tl;
        TimelineData d = solidTimeline(200);
        d.markIn = 40;
        d.markOut = 46;
        tl.setData(d);
        auto e = makeEngine();
        e->setTimeline(&tl);
        e->playInToOut();
        QCOMPARE(e->position(), int64_t(40));
        QVERIFY(waitFor([&] { return !e->isPlaying(); }, 2000));
        QCOMPARE(e->position(), int64_t(45));
        e->goToIn();
        QCOMPARE(e->position(), int64_t(40));
        e->goToOut();
        QCOMPARE(e->position(), int64_t(45));
        QCOMPARE(tl.playhead(), int64_t(45));
    }

    void stepAndSeconds()
    {
        Timeline tl;
        tl.setData(solidTimeline(100));
        auto e = makeEngine();
        e->setTimeline(&tl);
        e->step(1);
        QCOMPARE(e->position(), int64_t(1));
        e->step(-5);
        QCOMPARE(e->position(), int64_t(0)); // clamped
        e->stepSeconds(1);
        QCOMPARE(e->position(), int64_t(25));
        e->stepSeconds(-1);
        QCOMPARE(e->position(), int64_t(0));
        e->goToEnd();
        QCOMPARE(e->position(), int64_t(99));
        e->step(10);
        QCOMPARE(e->position(), int64_t(99));
        QCOMPARE(tl.playhead(), int64_t(99));
    }

    /// Moving the playhead 60 times without returning to the event loop (a fast scrub) renders
    /// a handful of frames, not 60, and ends on the last position.
    void scrubCoalesces()
    {
        Timeline tl;
        tl.setData(solidTimeline(100, 50));
        auto e = makeEngine();
        e->setTimeline(&tl);
        QVERIFY(waitFor([&] { return e->currentFrame() != nullptr; }));
        const int64_t before = e->scheduler().issuedCount();
        for (int64_t f = 0; f < 60; ++f) {
            tl.setPlayhead(f);
        }
        QVERIFY(waitFor([&] { return e->currentFrame() && e->currentFrame()->frame == 59; }));
        QTest::qWait(50);
        const int64_t issued = e->scheduler().issuedCount() - before;
        qInfo("60 playhead moves -> %lld renders", qlonglong(issued));
        QVERIFY2(issued <= 3, qPrintable(QString::number(issued)));
        QCOMPARE(qBlue(centre(e->currentFrame())), 255);
    }

    /// Clicking / dragging the ruler while playing (alpha.2 feedback 3): the target frame is
    /// shown at once, playback carries on from there, and a burst of moves is coalesced (no
    /// render or sound restart per move).
    void seekWhilePlaying()
    {
        Timeline tl;
        tl.setData(solidTimeline(1000, 500)); // red before 500, blue from 500
        auto e = makeEngine(std::make_unique<test::FakeAudioOutput>(10, 40));
        e->setTimeline(&tl);
        QSignalSpy states(e.get(), &PlaybackEngine::stateChanged);
        e->play();
        QTest::qWait(300);
        // A click: frame 700 on screen quickly, still playing.
        QElapsedTimer timer;
        timer.start();
        tl.setPlayhead(700);
        QCOMPARE(e->position(), int64_t(700));
        QVERIFY(waitFor([&] { return e->currentFrame() && e->currentFrame()->frame >= 700; }));
        const qint64 clickMs = timer.elapsed();
        QCOMPARE(qBlue(centre(e->currentFrame())), 255);
        QTest::qWait(400);
        QVERIFY(e->isPlaying());
        QVERIFY2(
            e->position() > 700 && e->position() < 720, qPrintable(QString::number(e->position())));
        QCOMPARE(states.count(), 1); // Playing only: the seek is no pause/play cycle
        QVERIFY(e->stats().audioMaster); // sound resumed
        // A drag: 60 moves in quick succession (back into the red part).
        const int64_t before = e->scheduler().issuedCount();
        for (int64_t f = 100; f < 160; ++f)
            tl.setPlayhead(f);
        const int64_t issued = e->scheduler().issuedCount() - before;
        QVERIFY2(issued <= 2, qPrintable(QString::number(issued)));
        QCOMPARE(e->position(), int64_t(159));
        QVERIFY(waitFor([&] {
            return e->currentFrame() && e->currentFrame()->frame >= 159
                && e->currentFrame()->frame < 170;
        }));
        QCOMPARE(qRed(centre(e->currentFrame())), 255);
        QTest::qWait(400);
        QVERIFY(e->isPlaying());
        QVERIFY2(
            e->position() > 159 && e->position() < 180, qPrintable(QString::number(e->position())));
        // Engine seek() (scrub bar, go to start) behaves the same and writes the playhead.
        e->seek(600);
        QCOMPARE(tl.playhead(), int64_t(600));
        QVERIFY(waitFor([&] { return e->currentFrame() && e->currentFrame()->frame >= 600; }));
        QVERIFY(e->stats().seeksWhilePlaying >= 62);
        qInfo("click seek -> frame on screen in %lld ms (engine: %.1f ms); 60 drag moves -> "
              "%lld renders",
            qlonglong(clickMs), e->stats().lastSeekMs, qlonglong(issued));
        // Pausing while parked shows exactly the paused frame.
        tl.setPlayhead(300);
        e->pause();
        QVERIFY(waitFor([&] { return e->currentFrame() && e->currentFrame()->frame == 300; }));
        QCOMPARE(e->position(), int64_t(300));
    }

    void rerendersOnEditWhilePaused()
    {
        Timeline tl;
        tl.setData(solidTimeline(100));
        tl.setPlayhead(10);
        auto e = makeEngine();
        e->setTimeline(&tl);
        QVERIFY(
            waitFor([&] { return e->currentFrame() && qRed(centre(e->currentFrame())) == 255; }));
        tl.mutableData().videoTracks[0].clips[0].solidColor = Qt::green;
        tl.notifyChanged();
        QVERIFY(waitFor([&] {
            return qGreen(centre(e->currentFrame())) > 120 && qRed(centre(e->currentFrame())) == 0;
        }));
    }

    void outputSizeFollowsClients()
    {
        Timeline tl;
        tl.setData(solidTimeline(10));
        auto e = makeEngine();
        e->setTimeline(&tl);
        QCOMPARE(e->outputSize(), QSize(320, 180)); // no clients: native
        int a = 0;
        int b = 0;
        e->setOutputClient(&a, QSize(160, 160), false);
        QCOMPARE(e->outputSize(), QSize(160, 90));
        e->setOutputClient(&b, QSize(1000, 1000), false);
        QCOMPARE(e->outputSize(), QSize(320, 180)); // never above native
        e->removeOutputClient(&b);
        QCOMPARE(e->outputSize(), QSize(160, 90));
        QVERIFY(waitFor(
            [&] { return e->currentFrame() && e->currentFrame()->size == QSize(160, 90); }));
        QCOMPARE(e->qualityMode(), PlaybackEngine::QualityMode::Full); // the default
        // Proxy resolutions are fractions of the timeline resolution, capped by the viewer:
        // Half of 320x180 is what this 160x90 viewer shows anyway; Quarter is smaller.
        e->setQualityMode(PlaybackEngine::QualityMode::Half);
        QCOMPARE(e->currentQuality(), Quality::Half);
        QCOMPARE(e->renderSize(), QSize(160, 90));
        e->setQualityMode(PlaybackEngine::QualityMode::Quarter);
        QCOMPARE(e->renderSize(), QSize(80, 45));
        QVERIFY(
            waitFor([&] { return e->currentFrame() && e->currentFrame()->size == QSize(80, 45); }));
        e->setOutputClient(&b, QSize(1000, 1000), false);
        QCOMPARE(e->renderSize(), QSize(80, 45));
        e->setQualityMode(PlaybackEngine::QualityMode::Half);
        QCOMPARE(e->renderSize(), QSize(160, 90));
        e->setQualityMode(PlaybackEngine::QualityMode::Auto);
        QCOMPARE(e->currentQuality(), Quality::Full); // paused: always full
    }

    /// Audio master clock with a fake sound card (10 ms periods, 40 ms latency, crystal 0.1 %
    /// fast): positions are monotonic and stay within one frame of what is heard.
    void audioMasterClock()
    {
        Timeline tl;
        tl.setData(solidTimeline(2000));
        tl.setPlayhead(50);
        auto fake = std::make_unique<test::FakeAudioOutput>(10, 40, 1.001);
        test::FakeAudioOutput* device = fake.get();
        auto e = makeEngine(std::move(fake));
        e->setTimeline(&tl);
        double maxErr = 0;
        int64_t last = -1;
        bool monotonic = true;
        int samples = 0;
        connect(e.get(), &PlaybackEngine::positionChanged, this, [&](int64_t f) {
            if (!e->isPlaying() || device->starts() == 0) {
                return;
            }
            monotonic = monotonic && f >= last;
            last = f;
            const double heard = 50 + device->trueHeardFrames(monotonicUs()) / 48000.0 * 25.0;
            if (heard > 60) { // after the clock settled
                maxErr = std::max(maxErr, std::abs(double(f) - heard));
                ++samples;
            }
        });
        e->play();
        QVERIFY(e->stats().audioMaster);
        QTest::qWait(2000);
        const auto s = e->stats();
        e->pause();
        qInfo("audio master: %d samples, max |position - heard| %.2f frames, last A/V error %.3f "
              "frames, "
              "%lld presented, %lld dropped, underrun frames %lld",
            samples, maxErr, s.avOffsetFrames, qlonglong(s.presented), qlonglong(s.dropped),
            qlonglong(device->underrunFrames()));
        QVERIFY(monotonic);
        QVERIFY(samples > 20);
        // position() is the floor of the clock, so one frame of quantisation plus slack.
        QVERIFY2(maxErr < 1.5, qPrintable(QString::number(maxErr)));
        QVERIFY(s.audioMaster);
        QCOMPARE(device->starts(), 1);
    }

    void mutedUsesWallClock()
    {
        Timeline tl;
        tl.setData(solidTimeline(2000));
        auto fake = std::make_unique<test::FakeAudioOutput>();
        test::FakeAudioOutput* device = fake.get();
        auto e = makeEngine(std::move(fake));
        e->setTimeline(&tl);
        e->setMuted(true);
        e->play();
        QVERIFY(!e->stats().audioMaster);
        QCOMPARE(device->starts(), 0);
        e->setMuted(false); // switches to the device while playing
        QVERIFY(e->stats().audioMaster);
        QCOMPARE(device->starts(), 1);
        // Shuttling is heard too (varispeed, spec 1.16): the device stays the clock.
        e->setRate(2.0);
        QVERIFY(e->stats().audioMaster);
        e->setMuted(true);
        QVERIFY(!e->stats().audioMaster);
        e->setMuted(false);
        e->setRate(1.0);
        QVERIFY(e->stats().audioMaster);
        e->pause();
    }

    /// A device that never reports progress makes the engine fall back to the wall clock.
    void deadDeviceFallsBack()
    {
        struct DeadOutput final : AudioOutput
        {
            bool isAvailable() const override { return true; }
            bool start(AudioRing*) override { return true; }
            void stop() override { }
            void setVolume(float) override { }
            void playedFrames(int64_t* f, int64_t* at) const override
            {
                *f = 0;
                *at = monotonicUs();
            }
            QString description() const override { return QStringLiteral("dead"); }
        };
        Timeline tl;
        tl.setData(solidTimeline(2000));
        auto e = makeEngine(std::make_unique<DeadOutput>());
        e->setTimeline(&tl);
        e->play();
        QTest::qWait(PlaybackEngine::kAudioStartTimeoutMs + 400);
        QVERIFY(!e->stats().audioMaster);
        QVERIFY(e->position() > 3); // moving on the wall clock
        e->pause();
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    int result = 0;
    {
        TestPlaybackEngine test;
        result = QTest::qExec(&test, argc, argv);
    }
    g_compositor.reset();
    return result;
}

#include "test_playbackengine.moc"
