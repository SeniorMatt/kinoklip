// SPDX-License-Identifier: GPL-3.0-or-later
// Mixer graph tests (Audio page, spec 37): pan law, bus routing and summing, output bus,
// solo / mute, channel format, EQ / dynamics inside the mix, fader and pan automation,
// contiguity (a range mixes the same in one call, in chunks and after a seek) and metering.
// The sources are float WAV files synthesized by the test.
#include "core/MixerOps.h"
#include "render/AudioMixer.h"
#include "render/MixLevels.h"
#include "render/RenderTypes.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <numbers>
#include <vector>

using namespace kk;

namespace {

constexpr int kRate = 48000;
constexpr double kAmp = 0.25; ///< source sine amplitude (-12 dBFS, below the soft clipper)

/// Writes a 48 kHz stereo float WAV of @p seconds: left = sine(@p hz) * kAmp, right the same
/// unless @p leftOnly.
bool writeWav(const QString& path, double hz, double seconds, bool leftOnly = false)
{
    const int n = int(seconds * kRate);
    QByteArray data;
    data.resize(qsizetype(n) * 8);
    auto* f = reinterpret_cast<float*>(data.data());
    for (int i = 0; i < n; ++i) {
        const float v = float(kAmp * std::sin(2.0 * std::numbers::pi * hz * i / kRate));
        f[size_t(i) * 2] = v;
        f[size_t(i) * 2 + 1] = leftOnly ? 0.0f : v;
    }
    QByteArray h;
    auto u32 = [&h](quint32 v) { h.append(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&h](quint16 v) { h.append(reinterpret_cast<const char*>(&v), 2); };
    h.append("RIFF");
    u32(quint32(36 + data.size()));
    h.append("WAVEfmt ");
    u32(16);
    u16(3); // IEEE float
    u16(2);
    u32(kRate);
    u32(kRate * 8);
    u16(8);
    u16(32);
    h.append("data");
    u32(quint32(data.size()));
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(h) == h.size()
        && file.write(data) == data.size();
}

/// Peak per side over frames [from, end) of interleaved @p s.
std::pair<double, double> peaks(const std::vector<float>& s, int from = 0)
{
    double l = 0, r = 0;
    for (size_t i = size_t(from); i < s.size() / 2; ++i) {
        l = std::max(l, double(std::abs(s[i * 2])));
        r = std::max(r, double(std::abs(s[i * 2 + 1])));
    }
    return {l, r};
}

double db(double linear)
{
    return 20.0 * std::log10(std::max(linear, 1e-12));
}

} // namespace

class TestMixerGraph : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::shared_ptr<MediaPathMap> m_paths = std::make_shared<MediaPathMap>();

    /// 25 fps timeline with the 1 kHz source on A1 (and @p second on A2) over 4 s.
    TimelineData timeline(
        const QString& first = QStringLiteral("sine"), const QString& second = QString())
    {
        TimelineData tl = makeDefaultTimeline(QStringLiteral("mix"), Rational {25, 1}, 1920, 1080, 2, 2);
        int track = 0;
        for (const QString& id : {first, second}) {
            if (id.isEmpty())
                continue;
            ClipData c;
            c.id = tl.allocateId();
            c.kind = ClipKind::Media;
            c.mediaId = id;
            c.start = 0;
            c.duration = 100;
            c.sourceDurationUs = 4'000'000;
            tl.audioTracks[size_t(track++)].clips.push_back(c);
        }
        return tl;
    }

