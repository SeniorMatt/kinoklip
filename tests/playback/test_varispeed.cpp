// SPDX-License-Identifier: GPL-3.0-or-later
// Audio while shuttling (spec 1.16) and audio scrubbing (spec 23.7): AudioFeeder varispeed
// resampling against a direct AudioMixer reference (mono_ramp_48k.wav: every sample value
// tells its own source position), the rate-aware audio clock, and the engine's scrub
// snippets with a fake sound device.
#include "../render/RenderTestUtil.h"
#include "PlaybackTestUtil.h"
#include "core/Timeline.h"
#include "playback/AudioFeeder.h"
#include "playback/AudioRing.h"
#include "playback/PlaybackClock.h"
#include "playback/PlaybackEngine.h"
#include "playback/VarispeedResampler.h"
#include "render/AudioMixer.h"
#include "render/Compositor.h"

#include <QElapsedTimer>
#include <QTest>

#include <cmath>
#include <vector>

using namespace kk;
using namespace kk::test;

namespace {

const QString kRamp = QStringLiteral("ramp");
constexpr int kRampPeriod = 4800; ///< mono_ramp_48k.wav: a sawtooth with this period
constexpr int kCh = AudioRing::kChannels;

/// A 25 fps timeline with the 5 s ramp on A1 from frame 0.
TimelineData rampTimeline()
{
    TimelineData tl = emptyTimeline();
    addClip(tl, TrackType::Audio, 0, mediaClip(tl, kRamp, 0, 125, 0, 5'000'000));
    return tl;
}

/// Reads @p frames from the ring the feeder fills (waits up to 2 s).
std::vector<float> readRing(AudioFeeder& feeder, AudioRing& ring, size_t frames)
{
    std::vector<float> out(frames * kCh);
    (void)feeder.waitForFill(frames, 2000);
    const size_t got = ring.read(out.data(), frames);
    out.resize(got * kCh);
    return out;
}

/// One compositor for all cases (released in main() before the application object).
std::shared_ptr<Compositor> g_compositor;

/// An engine with the fake sound device @p device.
std::unique_ptr<PlaybackEngine> makeEngine(std::unique_ptr<AudioOutput> device)
{
    if (!g_compositor)
        g_compositor = std::make_shared<Compositor>();
    PlaybackEngine::Options options;
    options.compositor = g_compositor;
    options.audio = std::move(device);
    return std::make_unique<PlaybackEngine>(std::move(options));
}

} // namespace

class TestVarispeed : public QObject
{
    Q_OBJECT

    QString m_skip;
    std::shared_ptr<MediaPathMap> m_paths;

private slots:
    void initTestCase()
    {
        if (!ensureFixtures(&m_skip))
            qWarning("%s", qPrintable(m_skip));
        m_paths = std::make_shared<MediaPathMap>();
        m_paths->insert(kRamp, fixture("mono_ramp_48k.wav"));
    }

