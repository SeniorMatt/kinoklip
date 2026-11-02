// SPDX-License-Identifier: GPL-3.0-or-later
// AudioMixer tests: sample accuracy, gain/pan maths, volume keyframes, mute/solo, speed,
// crossfades under (linked) video transitions, fades at track ends, soft clipper.
// Uses mono_ramp_48k.wav: sample n = ((n mod 4800) / 4800)·2 − 1, so every value tells its
// own source position.
#include "RenderTestUtil.h"
#include "render/AudioMixer.h"

#include <QElapsedTimer>

#include <numbers>
#include <vector>

using namespace kk;
using namespace kk::test;

namespace {

/// Value of source sample @p n of the ramp fixture.
double ramp(int64_t n)
{
    return double(n % 4800) / 4800.0 * 2.0 - 1.0;
}

/// 16-bit PCM quantisation + float rounding.
constexpr double kTolerance = 1.0 / 16384.0;

const QString kRamp = QStringLiteral("ramp");

} // namespace

class TestAudioMixer : public QObject
{
    Q_OBJECT

    QString m_skip;

    std::shared_ptr<MediaPathMap> paths()
    {
        auto map = std::make_shared<MediaPathMap>();
        map->insert(kRamp, fixture("mono_ramp_48k.wav"));
        return map;
    }

    /// Timeline with one ramp clip on A1 at frame @p start (25 fps), source in @p sourceInUs.
    TimelineData oneClip(int64_t start, int64_t duration, int64_t sourceInUs, double gainDb)
    {
        TimelineData tl = emptyTimeline();
        ClipData c = mediaClip(tl, kRamp, start, duration, sourceInUs, 5'000'000);
        c.audio.setBase(keys::VolumeDb, gainDb);
        addClip(tl, TrackType::Audio, 0, c);
        return tl;
    }

private slots:
    void initTestCase()
    {
        if (!ensureFixtures(&m_skip))
            qWarning("%s", qPrintable(m_skip));
    }

    void maths()
    {
        float l = 0, r = 0;
        AudioMixer::panGains(0.0, l, r);
        QVERIFY(std::abs(l - 1.0f) < 1e-6f && std::abs(r - 1.0f) < 1e-6f); // unity at centre
        AudioMixer::panGains(-1.0, l, r);
        QVERIFY(std::abs(l - float(std::numbers::sqrt2)) < 1e-6f && std::abs(r) < 1e-6f);
        for (const double pan : {-0.7, -0.2, 0.3, 0.9}) {
            AudioMixer::panGains(pan, l, r);
            QVERIFY(std::abs(l * l + r * r - 2.0f) < 1e-5f); // constant power
        }
        AudioMixer::panGains(5.0, l, r); // clamped to +1
        QVERIFY(std::abs(l) < 1e-6f);

        QCOMPARE(AudioMixer::dbToGain(0), 1.0f);
        QVERIFY(std::abs(AudioMixer::dbToGain(-6.0206) - 0.5f) < 1e-4f);
        QVERIFY(std::abs(AudioMixer::dbToGain(20) - 10.0f) < 1e-4f);
        QCOMPARE(AudioMixer::dbToGain(-96), 0.0f);
        QCOMPARE(AudioMixer::dbToGain(-200), 0.0f);

        QCOMPARE(AudioMixer::softClip(0.5f), 0.5f);
        QCOMPARE(AudioMixer::softClip(-0.8f), -0.8f);
        QVERIFY(AudioMixer::softClip(10.0f) <= 1.0f && AudioMixer::softClip(1.0f) < 1.0f);
        QVERIFY(AudioMixer::softClip(-10.0f) >= -1.0f);
        QVERIFY(AudioMixer::softClip(1.2f) > AudioMixer::softClip(1.0f)); // monotonic
        const float k = AudioMixer::kClipKnee;
        QVERIFY(std::abs(AudioMixer::softClip(k + 1e-4f) - (k + 1e-4f)) < 1e-6f); // smooth knee
    }