    std::vector<float> render(
        const TimelineData& tl, int64_t start = 0, int frames = 24000, MixLevels* levels = nullptr)
    {
        AudioMixer mixer;
        mixer.setMediaPaths(m_paths);
        mixer.setLevels(levels);
        std::vector<float> out(size_t(frames) * 2);
        mixer.mix(tl, start, frames, out.data());
        return out;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        const QString a = m_dir.filePath(QStringLiteral("sine1k.wav"));
        const QString b = m_dir.filePath(QStringLiteral("sine300.wav"));
        const QString l = m_dir.filePath(QStringLiteral("left1k.wav"));
        QVERIFY(writeWav(a, 1000.0, 4.0));
        QVERIFY(writeWav(b, 300.0, 4.0));
        QVERIFY(writeWav(l, 1000.0, 4.0, true));
        m_paths->insert(QStringLiteral("sine"), a);
        m_paths->insert(QStringLiteral("low"), b);
        m_paths->insert(QStringLiteral("left"), l);
    }

    void neutralMixerPlaysTheSourceUnchanged()
    {
        const auto [l, r] = peaks(render(timeline()));
        QVERIFY(std::abs(l - kAmp) < 1e-3 && std::abs(r - kAmp) < 1e-3);
    }

    void panLawIsMinus3dBConstantPower()
    {
        TimelineData tl = timeline();
        TrackData& a1 = tl.audioTracks[0];
        // Centre: unity on both sides.
        a1.mix.strip.pan = 0.0;
        auto [cl, cr] = peaks(render(tl));
        // Hard left: +3.01 dB on the left, nothing on the right (the centre is -3 dB
        // relative to a hard pan: constant power).
        a1.mix.strip.pan = -1.0;
        auto [ll, lr] = peaks(render(tl));
        QVERIFY2(std::abs(db(ll) - db(cl) - 3.0103) < 0.02,
            qPrintable(QString::number(db(ll) - db(cl))));
        QVERIFY(lr < 1e-6);
        // Half right: power sum equals the centre's.
        a1.mix.strip.pan = 0.5;
        auto [hl, hr] = peaks(render(tl));
        QVERIFY(std::abs(hl * hl + hr * hr - (cl * cl + cr * cr)) < 1e-3);
        QVERIFY(hr > hl);
    }

    void busRoutingSummingAndOutputBus()
    {
        TimelineData tl = timeline(QStringLiteral("sine"), QStringLiteral("low"));
        const quint64 a1 = tl.audioTracks[0].id;
        const quint64 a2 = tl.audioTracks[1].id;
        // Both on Bus 1: the sum (1 kHz + 300 Hz peaks add up to at most 2 x kAmp).
        const double both = peaks(render(tl)).first;
        QVERIFY(both > 1.5 * kAmp && both <= 2.0 * kAmp + 1e-3);

        // A2 to a new Bus 2 only: Bus 1 hears A1 alone.
        const int bus2 = mixerops::addBus(tl, false);
        QVERIFY(mixerops::setTrackRoute(tl, a2, bus2, true));
        QVERIFY(mixerops::setTrackRoute(tl, a2, kMainBusId, false));
        QVERIFY(std::abs(peaks(render(tl)).first - kAmp) < 1e-3);
        // Output Bus 2: A2 alone, with Bus 2's fader.
        QVERIFY(mixerops::setOutputBus(tl, bus2));
        mixerops::bus(tl, bus2)->strip.volumeDb = -6.0206;
        QVERIFY(std::abs(peaks(render(tl)).first - kAmp / 2) < 1e-3);
        // A1 routed to Bus 2 as well: summed there.
        QVERIFY(mixerops::setTrackRoute(tl, a1, bus2, true));
        QVERIFY(peaks(render(tl)).first > 0.75 * kAmp);
        // Bus mute; then deleting the bus falls back to the main bus as output.
        mixerops::bus(tl, bus2)->strip.muted = true;
        QVERIFY(peaks(render(tl)).first < 1e-6);
        QVERIFY(mixerops::removeBus(tl, bus2));
        QCOMPARE(tl.mixer.outputBus, kMainBusId);
        QVERIFY(!tl.audioTracks[1].mix.buses.contains(bus2));
        QVERIFY(std::abs(peaks(render(tl)).first - kAmp) < 1e-3); // A1 only (A2 unrouted)
        QVERIFY(!mixerops::removeBus(tl, kMainBusId)); // the main bus stays
    }

