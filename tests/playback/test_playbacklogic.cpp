// SPDX-License-Identifier: GPL-3.0-or-later
// Pure playback logic: J/K/L state machine, playback clock (free and audio-slaved), audio ring,
// frame scheduler (look-ahead, dropping, scrub coalescing).
#include "playback/AudioRing.h"
#include "playback/FrameScheduler.h"
#include "playback/PlaybackClock.h"
#include "playback/ShuttleControl.h"

#include <QTest>

#include <cmath>
#include <random>

using namespace kk;

namespace {

using Cmd = ShuttleControl::Command;
using Key = ShuttleControl::Key;

/// Render function whose requests are completed by the test.
struct FakeRenderer
{
    std::vector<std::pair<int64_t, std::promise<RenderedFramePtr>>> pending;
    FrameScheduler::RenderFn fn()
    {
        return [this](RenderRequest request) {
            pending.emplace_back(request.frame, std::promise<RenderedFramePtr>());
            return pending.back().second.get_future();
        };
    }
    /// Completes the oldest pending request.
    int64_t finishOldest(double renderMs = 1.0)
    {
        auto [frame, promise] = std::move(pending.front());
        pending.erase(pending.begin());
        auto f = std::make_shared<RenderedFrame>();
        f->frame = frame;
        f->ok = true;
        f->renderMs = renderMs;
        promise.set_value(f);
        return frame;
    }
};

} // namespace