    /// Output frame k at rate r carries timeline sample start + r * k: forwards fast,
    /// backwards, and slow. The band-limited resampler passes the ramp exactly; frames whose
    /// kernel reaches one of the sawtooth's jumps (every 4800 samples) are skipped.
    void spec1_16_feederResamplesAtTheRate_data()
    {
        QTest::addColumn<double>("rate");
        QTest::addColumn<qint64>("start");
        QTest::newRow("2x") << 2.0 << qint64(10'000);
        QTest::newRow("8x") << 8.0 << qint64(3'000);
        QTest::newRow("reverse 1x") << -1.0 << qint64(100'000);
        QTest::newRow("reverse 4x") << -4.0 << qint64(150'000);
        QTest::newRow("slow 0.5x") << 0.5 << qint64(20'000);
    }
    void spec1_16_feederResamplesAtTheRate()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        QFETCH(double, rate);
        QFETCH(qint64, start);
        const TimelineData tl = rampTimeline();
        // Reference: the plain mix of the whole range the test touches.
        AudioMixer mixer;
        mixer.setMediaPaths(m_paths);
        constexpr int kFrames = 4096;
        const int64_t lo = rate > 0 ? start : start + int64_t(rate * kFrames) - 2;
        const int count = int(std::abs(rate) * kFrames) + 4;
        std::vector<float> ref(size_t(count) * kCh);
        mixer.mix(tl, lo, count, ref.data());

        AudioRing ring(16384);
        AudioFeeder feeder(&ring);
        feeder.setMediaPaths(m_paths);
        feeder.start(snap(tl), start, {}, rate);
        const std::vector<float> got = readRing(feeder, ring, kFrames);
        feeder.stop();
        QCOMPARE(int(got.size()), kFrames * kCh);
        const double reach = VarispeedResampler::margin(rate) + 1;
        int checked = 0;
        for (int k = 0; k < kFrames; ++k) {
            const double pos = double(start) + rate * k;
            const double toJump = std::fmod(pos, double(kRampPeriod));
            if (toJump < reach || toJump > kRampPeriod - reach)
                continue;
            ++checked;
            const double i = pos - double(lo);
            const int i0 = int(std::floor(i));
            const double frac = i - i0;
            const double expected = ref[size_t(i0) * kCh] * (1 - frac)
                + (frac > 0 ? ref[size_t(i0 + 1) * kCh] * frac : 0.0);
            if (std::abs(got[size_t(k) * kCh] - expected) > 1e-4)
                QFAIL(qPrintable(QStringLiteral("frame %1: %2, expected %3")
                                     .arg(k)
                                     .arg(got[size_t(k) * kCh])
                                     .arg(expected)));
        }
        QVERIFY(checked > kFrames / 2);
    }

    /// Shuttling at 4x and 8x does not alias (spec 1.16): a tone above the output Nyquist
    /// frequency (half of 48 kHz / rate) is removed instead of folding back into the audible
    /// band, while a tone well below it passes. Linear interpolation, the old resampler, lets
    /// the high tone through almost unchanged.
    void spec1_16_shuttleDoesNotAlias_data()
    {
        QTest::addColumn<double>("rate");
        QTest::newRow("4x") << 4.0;
        QTest::newRow("8x") << 8.0;
        QTest::newRow("reverse 4x") << -4.0;
    }
    void spec1_16_shuttleDoesNotAlias()
    {
        QFETCH(double, rate);
        const double nyquistOut = 0.5 / std::abs(rate); // cycles per source sample
        const VarispeedResampler resampler;
        constexpr int kOut = 2048;
        const int margin = VarispeedResampler::margin(rate);
        const int spanFrames = int(std::abs(rate) * kOut) + 2 * margin + 2;
        const double pos0 = rate > 0 ? margin : spanFrames - 1 - margin;
        auto rmsOf = [&](double cyclesPerSample, bool linear) {
            std::vector<float> span(size_t(spanFrames) * kCh);
            for (int i = 0; i < spanFrames; ++i)
                for (int c = 0; c < kCh; ++c)
                    span[size_t(i) * kCh + c]
                        = float(std::sin(2 * M_PI * cyclesPerSample * i + 0.3 * c));
            std::vector<float> out(size_t(kOut) * kCh);
            if (linear) {
                for (int k = 0; k < kOut; ++k) {
                    const double p = pos0 + rate * k;
                    const int i = int(std::floor(p));
                    const float f = float(p - i);
                    for (int c = 0; c < kCh; ++c)
                        out[size_t(k) * kCh + c] = span[size_t(i) * kCh + c] * (1 - f)
                            + span[size_t(i + 1) * kCh + c] * f;
                }
            } else {
                resampler.process(span.data(), spanFrames, pos0, rate, kOut, out.data());
            }
            double sum = 0;
            for (float v : out)
                sum += double(v) * v;
            return std::sqrt(sum / double(out.size()));
        };
        // A tone at 1.5x the output Nyquist frequency: would alias, must be gone (< -40 dB).
        const double high = 1.5 * nyquistOut;
        const double filtered = rmsOf(high, false);
        QVERIFY2(filtered < 0.007, qPrintable(QString::number(filtered)));
        QVERIFY(rmsOf(high, true) > 0.1); // what linear interpolation did
        // A tone at 0.3x the output Nyquist frequency passes (RMS of a unit sine: 0.707).
        const double pass = rmsOf(0.3 * nyquistOut, false);
        QVERIFY2(std::abs(pass - M_SQRT1_2) < 0.03, qPrintable(QString::number(pass)));
    }

    /// Reverse play before the timeline start is silence, not a crash.
    void spec1_16_reverseIntoTheStartIsSilent()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        AudioRing ring(16384);
        AudioFeeder feeder(&ring);
        feeder.setMediaPaths(m_paths);
        feeder.start(snap(rampTimeline()), 100, {}, -2.0);
        const std::vector<float> got = readRing(feeder, ring, 2048);
        feeder.stop();
        QCOMPARE(int(got.size()), 2048 * kCh);
        for (size_t k = 200; k < 2048; ++k)
            QCOMPARE(got[k * kCh], 0.0f);
    }

    /// Varispeed in a loop wraps at the loop boundary in the playing direction.
    void spec1_16_varispeedLoopWraps()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        AudioRing ring(16384);
        AudioFeeder feeder(&ring);
        feeder.setMediaPaths(m_paths);
        const SampleLoop loop {48'000, 52'000};
        feeder.start(snap(rampTimeline()), 51'000, loop, 2.0);
        const std::vector<float> got = readRing(feeder, ring, 1024);
        feeder.stop();
        AudioMixer mixer;
        mixer.setMediaPaths(m_paths);
        std::vector<float> ref(4000 * kCh);
        mixer.mix(rampTimeline(), 48'000, 4000, ref.data());
        // Frame 600 is 1200 samples after 51000 = 52200, wrapped to 48200.
        QVERIFY(std::abs(got[600 * kCh] - ref[200 * kCh]) < 1e-4);
    }

    /// The audio clock follows the device at any rate and direction.
    void spec1_16_audioClockAtRate()
    {
        PlaybackClock clock;
        clock.setFps({25, 1});
        clock.startAudio(100.0, 0, 2.0);
        QCOMPARE(clock.position(10'000, 0, 10'000), 100.0); // device not started: hold
        // 48000 frames heard at 2x = 2 s of timeline = 50 frames.
        const double p = clock.position(1'010'000, 48'000, 1'010'000);
        QVERIFY2(std::abs(p - 150.0) < 1.0, qPrintable(QString::number(p)));
        PlaybackClock back;
        back.setFps({25, 1});
        back.startAudio(500.0, 0, -1.0);
        (void)back.position(10'000, 1, 10'000);
        const double q = back.position(1'010'000, 48'000, 1'010'000);
        QVERIFY2(std::abs(q - 475.0) < 1.0, qPrintable(QString::number(q)));
        QVERIFY(back.position(1'020'000, 48'000, 1'010'000) <= q); // never forwards
    }

    /// Shuttling keeps the device as the clock (sound at every rate).
    void spec1_16_shuttleIsHeard()
    {
        Timeline tl;
        tl.setData(rampTimeline());
        auto fake = std::make_unique<FakeAudioOutput>();
        FakeAudioOutput* device = fake.get();
        auto engine = makeEngine(std::move(fake));
        PlaybackEngine& e = *engine;
        e.setTimeline(&tl);
        e.setRate(2.0);
        QVERIFY(e.stats().audioMaster);
        QCOMPARE(device->starts(), 1);
        e.setRate(-1.0);
        QVERIFY(e.stats().audioMaster);
        e.setRate(0.5);
        QVERIFY(e.stats().audioMaster);
        QTRY_VERIFY_WITH_TIMEOUT(e.position() > 0, 3000);
        e.pause();
    }

    // ---- spec 23.7: audio scrubbing ----

    void spec23_7_scrubPlaysSnippetsThenStops()
    {
        PlaybackEngine::setAudioScrubbing(true);
        Timeline tl;
        tl.setData(rampTimeline());
        auto fake = std::make_unique<FakeAudioOutput>();
        FakeAudioOutput* device = fake.get();
        auto engine = makeEngine(std::move(fake));
        PlaybackEngine& e = *engine;
        e.setTimeline(&tl);
        e.seek(10);
        QVERIFY(e.isScrubbingAudio());
        QCOMPARE(device->starts(), 1);
        tl.setPlayhead(20); // a timeline scrub
        tl.setPlayhead(21);
        QCOMPARE(device->starts(), 1); // one device start per gesture
        QTRY_VERIFY_WITH_TIMEOUT(!e.isScrubbingAudio(), 2000);
        // Playing ends scrubbing at once.
        e.seek(30);
        QVERIFY(e.isScrubbingAudio());
        e.play();
        QVERIFY(!e.isScrubbingAudio());
        QVERIFY(e.stats().audioMaster);
        e.pause();
    }

    void spec23_7_scrubbingOffOrMutedIsSilent()
    {
        Timeline tl;
        tl.setData(rampTimeline());
        auto fake = std::make_unique<FakeAudioOutput>();
        FakeAudioOutput* device = fake.get();
        auto engine = makeEngine(std::move(fake));
        PlaybackEngine& e = *engine;
        e.setTimeline(&tl);
        PlaybackEngine::setAudioScrubbing(false);
        e.seek(10);
        QVERIFY(!e.isScrubbingAudio());
        PlaybackEngine::setAudioScrubbing(true);
        e.setMuted(true);
        e.seek(20);
        QVERIFY(!e.isScrubbingAudio());
        QCOMPARE(device->starts(), 0);
    }

    /// The feeder's scrub mode: a faded snippet from the target, then silence, with the
    /// ring kept short.
    void spec23_7_feederScrubSnippet()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        AudioRing ring(16384);
        AudioFeeder feeder(&ring);
        feeder.setMediaPaths(m_paths);
        feeder.startScrub(snap(rampTimeline()));
        QVERIFY(feeder.isScrubbing());
        feeder.scrubTo(24'100); // ramp value != 0 there
        std::vector<float> got;
        // Drain like a device would, until the snippet and some silence have passed.
        QElapsedTimer timer;
        timer.start();
        while (got.size() < size_t(AudioFeeder::kScrubSnippetFrames + 2048) * kCh
            && timer.elapsed() < 3000) {
            QVERIFY(ring.available() <= AudioFeeder::kScrubFillFrames + AudioFeeder::kChunkFrames);
            std::vector<float> part(512 * kCh);
            const size_t n = ring.read(part.data(), 512);
            got.insert(got.end(), part.begin(), part.begin() + long(n * kCh));
            QTest::qWait(2);
        }
        feeder.stop();
        AudioMixer mixer;
        mixer.setMediaPaths(m_paths);
        std::vector<float> ref(size_t(AudioFeeder::kScrubSnippetFrames) * kCh);
        mixer.mix(rampTimeline(), 24'100, AudioFeeder::kScrubSnippetFrames, ref.data());
        // Find the snippet start (the ring may begin with silence written before scrubTo()).
        size_t first = 0;
        while (first < got.size() / kCh && got[first * kCh] == 0.0f)
            ++first;
        QVERIFY(first > 0);
        --first; // the snippet's first sample is faded to exactly 0
        const size_t mid = first + AudioFeeder::kScrubSnippetFrames / 2;
        QVERIFY(mid < got.size() / kCh);
        QVERIFY(std::abs(got[mid * kCh] - ref[size_t(AudioFeeder::kScrubSnippetFrames / 2) * kCh])
            < 1e-4);
        const size_t after = first + AudioFeeder::kScrubSnippetFrames + 100;
        if (after < got.size() / kCh)
            QCOMPARE(got[after * kCh], 0.0f);
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    int result = 0;
    {
        TestVarispeed test;
        result = QTest::qExec(&test, argc, argv);
    }
    g_compositor.reset();
    return result;
}
#include "test_varispeed.moc"