    void soloAndMute()
    {
        TimelineData tl = timeline(QStringLiteral("sine"), QStringLiteral("low"));
        tl.audioTracks[1].solo = true;
        std::vector<float> out = render(tl);
        // Only A2 (300 Hz): zero crossings tell the frequency.
        int crossings = 0;
        for (size_t i = 1; i < out.size() / 2; ++i)
            crossings += (out[i * 2] >= 0) != (out[(i - 1) * 2] >= 0);
        QVERIFY2(std::abs(crossings - 300) < 4, qPrintable(QString::number(crossings)));
        tl.audioTracks[1].solo = false;
        tl.audioTracks[0].muted = true;
        tl.audioTracks[1].muted = true;
        QVERIFY(peaks(render(tl)).first == 0.0);
        // Solo wins over the other tracks' state, mute of the soloed track silences it.
        tl.audioTracks[0].muted = false;
        tl.audioTracks[1].solo = true;
        QVERIFY(peaks(render(tl)).first == 0.0);
    }

    void monoTrackFoldsTheChannels()
    {
        TimelineData tl = timeline(QStringLiteral("left"));
        auto [sl, sr] = peaks(render(tl));
        QVERIFY(std::abs(sl - kAmp) < 1e-3 && sr < 1e-6); // stereo: as recorded
        QVERIFY(mixerops::setTrackMono(tl, tl.audioTracks[0].id, true));
        auto [ml, mr] = peaks(render(tl));
        QVERIFY(std::abs(ml - kAmp / 2) < 1e-3 && std::abs(mr - kAmp / 2) < 1e-3);
    }

    void eqAndDynamicsInTheMix()
    {
        TimelineData tl = timeline();
        ChannelStrip& strip = tl.audioTracks[0].mix.strip;
        strip.eq.enabled = true;
        strip.eq.bands[2] = EqBand {EqBandType::Bell, true, 1000.0, 6.0, 1.0};
        QVERIFY(std::abs(db(peaks(render(tl), 12000).first) - (db(kAmp) + 6.0)) < 0.1);
        // The bus EQ adds to it (+3 dB more: stays below the master soft clipper's knee).
        EqSettings busEq = strip.eq;
        busEq.bands[2].gainDb = 3.0;
        mixerops::bus(tl, kMainBusId)->strip.eq = busEq;
        QVERIFY(std::abs(db(peaks(render(tl), 12000).first) - (db(kAmp) + 9.0)) < 0.15);
        // A limiter on the bus holds the output at its ceiling.
        DynamicsSettings& dyn = mixerops::bus(tl, kMainBusId)->strip.dynamics;
        dyn.limiterEnabled = true;
        dyn.limiterCeilingDb = -9.0;
        QVERIFY(db(peaks(render(tl), 2400).first) <= -9.0 + 0.01);
    }

