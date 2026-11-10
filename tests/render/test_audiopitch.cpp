// SPDX-License-Identifier: GPL-3.0-or-later
// Inspector > Audio pitch and clip equalizer (docs/behavior-spec.md section 67): the mixer
// shifts a clip's pitch without changing its length, leaves a clip at 0 untouched, gives the same
// samples whatever the block size (playback and export), and the clip EQ boosts its band.
#include "RenderTestUtil.h"
#include "core/ParamKeys.h"
#include "render/AudioMixer.h"

#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <functional>
#include <numbers>
#include <vector>

using namespace kk;
using namespace kk::test;

namespace {

constexpr int kRate = 48000;
const QString kMedia = QStringLiteral("fx");
constexpr int64_t kFrames = 50; ///< two seconds at 25 fps

/// Writes a 48 kHz mono 16-bit WAV of @p seconds whose sample n is @p sample(n) (-1 ... 1).
bool writeWav(const QString& path, int seconds, const std::function<double(int64_t)>& sample)
{
    const quint32 frames = quint32(kRate * seconds);
    QByteArray bytes;
    auto u32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i)
            bytes.append(char((v >> (8 * i)) & 0xff));
    };
    auto u16 = [&](quint16 v) {
        bytes.append(char(v & 0xff));
        bytes.append(char(v >> 8));
    };
    bytes.append("RIFF");
    u32(36 + frames * 2);
    bytes.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(1);
    u32(kRate);
    u32(kRate * 2);
    u16(2);
    u16(16);
    bytes.append("data");
    u32(frames * 2);
    for (quint32 n = 0; n < frames; ++n)
        u16(quint16(qint16(std::lround(sample(n) * 32767.0))));
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

double sine(int64_t n, double hz, double amp)
{
    return amp * std::sin(2.0 * std::numbers::pi * hz * double(n) / kRate);
}

double rms(const std::vector<float>& out, int from, int to)
{
    double sum = 0.0;
    for (int i = from; i < to; ++i)
        sum += double(out[size_t(2 * i)]) * double(out[size_t(2 * i)]);
    return std::sqrt(sum / (to - from));
}

double peak(const std::vector<float>& out, int from, int to)
{
    double p = 0.0;
    for (int i = from; i < to; ++i)
        p = std::max(p, std::abs(double(out[size_t(2 * i)])));
    return p;
}

/// Rising zero crossings per second of channel 0 over [from, to) (the frequency of a sine).
double frequency(const std::vector<float>& out, int from, int to)
{
    int crossings = 0;
    for (int i = from + 1; i < to; ++i)
        crossings += out[size_t(2 * (i - 1))] < 0.0f && out[size_t(2 * i)] >= 0.0f;
    return crossings * double(kRate) / (to - from);
}

} // namespace

