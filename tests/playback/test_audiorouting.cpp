// SPDX-License-Identifier: GPL-3.0-or-later
// Output device routing without sound hardware: which device is chosen (Automatic follows the
// system default, a chosen device wins, falls back while missing and comes back), and a device
// switch in the middle of a play (the ring and its contents are kept, the played count and the
// playback clock stay monotonic and in sync with what is heard).
#include "playback/AudioRing.h"
#include "playback/AudioRouting.h"
#include "playback/PlaybackClock.h"

#include <QTest>

#include <deque>

using namespace kk;

namespace {

using Reason = AudioRouteChoice::Reason;

const AudioDeviceInfo kSpeakers {"alsa_output.pci-0000_00_1f.3.analog-stereo", "Speakers"};
const AudioDeviceInfo kAirPods {"bluez_output.AA_BB_CC_DD_EE_FF.1", "Bluetooth Headphones"};
const AudioDeviceInfo kHdmi {"alsa_output.pci-0000_01_00.1.hdmi-stereo", "HDMI"};

/// A simulated sound server stream in pull mode: a buffer of @p bufferFrames frames, played at
/// 48 frames per ms after @p startupMs of start-up latency, refilled from the RingPuller.
/// It remembers which ring frame each queued device frame carries (left sample = frame index).
struct FakeStream
{
    int bufferFrames = 2880; // 60 ms, like kDeviceBufferMs
    int startupMs = 0;
    int64_t processed = 0;
    std::deque<int64_t> queued; ///< ring frame indices in the device buffer
    std::vector<int64_t> heard; ///< ring frame indices in the order they were played
    std::vector<float> scratch;

    /// One millisecond of device time.
    void tick(RingPuller& puller)
    {
        if (startupMs > 0) {
            --startupMs;
            return;
        }
        for (int i = 0; i < 48 && !queued.empty(); ++i) {
            heard.push_back(queued.front());
            queued.pop_front();
            ++processed;
        }
        const size_t want = size_t(bufferFrames) - queued.size();
        scratch.resize(want * 2);
        puller.pull(reinterpret_cast<char*>(scratch.data()), want);
        for (size_t i = 0; i < want; ++i) {
            queued.push_back(int64_t(scratch[i * 2])); // silence (underrun) shows as 0
        }
    }
};

/// Producer side: keeps the ring filled with frames whose samples are their own index.
struct Producer
{
    int64_t next = 1; // frame index 0 would look like silence
    std::vector<float> buffer;
    void fill(AudioRing& ring)
    {
        const size_t space = ring.space();
        buffer.resize(space * 2);
        for (size_t i = 0; i < space; ++i) {
            buffer[i * 2] = float(next + int64_t(i));
            buffer[i * 2 + 1] = 0.0f;
        }
        next += int64_t(ring.write(buffer.data(), space));
    }
};

} // namespace

