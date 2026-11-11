// SPDX-License-Identifier: GPL-3.0-or-later
// Pure playback logic: J/K/L state machine, playback clock (free and audio-slaved), audio ring,
// frame scheduler (look-ahead, dropping, scrub coalescing), Auto proxy resolution.
#include "playback/AudioRing.h"
#include "playback/AutoQuality.h"
#include "playback/FrameScheduler.h"
#include "playback/PlaybackClock.h"
#include "playback/ShuttleControl.h"

#include <QTest>

#include <atomic>
#include <cmath>
#include <random>
#include <set>

using namespace kk;

namespace {

using Cmd = ShuttleControl::Command;
using Key = ShuttleControl::Key;

/// Render function whose requests are completed by the test.
struct FakeRenderer
{
    std::vector<std::pair<int64_t, std::promise<RenderedFramePtr>>> pending;
    std::vector<std::shared_ptr<const std::atomic<bool>>> cancelFlags; ///< by issue order
    FrameScheduler::RenderFn fn()
    {
        return [this](RenderRequest request) {
            cancelFlags.push_back(request.cancelled);
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

/// A render thread with a decoder that cannot skip: a request for frame f costs decodeMs for
/// every frame since the last decoded one, plus otherMs (upload, composite, output); requests
/// run one after the other in virtual time. Drives FrameScheduler + lookAheadKeys() like
/// PlaybackEngine::onTick() does and returns the frames shown per second over @p seconds.
double simulatePlayback(double fps, double decodeMs, double otherMs, bool sequential, int seconds)
{
    struct Job
    {
        double finishMs;
        std::promise<RenderedFramePtr> promise;
        RenderedFramePtr frame;
    };
    std::vector<Job> jobs;
    double now = 0.0, busyUntil = 0.0;
    int64_t decoded = -1;
    std::set<int64_t> repeated; ///< catch-up: frames nothing refers to, shown as the one before
    FrameScheduler scheduler([&](RenderRequest request) {
        const int64_t f = request.frame;
        const double start = std::max(now, busyUntil);
        const int64_t frames = std::max<int64_t>(1, f - decoded);
        // Catch-up: the frames nothing refers to are not decoded (about half of them).
        const double decodeCost
            = request.catchUp ? decodeMs * 0.5 * double(frames) : decodeMs * double(frames);
        busyUntil = start + decodeCost + otherMs;
        decoded = std::max(decoded, f);
        if (request.catchUp && f % 2 == 1)
            repeated.insert(f);
        Job job;
        job.finishMs = busyUntil;
        auto frame = std::make_shared<RenderedFrame>();
        frame->frame = f;
        frame->ok = true;
        frame->renderMs = busyUntil - start;
        job.frame = frame;
        auto future = job.promise.get_future();
        jobs.push_back(std::move(job));
        return future;
    });
    const double intervalMs = 1000.0 / fps;
    int64_t lastShown = -1;
    bool haveShown = false;
    DisplayLag lag;
    int64_t shownCount = 0;
    for (now = 0.0; now < seconds * 1000.0; now += 1.0) {
        for (auto it = jobs.begin(); it != jobs.end();) {
            if (it->finishMs <= now) {
                it->promise.set_value(it->frame);
                it = jobs.erase(it);
            } else {
                ++it;
            }
        }
        if (std::fmod(now, 4.0) != 0.0)
            continue;
        const double unwrapped = now / intervalMs;
        const int64_t due = int64_t(std::floor(unwrapped));
        const double avg = scheduler.averageRenderMs();
        LookAhead look;
        look.unwrapped = unwrapped;
        look.dueKey = due - (sequential ? lag.frames() : 0);
        look.lead = avg > 0.5 * intervalMs
            ? std::min(10, int(std::ceil(avg * scheduler.maxInFlight() / intervalMs)))
            : 0;
        look.count = scheduler.maxInFlight() + 2;
        look.sequential = sequential;
        look.maxLateFrames = int64_t(std::ceil(110.0 / intervalMs));
        look.haveNewest = scheduler.newestRequestedKey(1, &look.newest);
        if (haveShown && (!look.haveNewest || lastShown > look.newest)) {
            look.newest = lastShown;
            look.haveNewest = true;
        }
        std::vector<FrameSlot> wanted;
        bool catchUp = false;
        for (const int64_t key : lookAheadKeys(look, &catchUp)) {
            if (haveShown && key <= lastShown)
                continue;
            wanted.push_back({key, key, catchUp});
        }
        scheduler.prefetch(wanted);
        int64_t key = 0;
        const int64_t displayKey = due - (sequential ? lag.frames() : 0);
        const bool shown = bool(scheduler.poll(displayKey, 1, &key));
        if (sequential)
            lag.update(!shown && haveShown && displayKey > lastShown, int64_t(now * 1000.0),
                int(55.0 / intervalMs));
        if (shown) {
            lastShown = key;
            haveShown = true;
            if (repeated.count(key) == 0)
                ++shownCount; // a new picture (not the repeat of a skipped frame)
        }
    }
    return double(shownCount) / seconds;
}

} // namespace

class TestPlaybackLogic : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // ---- Frame rate with a decoder nearly as slow as the clock ----

    void slowDecoderKeepsItsThroughput()
    {
        // Per-frame cost (decode + other) against the 16.7 ms interval of 60 fps; the decoder
        // cannot skip. Without the sequential look-ahead the shown rate collapsed (model: 15 fps
        // at 16 ms, 2 fps at 32 ms).
        struct Case
        {
            double decodeMs, otherMs;
        };
        for (const Case c : {Case {8, 3}, Case {12, 3}, Case {14, 3}, Case {16, 3}, Case {20, 3},
                 Case {26, 3}, Case {32, 3}}) {
            const double throughput = std::min(60.0, 1000.0 / (c.decodeMs + c.otherMs));
            const double before = simulatePlayback(60.0, c.decodeMs, c.otherMs, false, 8);
            const double after = simulatePlayback(60.0, c.decodeMs, c.otherMs, true, 8);
            qInfo(
                "decode %.0f + %.0f ms: throughput %.1f fps, shown %.1f fps before, %.1f fps after",
                c.decodeMs, c.otherMs, throughput, before, after);
            // The picture keeps the decoder's pace (the catch-up skips the frames nothing refers to
            // while it is behind); when the decoder is slower than half the clock the model
            // jumps to the clock now and then, still far from a slideshow.
            const double bar = throughput >= 34.0 ? 0.8 : 0.5;
            QVERIFY2(after >= bar * throughput,
                qPrintable(QString("decode %1 ms: %2 fps shown of %3")
                        .arg(c.decodeMs)
                        .arg(after)
                        .arg(throughput)));
            QVERIFY2(after >= before, "never worse than before");
        }
    }

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

    void schedulerClearCancels()
    {
        FakeRenderer r;
        FrameScheduler s(r.fn());
        s.setParams(
            RenderParams {std::make_shared<TimelineData>(), {}, {}, Quality::Full, OutputImage});
        s.prefetch({{0, 0}, {1, 1}, {2, 2}});
        QCOMPARE(int(r.cancelFlags.size()), 3);
        for (const auto& flag : r.cancelFlags)
            QVERIFY(flag && !flag->load());
        s.clear(); // a seek: the look-ahead is stale
        for (const auto& flag : r.cancelFlags)
            QVERIFY(flag->load());
        s.requestStill(40);
        QCOMPARE(int(r.cancelFlags.size()), 4);
        QVERIFY(!r.cancelFlags.back()->load()); // new requests are not cancelled
        // A size/quality change keeps the frames in flight (same picture): not cancelled.
        s.prefetch({{41, 41}});
        s.setParams(
            RenderParams {std::make_shared<TimelineData>(), {}, {}, Quality::Half, OutputImage},
            false);
        QVERIFY(!r.cancelFlags.back()->load());
        while (!r.pending.empty())
            r.finishOldest();
    }

    // ---- AutoQuality ----

    /// Feeds @p seconds of observations every 100 ms of a renderer that needs @p fullMs per
    /// frame at Full and @p halfMs at Half (60 fps); returns the quality at the end.
    static Quality feed(AutoQuality& q, int64_t& now, double seconds, double fullMs, double halfMs)
    {
        for (int i = 0; i < int(std::lround(seconds * 10)); ++i) {
            now += 100'000;
            q.update(now, q.quality() == Quality::Full ? fullMs : halfMs, 1000.0 / 60.0);
        }
        return q.quality();
    }

    void autoQualityStaysFullWhenItKeepsUp()
    {
        AutoQuality q;
        int64_t now = 1'000'000;
        q.reset(now);
        QCOMPARE(feed(q, now, 30, 14.0, 5.0), Quality::Full); // 84 % of a 60 fps interval
    }

    void autoQualityNeedsSustainedOverload()
    {
        AutoQuality q;
        int64_t now = 1'000'000;
        q.reset(now);
        // Two overloaded windows (a decoder opening, a seek) followed by recovery: no change.
        QCOMPARE(feed(q, now, 2.5, 25.0, 9.0), Quality::Full);
        QCOMPARE(feed(q, now, 5, 10.0, 9.0), Quality::Full);
        // Sustained overload: Half after three overloaded windows, never Quarter.
        QCOMPARE(feed(q, now, 2.4, 25.0, 9.0), Quality::Full);
        QCOMPARE(feed(q, now, 1.1, 25.0, 9.0), Quality::Half);
        QCOMPARE(feed(q, now, 60, 25.0, 9.0), Quality::Half); // Half helps; load 0.54 stays
    }

    void autoQualityRecoversQuickly()
    {
        AutoQuality q;
        int64_t now = 1'000'000;
        q.reset(now);
        QCOMPARE(feed(q, now, 3, 25.0, 9.0), Quality::Half);
        // The content got lighter: back to Full after 3 s (settle + 2 s of headroom).
        QCOMPARE(feed(q, now, 3, 6.0, 3.0), Quality::Full);
        // Full fails again right away: Half, and the next upshift waits the back-off (5 s).
        QCOMPARE(feed(q, now, 3, 25.0, 9.0), Quality::Half);
        QCOMPARE(feed(q, now, 4.5, 6.0, 3.0), Quality::Half);
        QCOMPARE(feed(q, now, 1.0, 6.0, 3.0), Quality::Full);
    }

    void autoQualityGivesUpWhenHalfDoesNotHelp()
    {
        AutoQuality q;
        int64_t now = 1'000'000;
        q.reset(now);
        // Decode-bound: Full 25 ms, Half 23 ms -> pointless, back to Full for this play.
        QCOMPARE(feed(q, now, 3, 25.0, 23.0), Quality::Half);
        QCOMPARE(feed(q, now, 1.0, 25.0, 23.0), Quality::Full);
        QCOMPARE(feed(q, now, 30, 25.0, 23.0), Quality::Full);
        // A new play starts over.
        q.reset(now);
        QCOMPARE(feed(q, now, 3, 25.0, 23.0), Quality::Half);
    }
};

QTEST_GUILESS_MAIN(TestPlaybackLogic)
#include "test_playbacklogic.moc"