    void faderAndPanAutomation()
    {
        TimelineData tl = timeline();
        TrackData& t = tl.audioTracks[0];
        // Fader keys: 0 dB at frame 0, -20 dB at frame 50 (2 s), linear in dB between.
        QVERIFY(mixerops::writeAutomation(tl, t.id, mixkeys::kVolumeDb, 0, 0.0, 0));
        QVERIFY(mixerops::writeAutomation(tl, t.id, mixkeys::kVolumeDb, 50, -20.0, 50));
        // Around frame 25 (1 s): -10 dB (a 10 ms window: the ramp moves 0.1 dB in it).
        const std::vector<float> mid = render(tl, 48000 - 240, 480);
        QVERIFY2(std::abs(db(peaks(mid).first) - (db(kAmp) - 10.0)) < 0.3,
            qPrintable(QString::number(db(peaks(mid).first) - db(kAmp))));
        // After the last key: -20 dB.
        QVERIFY(std::abs(db(peaks(render(tl, 3 * 48000, 4800)).first) - (db(kAmp) - 20.0)) < 0.05);
        // Off: the static fader plays; Read again: the curve.
        QVERIFY(mixerops::setAutomationMode(tl, t.id, AutomationMode::Off));
        QVERIFY(std::abs(peaks(render(tl, 3 * 48000, 4800)).first - kAmp) < 1e-3);
        QVERIFY(mixerops::setAutomationMode(tl, t.id, AutomationMode::Read));
        // Pan automation: hard right from frame 0.
        QVERIFY(mixerops::writeAutomation(tl, t.id, mixkeys::kPan, 0, 1.0, 0));
        const auto [l, r] = peaks(render(tl, 3 * 48000, 4800));
        QVERIFY(l < 1e-6 && r > 0.0);
        // A write at frame 60 coming from frame 0 replaces the keys it passed (frame 50).
        QVERIFY(mixerops::writeAutomation(tl, t.id, mixkeys::kVolumeDb, 60, -6.0, 0));
        QCOMPARE(t.mix.automationKeys.values.value(mixkeys::kVolumeDb).keys.size(), 2);
    }

    void rangesMixTheSameInChunksAndAfterSeeks()
    {
        TimelineData tl = timeline();
        ChannelStrip& strip = tl.audioTracks[0].mix.strip;
        strip.eq.enabled = true;
        strip.eq.bands[0].enabled = true; // 80 Hz high-pass
        strip.eq.bands[3].gainDb = -4.0;
        strip.dynamics.compressorEnabled = true;
        strip.dynamics.thresholdDb = -18.0;
        // One call (export-like) ...
        const std::vector<float> whole = render(tl, 48000, 9600);
        // ... the same range in playback-sized chunks from a mixer that was elsewhere before.
        AudioMixer mixer;
        mixer.setMediaPaths(m_paths);
        std::vector<float> elsewhere(2048);
        mixer.mix(tl, 0, 1024, elsewhere.data()); // a different position first (seek after)
        std::vector<float> chunked(whole.size());
        for (int done = 0; done < 9600; done += 1024) {
            const int n = std::min(1024, 9600 - done);
            mixer.mix(tl, 48000 + done, n, chunked.data() + size_t(done) * 2);
        }
        for (size_t i = 0; i < whole.size(); ++i)
            QVERIFY2(std::abs(whole[i] - chunked[i]) < 1e-6, qPrintable(QString::number(i)));
    }

    void metersReportTracksBusesAndOutput()
    {
        TimelineData tl = timeline(QStringLiteral("sine"), QStringLiteral("low"));
        tl.audioTracks[1].mix.strip.volumeDb = -6.0206;
        const int bus2 = mixerops::addBus(tl, true, QStringLiteral("Dialogue"));
        QVERIFY(mixerops::setTrackRoute(tl, tl.audioTracks[1].id, bus2, true));
        MixLevels levels;
        render(tl, 0, 24000, &levels);
        QCOMPARE(levels.tracks.count, int(tl.audioTracks.size()));
        const ChannelLevel* a1 = levels.tracks.find(tl.audioTracks[0].id);
        const ChannelLevel* a2 = levels.tracks.find(tl.audioTracks[1].id);
        QVERIFY(a1 && a2);
        QVERIFY(std::abs(a1->peak[0] - kAmp) < 1e-3);
        QVERIFY(std::abs(a2->peak[0] - kAmp / 2) < 1e-3);
        QCOMPARE(levels.buses.count, 2);
        const ChannelLevel* b2 = levels.buses.find(quint64(bus2));
        QVERIFY(b2 && b2->mono && std::abs(b2->peak[0] - kAmp / 2) < 1e-3);
        QVERIFY(levels.output[0] > kAmp);
    }
};

QTEST_GUILESS_MAIN(TestMixerGraph)
#include "test_mixergraph.moc"