class TestPlaybackLogic : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // ---- ShuttleControl ----

    void shuttleRates()
    {
        ShuttleControl s;
        double rate = 0;
        auto press = [&](Key k) {
            const Cmd c = s.trigger(k, rate, 0);
            if (c.kind == Cmd::Kind::SetRate) {
                rate = c.rate;
            }
            return rate;
        };
        QCOMPARE(press(Key::L), 1.0);
        QCOMPARE(press(Key::L), 2.0);
        QCOMPARE(press(Key::L), 4.0);
        QCOMPARE(press(Key::L), 8.0);
        QCOMPARE(press(Key::L), 8.0); // capped
        QCOMPARE(press(Key::J), -1.0); // direction change restarts at 1x
        QCOMPARE(press(Key::J), -2.0);
        QCOMPARE(press(Key::J), -4.0);
        QCOMPARE(press(Key::J), -8.0);
        QCOMPARE(press(Key::J), -8.0);
        QCOMPARE(press(Key::K), 0.0);
        QCOMPARE(press(Key::J), -1.0);
        QCOMPARE(press(Key::L), 1.0);
        QCOMPARE(ShuttleControl::nextForwardRate(0.5), 1.0); // from slow play
    }

    void shuttleKHeldSteps()
    {
        ShuttleControl s;
        QVERIFY(s.setKeyHeld(Key::K, true, 0).kind == Cmd::Kind::None);
        QCOMPARE(s.trigger(Key::K, 2.0, 0).rate, 0.0);
        // Tap L with K held: one frame forward, no slow play after release.
        Cmd c = s.trigger(Key::L, 0.0, 10);
        QVERIFY(c.kind == Cmd::Kind::Step);
        QCOMPARE(c.step, 1);
        s.setKeyHeld(Key::L, true, 10);
        s.setKeyHeld(Key::L, false, 60);
        QVERIFY(s.poll(10 + ShuttleControl::kHoldMs + 1).kind == Cmd::Kind::None);
        // Tap J: one frame back.
        c = s.trigger(Key::J, 0.0, 100);
        QVERIFY(c.kind == Cmd::Kind::Step);
        QCOMPARE(c.step, -1);
        s.setKeyHeld(Key::J, false, 120);
    }

    void shuttleKHeldSlowPlay()
    {
        ShuttleControl s;
        s.setKeyHeld(Key::K, true, 0);
        s.setKeyHeld(Key::L, true, 100);
        QVERIFY(s.trigger(Key::L, 0.0, 100).kind == Cmd::Kind::Step);
        QCOMPARE(s.holdDeadlineMs(), int64_t(100 + ShuttleControl::kHoldMs));
        QVERIFY(s.poll(150).kind == Cmd::Kind::None); // not yet
        Cmd c = s.poll(100 + ShuttleControl::kHoldMs);
        QVERIFY(c.kind == Cmd::Kind::SetRate);
        QCOMPARE(c.rate, ShuttleControl::kSlowRate);
        QVERIFY(s.isSlowPlaying());
        // Releasing L stops the slow play.
        c = s.setKeyHeld(Key::L, false, 900);
        QVERIFY(c.kind == Cmd::Kind::SetRate);
        QCOMPARE(c.rate, 0.0);
        // K + held J: slow reverse, ended by releasing K.
        s.setKeyHeld(Key::J, true, 1000);
        s.trigger(Key::J, 0.0, 1000);
        c = s.poll(1000 + ShuttleControl::kHoldMs);
        QCOMPARE(c.rate, -ShuttleControl::kSlowRate);
        c = s.setKeyHeld(Key::K, false, 1500);
        QVERIFY(c.kind == Cmd::Kind::SetRate);
        QCOMPARE(c.rate, 0.0);
        // Without K, L shuttles again.
        s.setKeyHeld(Key::J, false, 1600);
        QCOMPARE(s.trigger(Key::L, 0.0, 1700).rate, 1.0);
    }

    // ---- PlaybackClock ----

    void freeClock()
    {
        PlaybackClock c;
        c.setFps({25, 1});
        c.startFree(100, 1.0, 0);
        QCOMPARE(c.position(1'000'000), 125.0);
        c.startFree(100, -2.0, 0);
        QCOMPARE(c.position(1'000'000), 50.0);
        c.startFree(10, 0.5, 0);
        QCOMPARE(c.position(2'000'000), 35.0);
        c.stop();
        QCOMPARE(c.position(9'000'000), 35.0); // frozen
        QVERIFY(!c.isRunning());
    }

    void audioClockMonotonicAndBounded_data()
    {
        QTest::addColumn<int>("periodMs");
        QTest::addColumn<double>("drift");
        QTest::addColumn<int>("jitterMs");
        QTest::newRow("10 ms periods") << 10 << 1.0 << 0;
        QTest::newRow("20 ms periods + jitter") << 20 << 1.0 << 6;
        QTest::newRow("40 ms periods, fast device") << 40 << 1.002 << 8;
        QTest::newRow("10 ms periods, slow device") << 10 << 0.998 << 3;
    }

    /// Simulates a device reporting its heard position in steps (one per period, with timing
    /// jitter and a drifting crystal) and samples the clock every 4 ms like the engine does.
    /// The clock must never go backwards and must stay within half a frame of what is heard.
    void audioClockMonotonicAndBounded()
    {
        QFETCH(int, periodMs);
        QFETCH(double, drift);
        QFETCH(int, jitterMs);
        const Rational fps {25, 1};
        PlaybackClock c;
        c.setFps(fps);
        const int64_t startUs = 1'000'000;
        const int64_t latencyUs = 50'000;
        c.startAudio(100, startUs);
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> jitter(0, std::max(0, jitterMs));
        int64_t reported = 0;
        int64_t reportedAt = startUs;
        // The device finishes period k at wall time boundary(k); the driver learns about it up
        // to jitterMs later (that delay is the realistic error source: reports are stale).
        const int64_t periodFrames = 48 * periodMs;
        auto boundary = [&](int64_t k) {
            return startUs + latencyUs
                + int64_t(double(k * periodFrames) / (48000.0 * drift) * 1e6);
        };
        int64_t k = 1;
        int64_t nextReport = boundary(k) + jitter(rng) * 1000;
        double last = -1;
        double maxErr = 0;
        for (int64_t t = startUs; t < startUs + 20'000'000; t += 4000) {
            if (t >= nextReport) {
                reported = k * periodFrames;
                reportedAt = t; // stamped when read, i.e. late
                ++k;
                nextReport = std::max(t, boundary(k) + jitter(rng) * 1000);
            }
            const double pos = c.position(t, reported, reportedAt);
            QVERIFY2(pos >= last, "clock went backwards");
            last = pos;
            const double truth = 100
                + std::max(0.0, double(t - startUs - latencyUs) / 1e6 * drift) * fps.toDouble();
            if (t > startUs + 1'000'000) { // after settling
                maxErr = std::max(maxErr, std::abs(pos - truth));
            }
        }
        qInfo("max |clock - heard| = %.3f frames", maxErr);
        QVERIFY2(maxErr < 0.5, qPrintable(QString::number(maxErr)));
        QVERIFY(c.isAudioMaster());
    }

    void audioClockHoldsUntilDeviceStarts()
    {
        PlaybackClock c;
        c.setFps({25, 1});
        c.startAudio(10, 0);
        QCOMPARE(c.position(200'000, 0, 200'000), 10.0); // nothing heard yet: hold
        // First samples heard at t=200 ms: no jump ahead by the 200 ms start-up time.
        const double p = c.position(220'000, 960, 220'000); // 20 ms heard
        QVERIFY(std::abs(p - 10.5) < 0.01);
    }

    void audioClockSnapsOnLargeError()
    {
        PlaybackClock c;
        c.setFps({25, 1});
        c.startAudio(0, 0);
        c.position(100'000, 4800, 100'000);
        // Device jumped 1 s ahead (e.g. underrun recovery): snap instead of slewing for ages.
        const double p = c.position(110'000, 4800 + 48000 + 480, 110'000);
        QVERIFY(std::abs(p - (0.11 * 25 + 25)) < 0.05);
    }

    // ---- AudioRing ----

    void ringWrapsAndCounts()
    {
        AudioRing ring(100); // rounds to 128
        QCOMPARE(ring.capacity(), size_t(128));
        std::vector<float> in(2 * 100), out(2 * 100);
        for (size_t i = 0; i < in.size(); ++i) {
            in[i] = float(i);
        }
        QCOMPARE(ring.write(in.data(), 100), size_t(100));
        QCOMPARE(ring.read(out.data(), 60), size_t(60));
        QCOMPARE(ring.write(in.data(), 100), size_t(88)); // only 88 free
        QCOMPARE(ring.available(), size_t(128));
        QCOMPARE(ring.read(out.data(), 40), size_t(40));
        QCOMPARE(out[0], 120.0f); // frame 60 of the first write
        QCOMPARE(ring.read(out.data(), 100), size_t(88));
        QCOMPARE(out[0], 0.0f); // start of the second write, across the wrap
        QCOMPARE(out[2 * 87 + 1], 175.0f);
        QCOMPARE(ring.totalRead(), uint64_t(188));
    }

    // ---- FrameScheduler ----

    void schedulerBoundsInFlight()
    {
        FakeRenderer r;
        FrameScheduler s(r.fn());
        s.setMaxInFlight(3);
        RenderParams p;
        p.timeline = std::make_shared<TimelineData>();
        s.setParams(p);
        std::vector<FrameSlot> want;
        for (int64_t f = 0; f < 6; ++f) {
            want.push_back({f, f});
        }
        QCOMPARE(s.prefetch(want), 3);
        QCOMPARE(s.prefetch(want), 0); // full
        QCOMPARE(int(r.pending.size()), 3);
        r.finishOldest();
        QCOMPARE(s.prefetch(want), 1); // one slot free -> frame 3
        QCOMPARE(r.pending.back().first, int64_t(3));
    }

    void schedulerPresentsDueAndDropsLate()
    {
        FakeRenderer r;
        FrameScheduler s(r.fn());
        s.setMaxInFlight(4);
        s.setParams(
            RenderParams {std::make_shared<TimelineData>(), {}, {}, Quality::Full, OutputImage});
        s.prefetch({{10, 10}, {11, 11}, {12, 12}, {13, 13}});
        r.finishOldest(); // 10
        r.finishOldest(); // 11
        QVERIFY(!s.poll(9, 1)); // nothing due yet
        int64_t key = 0;
        RenderedFramePtr f = s.poll(12, 1, &key); // 12 is due but not ready: show 11, drop 10
        QVERIFY(f);
        QCOMPARE(f->frame, int64_t(11));
        QCOMPARE(key, int64_t(11));
        QCOMPARE(s.droppedCount(), int64_t(1));
        r.finishOldest(); // 12
        r.finishOldest(); // 13
        f = s.poll(13, 1);
        QCOMPARE(f->frame, int64_t(13));
        QCOMPARE(s.droppedCount(), int64_t(2)); // 12 was never shown
        QVERIFY(!s.busy());
    }

    void schedulerReverseAndLoopKeys()
    {
        FakeRenderer r;
        FrameScheduler s(r.fn());
        s.setParams(
            RenderParams {std::make_shared<TimelineData>(), {}, {}, Quality::Full, OutputImage});
        // Reverse: due frames decrease.
        s.prefetch({{20, 20}, {19, 19}, {18, 18}});
        r.finishOldest();
        r.finishOldest();
        r.finishOldest();
        QCOMPARE(s.poll(19, -1)->frame, int64_t(19));
        s.clear();
        // Loop: keys keep increasing while the frame wraps (loop [0, 50)).
        s.prefetch({{49, 49}, {50, 0}, {51, 1}});
        r.finishOldest();
        r.finishOldest();
        r.finishOldest();
        int64_t key = 0;
        RenderedFramePtr f = s.poll(50, 1, &key);
        QCOMPARE(key, int64_t(50));
        QCOMPARE(f->frame, int64_t(0));
    }

    /// Scrubbing through 50 frames while each render is slow issues only the first and the
    /// last request, and ends on the last requested frame.
    void schedulerCoalescesStills()
    {
        FakeRenderer r;
        FrameScheduler s(r.fn());
        s.setParams(
            RenderParams {std::make_shared<TimelineData>(), {}, {}, Quality::Full, OutputImage});
        for (int64_t f = 0; f < 50; ++f) {
            s.requestStill(f);
        }
        QCOMPARE(s.issuedCount(), int64_t(1));
        QVERIFY(!s.pollStill()); // not finished
        r.finishOldest();
        RenderedFramePtr shown = s.pollStill();
        QCOMPARE(shown->frame, int64_t(0));
        QCOMPARE(s.issuedCount(), int64_t(2)); // the pending one (49) was issued
        QCOMPARE(r.pending.back().first, int64_t(49));
        r.finishOldest();
        QCOMPARE(s.pollStill()->frame, int64_t(49));
        QVERIFY(!s.busy());
        // Same frame again: nothing to do, unless forced (edit).
        s.requestStill(49);
        QCOMPARE(s.issuedCount(), int64_t(2));
        s.requestStill(49, true);
        QCOMPARE(s.issuedCount(), int64_t(3));
    }

    void schedulerParamChangeDiscards()
    {
        FakeRenderer r;
        FrameScheduler s(r.fn());
        s.setMaxInFlight(2);
        s.setParams(
            RenderParams {std::make_shared<TimelineData>(), {}, {}, Quality::Full, OutputImage});
        s.prefetch({{0, 0}, {1, 1}});
        s.setParams(
            RenderParams {std::make_shared<TimelineData>(), {}, {}, Quality::Half, OutputImage});
        // Discarded requests still occupy the render thread: no new ones until they finish.
        QCOMPARE(s.prefetch({{0, 0}, {1, 1}}), 0);
        r.finishOldest();
        r.finishOldest();
        QVERIFY(!s.poll(5, 1)); // old results are never presented
        QCOMPARE(s.prefetch({{0, 0}, {1, 1}}), 2);
    }
};

QTEST_GUILESS_MAIN(TestPlaybackLogic)
#include "test_playbacklogic.moc"