class TestAudioPitch : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_wav, m_quiet;

    std::vector<float> render(const TimelineData& tl, const QString& wav, int frames, int block = 0)
    {
        AudioMixer mixer;
        auto map = std::make_shared<MediaPathMap>();
        map->insert(kMedia, wav);
        mixer.setMediaPaths(map);
        std::vector<float> out(size_t(2) * frames);
        if (block <= 0)
            block = frames;
        for (int pos = 0; pos < frames; pos += block) {
            const int n = std::min(block, frames - pos);
            mixer.mix(tl, pos, n, out.data() + size_t(2) * pos);
        }
        return out;
    }

    TimelineData timeline(double semitones, double cents = 0.0)
    {
        TimelineData tl = emptyTimeline();
        ClipData c = mediaClip(tl, kMedia, 0, kFrames, 0, 2'000'000);
        c.audio.setBase(keys::PitchSemitones, semitones);
        c.audio.setBase(keys::PitchCents, cents);
        addClip(tl, TrackType::Audio, 0, c);
        return tl;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_wav = m_dir.filePath("sine.wav");
        QVERIFY(writeWav(m_wav, 2, [](int64_t n) { return sine(n, 440, 0.6); }));
        m_quiet = m_dir.filePath("quiet.wav"); // room for a boost (the mixer clips at 1)
        QVERIFY(writeWav(m_quiet, 2, [](int64_t n) { return sine(n, 440, 0.1); }));
    }

    void zeroLeavesTheSamplesUnchanged()
    {
        TimelineData plain = emptyTimeline();
        addClip(plain, TrackType::Audio, 0, mediaClip(plain, kMedia, 0, kFrames, 0, 2'000'000));
        const auto a = render(plain, m_wav, 48000);
        const auto b = render(timeline(0, 0), m_wav, 48000);
        QVERIFY(a == b); // bit for bit
    }

    void octaveUpDoublesTheFrequencyAndKeepsTheLength()
    {
        const auto out = render(timeline(12), m_wav, 96000); // the whole 2 s clip
        QVERIFY(std::abs(frequency(out, 24000, 72000) - 880.0) < 12.0);
        QVERIFY(rms(out, 24000, 72000) > 0.3);
        QVERIFY(rms(out, 94000, 96000) > 0.3); // sound up to the end: same length
    }

    void octaveDownHalvesTheFrequency()
    {
        const auto out = render(timeline(-12), m_wav, 96000);
        QVERIFY(std::abs(frequency(out, 24000, 72000) - 220.0) < 8.0);
        QVERIFY(rms(out, 24000, 72000) > 0.3);
    }

    void centsAddToSemitones()
    {
        // +1 semitone, -100 cents = 0: the same as no pitch.
        const auto out = render(timeline(1, -100), m_wav, 48000);
        TimelineData plain = emptyTimeline();
        addClip(plain, TrackType::Audio, 0, mediaClip(plain, kMedia, 0, kFrames, 0, 2'000'000));
        QVERIFY(render(plain, m_wav, 48000) == out);
        // 50 cents up: 440 * 2^(1/24) = 452.9 Hz
        const auto up = render(timeline(0, 50), m_wav, 96000);
        QVERIFY(std::abs(frequency(up, 24000, 72000) - 452.9) < 6.0);
    }

    void blocksOfAnySizeGiveTheSameSamples()
    {
        const TimelineData tl = timeline(7);
        const auto whole = render(tl, m_wav, 48000);
        const auto parts = render(tl, m_wav, 48000, 777);
        float worst = 0.0f;
        for (size_t i = 0; i < whole.size(); ++i)
            worst = std::max(worst, std::abs(whole[i] - parts[i]));
        QVERIFY2(worst < 1e-4f, qPrintable(QString::number(worst)));
    }

    void equalizerBoostsItsBand()
    {
        TimelineData plain = emptyTimeline();
        addClip(plain, TrackType::Audio, 0, mediaClip(plain, kMedia, 0, kFrames, 0, 2'000'000));
        const double base = rms(render(plain, m_quiet, 48000), 12000, 48000);

        TimelineData tl = plain;
        ClipData& c = tl.audioTracks[0].clips[0];
        c.audio.setStatic(keys::EqEnabled, true);
        c.audio.setBase(keys::EqBandFreq[1], 440);
        c.audio.setBase(keys::EqBandGain[1], 12);
        const double boosted = rms(render(tl, m_quiet, 48000), 12000, 48000);
        QVERIFY2(boosted / base > 3.5 && boosted / base < 4.5,
            qPrintable(QString::number(boosted / base)));

        // Off: untouched, bit for bit; a band far away from the tone leaves it alone.
        c.audio.setStatic(keys::EqEnabled, false);
        QVERIFY(render(tl, m_quiet, 48000) == render(plain, m_quiet, 48000));
        c.audio.setStatic(keys::EqEnabled, true);
        c.audio.setBase(keys::EqBandFreq[1], 8000);
        c.audio.setBase(keys::EqBandGain[1], 12);
        c.audio.setBase(keys::EqBandQ[1], 4);
        const double far = rms(render(tl, m_quiet, 48000), 12000, 48000);
        QVERIFY(far / base < 1.1);
    }

    void lowShelfCutsALowTone()
    {
        const QString low = m_dir.filePath("low.wav");
        QVERIFY(writeWav(low, 2, [](int64_t n) { return sine(n, 50, 0.5); }));
        TimelineData plain = emptyTimeline();
        addClip(plain, TrackType::Audio, 0, mediaClip(plain, kMedia, 0, kFrames, 0, 2'000'000));
        const double base = rms(render(plain, low, 48000), 12000, 48000);
        TimelineData tl = plain;
        ClipData& c = tl.audioTracks[0].clips[0];
        c.audio.setStatic(keys::EqEnabled, true);
        c.audio.setBase(keys::EqBandGain[0], -12);
        const double cut = rms(render(tl, low, 48000), 12000, 48000);
        QVERIFY2(cut / base < 0.35, qPrintable(QString::number(cut / base)));
    }
};

QTEST_GUILESS_MAIN(TestAudioPitch)
#include "test_audiopitch.moc"