class TestAudioRouting : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void automaticFollowsTheSystemDefault()
    {
        const std::vector<AudioDeviceInfo> both {kSpeakers, kAirPods};
        AudioRouteChoice choice = chooseAudioOutput({}, {kSpeakers}, kSpeakers.id);
        QCOMPARE(choice.id, kSpeakers.id);
        QCOMPARE(choice.reason, Reason::SystemDefault);
        // AirPods connected: the desktop makes them the default, Automatic follows.
        choice = chooseAudioOutput({}, both, kAirPods.id);
        QCOMPARE(choice.id, kAirPods.id);
        QCOMPARE(choice.reason, Reason::SystemDefault);
        // Switched back to the speakers in the desktop's audio applet.
        choice = chooseAudioOutput({}, both, kSpeakers.id);
        QCOMPARE(choice.id, kSpeakers.id);
        // AirPods removed while they were the default, before the server names a new default.
        choice = chooseAudioOutput({}, {kSpeakers}, kAirPods.id);
        QCOMPARE(choice.id, kSpeakers.id);
    }

    void chosenDeviceWins()
    {
        const std::vector<AudioDeviceInfo> all {kSpeakers, kAirPods, kHdmi};
        const AudioRouteChoice choice = chooseAudioOutput(kHdmi.id, all, kAirPods.id);
        QCOMPARE(choice.id, kHdmi.id);
        QCOMPARE(choice.reason, Reason::Chosen);
    }

    void missingChosenDeviceFallsBackAndComesBack()
    {
        // Chosen AirPods, switched off: the system default plays.
        AudioRouteChoice choice = chooseAudioOutput(kAirPods.id, {kSpeakers, kHdmi}, kHdmi.id);
        QCOMPARE(choice.id, kHdmi.id);
        QCOMPARE(choice.reason, Reason::ChosenMissing);
        // Connected again (the system default stays HDMI): back to the AirPods.
        choice = chooseAudioOutput(kAirPods.id, {kSpeakers, kHdmi, kAirPods}, kHdmi.id);
        QCOMPARE(choice.id, kAirPods.id);
        QCOMPARE(choice.reason, Reason::Chosen);
    }

    void noDevices()
    {
        AudioRouteChoice choice = chooseAudioOutput(kAirPods.id, {}, {});
        QVERIFY(choice.id.isEmpty());
        QCOMPARE(choice.reason, Reason::None);
        // A default reported before the list is still used.
        choice = chooseAudioOutput({}, {}, kSpeakers.id);
        QCOMPARE(choice.id, kSpeakers.id);
    }

    void trackerNeverGoesBackwards()
    {
        PlayedFramesTracker tracker;
        QCOMPARE(tracker.update(1000, 2880, 4000), int64_t(1000));
        tracker.beginStream(4000);
        // The new stream reports 0 played and holds nothing yet: the count holds.
        QCOMPARE(tracker.update(0, 0, 4000), int64_t(1000));
        // Its first frames are ring frames 4000.. (the 1000..3999 the old device held are lost).
        QCOMPARE(tracker.update(480, 2400, 6880), int64_t(4480));
        tracker.reset();
        QCOMPARE(tracker.played(), int64_t(0));
        QCOMPARE(tracker.streamBase(), int64_t(0));
    }

    /// Plays 3 s of simulated device time with a switch to a slower-starting device (Bluetooth
    /// takes longer to start) after 1 s: the ring is kept (the new device continues at the next
    /// ring frame), the played count stays monotonic and equal to the ring frame being heard,
    /// and the clock never goes backwards and ends in sync.
    void switchKeepsTheRingAndTheClock()
    {
        AudioRing ring(8192);
        Producer producer;
        producer.fill(ring);
        RingPuller puller(&ring, false);
        PlayedFramesTracker tracker;
        PlaybackClock clock;
        clock.setFps(Rational {25, 1});
        clock.startAudio(0.0, 0, 1.0);
        tracker.reset();
        tracker.beginStream(0);

        auto stream = std::make_unique<FakeStream>();
        std::vector<int64_t> heardAll;
        int64_t lastPlayed = 0;
        int64_t lastPlayedUs = 0;
        double lastPosition = 0.0;
        int64_t lastPulledBeforeSwitch = 0;
        int64_t firstPulledAfterSwitch = -1;
        constexpr int kSwitchMs = 1000;
        for (int ms = 1; ms <= 3000; ++ms) {
            if (ms == kSwitchMs) {
                // Device switch: the old stream and whatever it buffered go away.
                lastPulledBeforeSwitch = stream->queued.empty() ? 0 : stream->queued.back();
                heardAll.insert(heardAll.end(), stream->heard.begin(), stream->heard.end());
                stream = std::make_unique<FakeStream>();
                stream->startupMs = 120;
                tracker.beginStream(puller.pulled());
            }
            stream->tick(puller);
            if (ms >= kSwitchMs && firstPulledAfterSwitch < 0 && !stream->queued.empty()) {
                firstPulledAfterSwitch = stream->queued.front();
            }
            producer.fill(ring);
            const int64_t buffered = int64_t(stream->queued.size());
            const int64_t played = tracker.update(stream->processed, buffered, puller.pulled());
            QVERIFY2(played >= lastPlayed, "played count went backwards");
            if (played > lastPlayed) {
                lastPlayed = played;
                lastPlayedUs = int64_t(ms) * 1000;
            }
            // Sync: the latest heard frame is ring frame played - 1 (frame indices start at 1
            // in the producer, so it is the frame index == played).
            if (!stream->heard.empty() && played > 0) {
                QCOMPARE(stream->heard.back(), played);
            }
            const double position = clock.position(int64_t(ms) * 1000, lastPlayed, lastPlayedUs);
            QVERIFY2(position >= lastPosition, "playback clock went backwards");
            lastPosition = position;
        }
        // The ring was not cleared: the new device starts with the frame after the last one
        // the old device took.
        QCOMPARE(firstPulledAfterSwitch, lastPulledBeforeSwitch + 1);
        QCOMPARE(puller.skipDebt(), int64_t(0));
        // In sync at the end: the clock is within a frame of what is heard.
        QVERIFY2(std::abs(clock.lastAudioError()) < 1.0,
            qPrintable(QString::number(clock.lastAudioError())));
        // Steps forwards over at most the old device's buffer (60 ms = 1.5 frames at 25 fps)
        // plus the new one's start-up: the end position is close to the wall clock.
        QVERIFY(std::abs(lastPosition - 3000.0 / 40.0) < 4.0);
    }

    void int16AfterSwitch()
    {
        AudioRing ring(1024);
        std::vector<float> data(512 * 2, 0.5f);
        ring.write(data.data(), 512);
        RingPuller puller(&ring, false);
        std::vector<float> f(256 * 2);
        puller.pull(reinterpret_cast<char*>(f.data()), 256);
        QCOMPARE(f[0], 0.5f);
        // The next device rejects float: the same puller continues in int16.
        puller.setInt16(true);
        QCOMPARE(puller.bytesPerFrame(), 4);
        std::vector<int16_t> s(256 * 2);
        puller.pull(reinterpret_cast<char*>(s.data()), 256);
        QCOMPARE(s[0], int16_t(16383));
        QCOMPARE(puller.pulled(), int64_t(512));
        QCOMPARE(ring.available(), size_t(0));
    }
};

QTEST_GUILESS_MAIN(TestAudioRouting)
#include "test_audiorouting.moc"