    void sampleAccuracy()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        // Clip at frame 10 (sample 19200), source in 0.5 s (24000), −6 dB (stays below the knee).
        const TimelineData tl = oneClip(10, 50, 500'000, -6.0206);
        AudioMixer mixer;
        mixer.setMediaPaths(paths());
        std::vector<float> out(2 * 2000);
        mixer.mix(tl, 19000, 2000, out.data());
        for (int i = 0; i < 2000; ++i) {
            const int64_t s = 19000 + i;
            const double expected = s < 19200 ? 0.0 : 0.5 * ramp(24000 + (s - 19200));
            QVERIFY2(std::abs(out[size_t(2 * i)] - expected) < kTolerance
                    && out[size_t(2 * i)] == out[size_t(2 * i + 1)],
                qPrintable(QStringLiteral("sample %1: %2 vs %3")
                               .arg(s)
                               .arg(out[size_t(2 * i)])
                               .arg(expected)));
        }
        // A random read elsewhere gives the same samples as contiguous playback would.
        mixer.mix(tl, 60000, 512, out.data());
        for (int i = 0; i < 512; ++i)
            QVERIFY(std::abs(out[size_t(2 * i)] - 0.5 * ramp(24000 + (60000 + i - 19200)))
                < kTolerance);
        // Past the clip end (frame 60 = sample 115200): silence.
        mixer.mix(tl, 115200, 256, out.data());
        QVERIFY(std::all_of(out.begin(), out.begin() + 512, [](float v) { return v == 0.0f; }));
        QCOMPARE(mixer.openDecoders(), 1);
    }

