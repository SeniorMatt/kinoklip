// SPDX-License-Identifier: GPL-3.0-or-later
// Clip audio effects (docs/behavior-spec.md section 51): an effect stack on an audio clip changes
// the mixed samples as expected (gain, filters, dynamics, EQ, delay, reverb), a bypassed effect
// leaves the signal alone, the order of the stack matters, clip effects run before the clip's
// volume, and mixing in blocks of any size (playback) gives the samples of one long mix (export).
#include "RenderTestUtil.h"
#include "core/AudioEffectKeys.h"
#include "core/EffectRegistry.h"
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

/// An EffectInstance of descriptor @p id with its defaults and the given parameter values.
EffectInstance fx(TimelineData& tl, const char* id,
    std::initializer_list<std::pair<const char*, double>> values = {})
{
    EffectInstance e = EffectRegistry::instance().createEffect(QLatin1String(id), tl.allocateId());
    for (const auto& [key, v] : values)
        e.params.setBase(QLatin1String(key), v);
    return e;
}

} // namespace

class TestAudioClipEffects : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_wavSine, m_wavDc, m_wavImpulse, m_wavLow;

    std::shared_ptr<MediaPathMap> paths(const QString& wav)
    {
        auto map = std::make_shared<MediaPathMap>();
        map->insert(kMedia, wav);
        return map;
    }

    /// One clip of @p wav on A1 carrying @p effects, volume @p volumeDb.
    TimelineData timeline(const QVector<EffectInstance>& effects, double volumeDb = 0.0)
    {
        TimelineData tl = emptyTimeline();
        ClipData c = mediaClip(tl, kMedia, 0, kFrames, 0, 2'000'000);
        c.audio.setBase(keys::VolumeDb, volumeDb);
        c.effects = effects;
        addClip(tl, TrackType::Audio, 0, c);
        return tl;
    }

    /// The mix of [0, frames) in one call.
    std::vector<float> render(const TimelineData& tl, const QString& wav, int frames, int block = 0)
    {
        AudioMixer mixer;
        mixer.setMediaPaths(paths(wav));
        std::vector<float> out(size_t(2) * frames);
        if (block <= 0)
            block = frames;
        for (int pos = 0; pos < frames; pos += block) {
            const int n = std::min(block, frames - pos);
            mixer.mix(tl, pos, n, out.data() + size_t(2) * pos);
        }
        return out;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_wavSine = m_dir.filePath("sine.wav");
        m_wavDc = m_dir.filePath("dc.wav");
        m_wavImpulse = m_dir.filePath("impulse.wav");
        m_wavLow = m_dir.filePath("low.wav");
        QVERIFY(writeWav(m_wavSine, 2, [](int64_t n) { return sine(n, 440, 0.6); }));
        QVERIFY(writeWav(m_wavDc, 2, [](int64_t) { return 0.5; }));
        QVERIFY(writeWav(m_wavImpulse, 2, [](int64_t n) { return n == 1000 ? 0.8 : 0.0; }));
        QVERIFY(writeWav(m_wavLow, 2, [](int64_t n) { return sine(n, 50, 0.1); }));
    }

    void registryHasTheAudioEffects()
    {
        const auto list = EffectRegistry::instance().byCategory(EffectCategory::AudioEffect);
        QStringList ids;
        for (const EffectDescriptor* d : list) {
            ids.append(d->id);
            for (const ParamDescriptor& p : d->params)
                QVERIFY2(!p.animatable, qPrintable(d->id + "/" + p.key));
        }
        QCOMPARE(ids,
            QStringList({"audio_gain", "audio_eq", "audio_compressor", "audio_limiter",
                "audio_highpass", "audio_lowpass", "audio_delay", "audio_reverb"}));
    }

    void noEffectsIsUntouched()
    {
        const auto plain = render(timeline({}), m_wavSine, 24000);
        QVERIFY(peak(plain, 1000, 24000) > 0.55);
    }

    void gainMinus6dBHalvesTheAmplitude()
    {
        TimelineData base = timeline({});
        const auto plain = render(base, m_wavSine, 24000);
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::Gain, {{afxkeys::GainDb, -6.0206}}));
        const auto out = render(tl, m_wavSine, 24000);
        for (int i = 0; i < 24000; ++i)
            QVERIFY2(std::abs(out[size_t(2 * i)] - 0.5f * plain[size_t(2 * i)]) < 2e-4f,
                qPrintable(QString::number(i)));
        QVERIFY(std::abs(peak(out, 1000, 24000) / peak(plain, 1000, 24000) - 0.5) < 0.01);
    }

    void bypassRestoresTheOriginal()
    {
        const auto plain = render(timeline({}), m_wavSine, 24000);
        TimelineData tl = timeline({});
        EffectInstance gain = fx(tl, afxid::Gain, {{afxkeys::GainDb, -12.0}});
        EffectInstance hp = fx(tl, afxid::HighPass, {{afxkeys::FrequencyHz, 2000.0}});
        gain.enabled = hp.enabled = false;
        tl.audioTracks[0].clips[0].effects = {gain, hp};
        const auto out = render(tl, m_wavSine, 24000);
        QVERIFY(out == plain); // bit for bit
    }

    void highPassRemovesDc()
    {
        const auto plain = render(timeline({}), m_wavDc, 48000);
        QVERIFY(std::abs(plain[2 * 40000] - 0.5f) < 1e-3f);
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::HighPass, {{afxkeys::FrequencyHz, 80.0}}));
        const auto out = render(tl, m_wavDc, 48000);
        QVERIFY(peak(out, 40000, 48000) < 0.01); // 0.5 -> ~0 after 0.8 s
        QVERIFY(peak(out, 0, 100) > 0.4); // the step passes first
    }

    void lowPassCutsHighFrequencies()
    {
        QTemporaryDir dir;
        const QString wav = dir.filePath("hi.wav");
        QVERIFY(writeWav(wav, 2, [](int64_t n) { return sine(n, 8000, 0.5); }));
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::LowPass, {{afxkeys::FrequencyHz, 500.0}}));
        const auto out = render(tl, wav, 24000);
        QVERIFY(rms(out, 4000, 24000) < 0.35 * std::pow(10.0, -40.0 / 20.0)); // > 40 dB down
    }

    void equalizerLowShelfBoostsBass()
    {
        const auto plain = render(timeline({}), m_wavLow, 24000);
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::Eq, {{afxkeys::LowGainDb, 12.0}, {afxkeys::LowFreqHz, 200.0}}));
        const auto out = render(tl, m_wavLow, 24000);
        const double gain = rms(out, 8000, 24000) / rms(plain, 8000, 24000);
        QVERIFY2(gain > 3.0 && gain < 4.6, qPrintable(QString::number(gain))); // about +12 dB
    }

    void compressorReducesLoudSound()
    {
        const auto plain = render(timeline({}), m_wavSine, 24000);
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(fx(tl, afxid::Compressor,
            {{afxkeys::ThresholdDb, -30.0}, {afxkeys::Ratio, 8.0}, {afxkeys::AttackMs, 1.0}}));
        const auto out = render(tl, m_wavSine, 24000);
        QVERIFY(rms(out, 12000, 24000) < 0.5 * rms(plain, 12000, 24000));
    }

    void limiterHoldsTheCeiling()
    {
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::Limiter, {{afxkeys::CeilingDb, -6.0}}));
        const auto out = render(tl, m_wavSine, 24000);
        QVERIFY(peak(out, 0, 24000) <= 0.5013); // -6 dBFS
        QVERIFY(peak(out, 0, 24000) > 0.45);
    }

    void delayEchoesTheImpulse()
    {
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(fx(tl, afxid::Delay,
            {{afxkeys::TimeMs, 100.0}, {afxkeys::Feedback, 0.0}, {afxkeys::Mix, 50.0}}));
        const auto out = render(tl, m_wavImpulse, 24000);
        QVERIFY(std::abs(out[2 * 1000] - 0.4f) < 2e-3f); // dry half
        QVERIFY(std::abs(out[2 * (1000 + 4800)] - 0.4f) < 2e-3f); // echo after 100 ms
        QVERIFY(peak(out, 1001, 1000 + 4799) < 1e-4);
        QVERIFY(peak(out, 1000 + 4801, 24000) < 1e-4); // no feedback: one echo only
    }

    void delayReachesItsLongTimes()
    {
        // 1.5 s of the 2 s maximum: the stereo delay line must hold the whole time.
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(fx(tl, afxid::Delay,
            {{afxkeys::TimeMs, 1500.0}, {afxkeys::Feedback, 0.0}, {afxkeys::Mix, 50.0}}));
        const auto out = render(tl, m_wavImpulse, 90000);
        QVERIFY(std::abs(out[2 * (1000 + 72000)] - 0.4f) < 2e-3f); // echo after 1.5 s
        QVERIFY(peak(out, 1001, 1000 + 71999) < 1e-4);
    }

    void reverbAddsATail()
    {
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::Reverb, {{afxkeys::RoomSize, 70.0}, {afxkeys::Mix, 40.0}}));
        const auto out = render(tl, m_wavImpulse, 48000);
        QVERIFY(peak(out, 0, 1000) < 1e-6);
        QVERIFY(rms(out, 6000, 30000) > 1e-4); // a tail where the dry sound is silent
        QVERIFY(rms(out, 6000, 12000) > rms(out, 36000, 48000)); // and it dies away
        // Mix 0 is the dry signal.
        tl.audioTracks[0].clips[0].effects[0].params.setBase(afxkeys::Mix, 0.0);
        const auto dry = render(tl, m_wavImpulse, 48000);
        QVERIFY(peak(dry, 6000, 48000) < 1e-6);
    }

    void clipEffectsRunBeforeTheClipVolume()
    {
        const auto plain = render(timeline({}), m_wavSine, 24000);
        // Gain -6 dB in the stack and a clip volume of -6 dB multiply to a quarter.
        TimelineData tl = timeline({}, -6.0206);
        tl.audioTracks[0].clips[0].effects.append(
            fx(tl, afxid::Gain, {{afxkeys::GainDb, -6.0206}}));
        const auto out = render(tl, m_wavSine, 24000);
        QVERIFY(std::abs(peak(out, 1000, 24000) / peak(plain, 1000, 24000) - 0.25) < 0.01);
        // A limiter at -6 dB before a +12 dB clip volume: the volume comes after it, so the
        // peak is the ceiling plus 12 dB (soft clipper permitting: stay below its knee).
        TimelineData tl2 = timeline({}, 6.0206);
        tl2.audioTracks[0].clips[0].effects.append(
            fx(tl2, afxid::Limiter, {{afxkeys::CeilingDb, -12.0}}));
        const auto out2 = render(tl2, m_wavSine, 24000);
        QVERIFY(std::abs(peak(out2, 0, 24000) - 0.2512 * 2.0) < 0.01);
    }

    void stackOrderMatters()
    {
        TimelineData a = timeline({});
        a.audioTracks[0].clips[0].effects = {fx(a, afxid::Gain, {{afxkeys::GainDb, 12.0}}),
            fx(a, afxid::Limiter, {{afxkeys::CeilingDb, -6.0}})};
        TimelineData b = timeline({});
        b.audioTracks[0].clips[0].effects = {fx(b, afxid::Limiter, {{afxkeys::CeilingDb, -6.0}}),
            fx(b, afxid::Gain, {{afxkeys::GainDb, 12.0}})};
        const auto outA = render(a, m_wavSine, 24000);
        const auto outB = render(b, m_wavSine, 24000);
        QVERIFY(peak(outA, 0, 24000) <= 0.5013); // limited last
        QVERIFY(peak(outB, 0, 24000) > 1.5 * peak(outA, 0, 24000)); // boosted last
    }

    void playbackBlocksGiveTheSamplesOfAnExport()
    {
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects
            = {fx(tl, afxid::HighPass, {{afxkeys::FrequencyHz, 200.0}}),
                fx(tl, afxid::Eq, {{afxkeys::MidGainDb, 6.0}}),
                fx(tl, afxid::Compressor, {{afxkeys::ThresholdDb, -24.0}}),
                fx(tl, afxid::Delay, {{afxkeys::TimeMs, 37.0}}), fx(tl, afxid::Reverb),
                fx(tl, afxid::Limiter), fx(tl, afxid::Gain, {{afxkeys::GainDb, -3.0}})};
        const auto whole = render(tl, m_wavSine, 36000);
        for (const int block : {512, 1024, 997}) { // playback chunks; 997 is not a multiple of 64
            const auto chunks = render(tl, m_wavSine, 36000, block);
            float worst = 0.0f;
            for (size_t i = 0; i < whole.size(); ++i)
                worst = std::max(worst, std::abs(whole[i] - chunks[i]));
            QVERIFY2(
                worst < 1e-4f, qPrintable(QStringLiteral("block %1: %2").arg(block).arg(worst)));
        }
        // The same range again from a new mixer (a second export) is identical.
        QVERIFY(render(tl, m_wavSine, 36000) == whole);
    }

    void aSeekRestartsTheEffectMemory()
    {
        TimelineData tl = timeline({});
        tl.audioTracks[0].clips[0].effects.append(fx(tl, afxid::Delay,
            {{afxkeys::TimeMs, 100.0}, {afxkeys::Feedback, 0.0}, {afxkeys::Mix, 100.0}}));
        AudioMixer mixer;
        mixer.setMediaPaths(paths(m_wavImpulse));
        std::vector<float> out(2 * 4800);
        mixer.mix(tl, 0, 4800, out.data()); // the impulse at 1000 goes into the delay line
        mixer.mix(tl, 20000, 4800, out.data()); // a seek: the delay line must be empty
        QVERIFY(peak(out, 0, 4800) < 1e-6);
    }
};

QTEST_GUILESS_MAIN(TestAudioClipEffects)
#include "test_audioclipeffects.moc"