    void gainAndPan()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        TimelineData tl = oneClip(0, 50, 0, -12.0);
        tl.audioTracks[0].clips[0].audio.setBase(keys::Pan, 0.5);
        AudioMixer mixer;
        mixer.setMediaPaths(paths());
        std::vector<float> out(2 * 256);
        mixer.mix(tl, 1000, 256, out.data());
        float l = 0, r = 0;
        AudioMixer::panGains(0.5, l, r);
        const double g = std::pow(10.0, -12.0 / 20.0);
        for (int i = 0; i < 256; ++i) {
            QVERIFY(std::abs(out[size_t(2 * i)] - g * l * ramp(1000 + i)) < kTolerance);
            QVERIFY(std::abs(out[size_t(2 * i + 1)] - g * r * ramp(1000 + i)) < kTolerance);
        }
    }

    void volumeKeyframes()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        TimelineData tl = oneClip(0, 50, 0, 0);
        AnimatedValue& vol = tl.audioTracks[0].clips[0].audio.animated(keys::VolumeDb, 0);
        vol.setKey(0, 0);
        vol.setKey(10, -20);
        AudioMixer mixer;
        mixer.setMediaPaths(paths());
        std::vector<float> out(2 * 64);
        // Local frame 5 (sample 9600) → −10 dB.
        mixer.mix(tl, 9600, 64, out.data());
        const double g = std::pow(10.0, -10.0 / 20.0);
        QVERIFY2(std::abs(out[0] - g * ramp(9600)) < 0.002,
            qPrintable(QStringLiteral("%1 vs %2").arg(out[0]).arg(g * ramp(9600))));
        // Between frames the dB value is interpolated (no steps at frame boundaries).
        mixer.mix(tl, 9600 + 960, 64, out.data()); // local frame 5.5 → −11 dB
        QVERIFY(std::abs(out[0] - std::pow(10.0, -11.0 / 20.0) * ramp(10560)) < 0.002);
    }

    // Spec 15.11: audio follows the same curve as playback of video parameters: eased
    // segments are evaluated between frames, static (hold) keys step exactly at the next key.
    void spec15_11_volumeCurvesExact()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        TimelineData tl = oneClip(0, 50, 0, 0);
        AnimatedValue& vol = tl.audioTracks[0].clips[0].audio.animated(keys::VolumeDb, 0);
        vol.setKey(0, 0, Interp::EaseInOut);
        vol.setKey(10, -20, Interp::EaseInOut);
        vol.setKey(20, -6, Interp::Hold);
        vol.setKey(30, 0);
        AudioMixer mixer;
        mixer.setMediaPaths(paths());
        std::vector<float> out(2 * 64);
        // Local frame 2.5 (sample 4800): smoothstep(0.25) of -20 dB = -3.125 dB.
        mixer.mix(tl, 4800, 64, out.data());
        QVERIFY(std::abs(out[0] - std::pow(10.0, -3.125 / 20.0) * ramp(4800)) < 0.002);
        // Frame 29.5: still the held -6 dB (no ramp into the next key).
        mixer.mix(tl, 29 * 1920 + 960, 64, out.data());
        QVERIFY(std::abs(out[0] - std::pow(10.0, -6.0 / 20.0) * ramp(29 * 1920 + 960)) < 0.002);
    }

    void muteAndSolo()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        TimelineData tl = oneClip(0, 50, 0, -12);
        ClipData copy = tl.audioTracks[0].clips[0];
        copy.id = tl.allocateId();
        addClip(tl, TrackType::Audio, 1, copy);
        AudioMixer mixer;
        mixer.setMediaPaths(paths());
        std::vector<float> out(2 * 64);
        const double g = std::pow(10.0, -12.0 / 20.0);
        auto first = [&] {
            mixer.mix(tl, 3000, 64, out.data());
            return out[0];
        };
        QVERIFY(std::abs(first() - 2 * g * ramp(3000)) < kTolerance);
        tl.audioTracks[1].muted = true;
        QVERIFY(std::abs(first() - g * ramp(3000)) < kTolerance);
        tl.audioTracks[1].muted = false;
        tl.audioTracks[1].solo = true;
        QVERIFY(std::abs(first() - g * ramp(3000)) < kTolerance);
        tl.audioTracks[0].solo = true;
        QVERIFY(std::abs(first() - 2 * g * ramp(3000)) < kTolerance);
        tl.audioTracks[0].clips[0].enabled = false;
        QVERIFY(std::abs(first() - g * ramp(3000)) < kTolerance);
    }

    void speed()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        TimelineData tl = oneClip(0, 50, 0, -12);
        tl.audioTracks[0].clips[0].speed = 2.0;
        tl.audioTracks[0].clips[0].pitchCorrection = false; // resampled: pitch follows speed
        AudioMixer mixer;
        mixer.setMediaPaths(paths());
        std::vector<float> out(2 * 100);
        mixer.mix(tl, 100, 100, out.data());
        const double g = std::pow(10.0, -12.0 / 20.0);
        for (int i = 0; i < 100; ++i) // 2× speed: timeline sample s plays source sample 2s
            QVERIFY(std::abs(out[size_t(2 * i)] - g * ramp(2 * (100 + i))) < kTolerance);
    }

    void freezeAndReverseAreMuted()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        for (const double speed : {0.0, -1.0}) {
            TimelineData tl = oneClip(0, 50, 1'000'000, 0.0);
            tl.audioTracks[0].clips[0].speed = speed;
            AudioMixer mixer;
            mixer.setMediaPaths(paths());
            std::vector<float> out(2 * 2000);
            mixer.mix(tl, 1000, 2000, out.data());
            for (float v : out)
                QCOMPARE(v, 0.0f);
        }
    }

    void pitchCorrectedClipKeepsLength()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        // 50 % with pitch correction: sound for the whole (doubled) clip, silence after it.
        TimelineData tl = oneClip(0, 50, 0, 0.0);
        tl.audioTracks[0].clips[0].speed = 0.5;
        AudioMixer mixer;
        mixer.setMediaPaths(paths());
        std::vector<float> out(2 * 48000);
        mixer.mix(tl, 0, 48000, out.data()); // the clip is 2 s = 96000 samples at 25 fps
        double energy = 0.0;
        for (int i = 2000; i < 48000; ++i)
            energy += double(out[size_t(2 * i)]) * out[size_t(2 * i)];
        QVERIFY(energy / 46000.0 > 0.1); // the ramp fixture's RMS is ~0.58
        mixer.mix(tl, 96000 + 2048, 1024, out.data());
        for (int i = 0; i < 2048; ++i)
            QCOMPARE(out[size_t(i)], 0.0f);
    }

    void crossfadeUnderLinkedVideoTransition()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        // V1: A [0, 50) | B [50, 100) with a 10-frame dissolve on B (window [45, 55)).
        // A1: the linked audio clips, B's source starting at 2 s.
        TimelineData tl = emptyTimeline();
        ClipData va = solidClip(tl, Qt::red, 0, 50);
        ClipData vb = solidClip(tl, Qt::blue, 50, 50);
        va.linkGroup = 101;
        vb.linkGroup = 102;
        vb.transitionIn = TransitionData {QStringLiteral("cross_dissolve"), 10, {}};
        addClip(tl, TrackType::Video, 0, va);
        addClip(tl, TrackType::Video, 0, vb);
        ClipData aa = mediaClip(tl, kRamp, 0, 50, 0, 5'000'000);
        ClipData ab = mediaClip(tl, kRamp, 50, 50, 2'000'000, 5'000'000);
        aa.linkGroup = 101;
        ab.linkGroup = 102;
        aa.audio.setBase(keys::VolumeDb, -12);
        ab.audio.setBase(keys::VolumeDb, -12);
        addClip(tl, TrackType::Audio, 0, aa);
        addClip(tl, TrackType::Audio, 0, ab);

        AudioMixer mixer;
        mixer.setMediaPaths(paths());
        const double g = std::pow(10.0, -12.0 / 20.0);
        const int64_t fadeStart = 86400, fadeEnd = 105600, cut = 96000;
        std::vector<float> out(2 * 64);
        auto expected = [&](int64_t s) {
            const double t = double(s - fadeStart) / double(fadeEnd - fadeStart);
            const double gainA = s < fadeStart ? 1.0
                : s < fadeEnd                  ? std::cos(t * std::numbers::pi / 2)
                                               : 0.0;
            const double gainB = s < fadeStart ? 0.0
                : s < fadeEnd                  ? std::sin(t * std::numbers::pi / 2)
                                               : 1.0;
            // A keeps playing into its handle; B starts early (before its source in).
            return g * (gainA * ramp(s) + gainB * ramp(96000 + (s - cut)));
        };
        for (const int64_t s : {int64_t(80000), fadeStart + 100, cut - 500, cut, cut + 3000,
                 fadeEnd - 64, fadeEnd + 10}) {
            mixer.mix(tl, s, 64, out.data());
            // The gain ramps linearly inside 64-sample blocks: allow that deviation.
            QVERIFY2(std::abs(out[0] - expected(s)) < 0.003,
                qPrintable(
                    QStringLiteral("sample %1: %2 vs %3").arg(s).arg(out[0]).arg(expected(s))));
        }
        // At the cut both sides contribute equally (equal power: cos = sin = 0.707).
        mixer.mix(tl, cut, 1, out.data());
        QVERIFY(std::abs(out[0] - g * std::sqrt(0.5) * (ramp(cut) + ramp(96000))) < 0.003);
    }

    void fadeFromSilenceAtTrackStart()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        TimelineData tl = oneClip(0, 50, 0, -12);
        tl.audioTracks[0].clips[0].transitionIn
            = TransitionData {QStringLiteral("cross_dissolve"), 10, {}};
        AudioMixer mixer;
        mixer.setMediaPaths(paths());
        std::vector<float> out(2 * 64);
        const double g = std::pow(10.0, -12.0 / 20.0);
        mixer.mix(tl, 0, 64, out.data());
        QCOMPARE(out[0], 0.0f);
        mixer.mix(tl, 9600, 1, out.data()); // halfway through [0, 19200)
        QVERIFY(std::abs(out[0] - g * std::sin(std::numbers::pi / 4) * ramp(9600)) < 0.002);
    }

    void missingMediaIsSilent()
    {
        TimelineData tl = oneClip(0, 50, 0, 0);
        tl.audioTracks[0].clips[0].mediaId = QStringLiteral("nope");
        AudioMixer mixer;
        mixer.setMediaPaths(paths());
        std::vector<float> out(2 * 64, 1.0f);
        mixer.mix(tl, 0, 64, out.data());
        QVERIFY(std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }));
    }

    void throughput()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        // 4 tracks × 1 clip each, 10 s in 1024-frame blocks (typical playback block size).
        TimelineData tl = emptyTimeline();
        tl.audioTracks.resize(4);
        for (int t = 0; t < 4; ++t) {
            tl.audioTracks[size_t(t)].id = tl.allocateId();
            ClipData c = mediaClip(tl, kRamp, 0, 125, 0, 5'000'000);
            c.audio.setBase(keys::VolumeDb, -12);
            c.audio.animated(keys::Pan, 0).setKey(0, -1);
            c.audio.animated(keys::Pan, 0).setKey(100, 1);
            addClip(tl, TrackType::Audio, t, c);
        }
        AudioMixer mixer;
        mixer.setMediaPaths(paths());
        std::vector<float> out(2 * 1024);
        QElapsedTimer timer;
        timer.start();
        int64_t pos = 0;
        while (pos < 5 * 48000) {
            mixer.mix(tl, pos, 1024, out.data());
            pos += 1024;
        }
        const double ms = timer.nsecsElapsed() / 1e6;
        qInfo("mixed 4 tracks x 5 s in %.1f ms (%.0fx real time)", ms, 5000.0 / ms);
        QVERIFY(ms < 5000);
    }
};

QTEST_GUILESS_MAIN(TestAudioMixer)
#include "test_audiomixer.moc"
