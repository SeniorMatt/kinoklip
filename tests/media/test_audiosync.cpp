// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for media/AudioSync: offset detection on synthetic recordings of one "scene"
// (known offsets, gain, noise, different lengths, partial overlap, negative offsets), false
// match rejection, the FFT correlation helper, and the file path through AudioDecoder.
#include "media/AudioSync.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <cstring>
#include <random>

using namespace kk::media;

namespace {

constexpr int kRate = 48000;

/// A "scene" of @p seconds: speech-like bursts (noise shaped by 40-300 ms envelopes, varying
/// level, pauses between them) plus a few claps. Deterministic for a seed.
std::vector<float> scene(double seconds, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::vector<float> out(size_t(seconds * kRate), 0.0f);
    size_t pos = 0;
    while (pos < out.size()) {
        pos += size_t((0.03 + 0.25 * uni(rng)) * kRate); // pause
        const auto len = size_t((0.04 + 0.26 * uni(rng)) * kRate);
        const double level = 0.05 + 0.4 * uni(rng);
        // Band-limited noise: a one-pole low-pass with a random cut-off (vowel-ish colour).
        const double alpha = 0.2 + 0.7 * uni(rng);
        double state = 0.0;
        for (size_t i = 0; i < len && pos + i < out.size(); ++i) {
            const double env = std::sin(M_PI * double(i) / double(len));
            state = alpha * state + (1.0 - alpha) * gauss(rng);
            out[pos + i] += float(level * env * state);
        }
        pos += len;
        if (uni(rng) < 0.05 && pos < out.size()) { // a clap: 5 ms decaying noise
            for (size_t i = 0; i < size_t(0.005 * kRate) && pos + i < out.size(); ++i)
                out[pos + i] += float(0.8 * std::exp(-double(i) / 60.0) * gauss(rng));
        }
    }
    return out;
}

/// A recording of [from, from + seconds) of @p src with @p gain and white noise of @p noise
/// RMS; parts outside the scene are noise only.
std::vector<float> record(const std::vector<float>& src, double from, double seconds, double gain,
    double noise, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::vector<float> out(size_t(seconds * kRate));
    const auto offset = int64_t(std::llround(from * kRate));
    for (size_t i = 0; i < out.size(); ++i) {
        const int64_t s = offset + int64_t(i);
        const double x = s >= 0 && s < int64_t(src.size()) ? src[size_t(s)] : 0.0;
        out[i] = float(gain * x + noise * gauss(rng));
    }
    return out;
}

SyncMatch sync(
    const std::vector<float>& ref, const std::vector<float>& other, const SyncOptions& options = {})
{
    return findSyncOffset(syncFeatures(ref.data(), int64_t(ref.size())),
        syncFeatures(other.data(), int64_t(other.size())), options);
}

/// Allowed error of a found offset: 2 ms (a 25 fps frame is 40 ms).
constexpr double kTolerance = 0.002;

} // namespace

class TestAudioSync : public QObject
{
    Q_OBJECT

private slots:
    void correlationMatchesDirectSum()
    {
        std::mt19937 rng(3);
        std::normal_distribution<double> g(0.0, 1.0);
        const size_t m = 64;
        std::vector<double> a(m), b(m);
        for (size_t i = 0; i < m; ++i) {
            a[i] = g(rng);
            b[i] = g(rng);
        }
        const std::vector<double> c = circularCorrelation(a, b);
        for (size_t k = 0; k < m; ++k) {
            double direct = 0.0;
            for (size_t n = 0; n < m; ++n)
                direct += a[(n + k) % m] * b[n];
            QVERIFY2(std::abs(direct - c[k]) < 1e-9, qPrintable(QString::number(k)));
        }
    }

    void knownOffsets_data()
    {
        QTest::addColumn<double>("refFrom");
        QTest::addColumn<double>("refLength");
        QTest::addColumn<double>("otherFrom");
        QTest::addColumn<double>("otherLength");
        QTest::addColumn<double>("gain");
        QTest::addColumn<double>("noise");
        // Offset = otherFrom - refFrom (other time 0 at that reference time).
        QTest::newRow("clean, later start") << 0.0 << 40.0 << 7.345 << 25.0 << 1.0 << 0.0;
        QTest::newRow("negative offset") << 10.0 << 30.0 << 2.5 << 40.0 << 1.0 << 0.0;
        QTest::newRow("quiet and noisy (gain 0.1, SNR ~3 dB)")
            << 5.0 << 50.0 << 12.0011 << 30.0 << 0.1 << 0.006;
        QTest::newRow("loud other (gain 4)") << 0.0 << 30.0 << 3.21 << 20.0 << 4.0 << 0.01;
        QTest::newRow("partial overlap (8 s of 30 s)")
            << 0.0 << 30.0 << 22.0 << 30.0 << 0.5 << 0.01;
        QTest::newRow("short other (6 s)") << 0.0 << 60.0 << 41.7 << 6.0 << 1.0 << 0.01;
        QTest::newRow("sub-frame offset") << 0.0 << 30.0 << 1.0173 << 25.0 << 1.0 << 0.005;
    }

    void knownOffsets()
    {
        QFETCH(double, refFrom);
        QFETCH(double, refLength);
        QFETCH(double, otherFrom);
        QFETCH(double, otherLength);
        QFETCH(double, gain);
        QFETCH(double, noise);
        const std::vector<float> src = scene(120.0, 42);
        const std::vector<float> ref = record(src, refFrom, refLength, 1.0, 0.003, 1);
        const std::vector<float> other = record(src, otherFrom, otherLength, gain, noise, 2);
        const SyncMatch m = sync(ref, other);
        const double expected = otherFrom - refFrom;
        qInfo("offset %.4f s (expected %.4f), confidence %.2f, correlation %.2f, score %.1f, "
              "overlap %.1f s",
            m.offsetSeconds, expected, m.confidence, m.correlation, m.score, m.overlapSeconds);
        QVERIFY(m.found);
        QVERIFY2(std::abs(m.offsetSeconds - expected) < kTolerance,
            qPrintable(QStringLiteral("%1 vs %2").arg(m.offsetSeconds).arg(expected)));
        QVERIFY(m.confidence >= kSyncMinConfidence);
    }

    void unrelatedRecordingsAreRejected()
    {
        // Two different scenes: there is no right answer; the search must say so.
        for (unsigned seed = 0; seed < 4; ++seed) {
            const std::vector<float> a = record(scene(40.0, 100 + seed), 0.0, 40.0, 1.0, 0.01, 7);
            const std::vector<float> b = record(scene(40.0, 200 + seed), 0.0, 30.0, 1.0, 0.01, 8);
            const SyncMatch m = sync(a, b);
            qInfo("unrelated %u: score %.1f, confidence %.2f", seed, m.score, m.confidence);
            QVERIFY(!m.found);
        }
        // Pure noise against a scene.
        const std::vector<float> a = record(scene(30.0, 5), 0.0, 30.0, 1.0, 0.01, 9);
        const std::vector<float> b = record({}, 0.0, 30.0, 0.0, 0.1, 10);
        QVERIFY(!sync(a, b).found);
        // Silence against a scene.
        const std::vector<float> silence(size_t(20 * kRate), 0.0f);
        QVERIFY(!sync(a, silence).found);
    }

    void maxOffsetLimitsTheSearch()
    {
        const std::vector<float> src = scene(60.0, 11);
        const std::vector<float> ref = record(src, 0.0, 40.0, 1.0, 0.003, 1);
        const std::vector<float> other = record(src, 15.0, 20.0, 1.0, 0.003, 2);
        SyncOptions options;
        options.maxOffsetSeconds = 5.0; // the true offset (15 s) is outside
        const SyncMatch limited = sync(ref, other, options);
        QVERIFY(!limited.found || std::abs(limited.offsetSeconds) <= 5.0 + 1e-3);
        options.maxOffsetSeconds = 20.0;
        QVERIFY(std::abs(sync(ref, other, options).offsetSeconds - 15.0) < kTolerance);
    }

    void longRecordingsAreFast()
    {
        // 10 minutes against 8 minutes: features + correlation must stay interactive.
        const std::vector<float> src = scene(620.0, 77);
        const std::vector<float> ref = record(src, 0.0, 600.0, 1.0, 0.003, 1);
        const std::vector<float> other = record(src, 95.25, 480.0, 0.3, 0.003, 2);
        QElapsedTimer timer;
        timer.start();
        const SyncFeatures fr = syncFeatures(ref.data(), int64_t(ref.size()));
        const SyncFeatures fo = syncFeatures(other.data(), int64_t(other.size()));
        const qint64 featureMs = timer.restart();
        const SyncMatch m = findSyncOffset(fr, fo);
        const qint64 searchMs = timer.elapsed();
        qInfo("10 min vs 8 min: features %lld ms, search %lld ms, offset %.4f, confidence %.2f",
            featureMs, searchMs, m.offsetSeconds, m.confidence);
        QVERIFY(m.found);
        QVERIFY(std::abs(m.offsetSeconds - 95.25) < kTolerance);
        QVERIFY(searchMs < 3000);
    }

    void stereoDownmixMatchesMono()
    {
        const std::vector<float> mono = record(scene(10.0, 3), 0.0, 10.0, 1.0, 0.0, 1);
        std::vector<float> stereo(mono.size() * 2);
        for (size_t i = 0; i < mono.size(); ++i)
            stereo[2 * i] = stereo[2 * i + 1] = mono[i];
        SyncFeatureBuilder b;
        b.pushStereo(stereo.data(), int64_t(mono.size()));
        const SyncFeatures fs = b.finish();
        const SyncFeatures fm = syncFeatures(mono.data(), int64_t(mono.size()));
        QCOMPARE(fs.onset.size(), fm.onset.size());
        QCOMPARE(fs.onset.size(), size_t(10 * kSyncFeatureRate));
        for (size_t i = 0; i < fs.onset.size(); ++i)
            QVERIFY(std::abs(fs.onset[i] - fm.onset[i]) < 1e-4f);
    }

    void fromFiles()
    {
        // Two WAV files of the same scene, 3.5 s apart, through the real decoder.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::vector<float> src = scene(30.0, 21);
        auto writeWav = [](const QString& path, const std::vector<float>& s) {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly))
                return false;
            auto u32 = [&f](quint32 v) { f.write(reinterpret_cast<const char*>(&v), 4); };
            auto u16 = [&f](quint16 v) { f.write(reinterpret_cast<const char*>(&v), 2); };
            const auto bytes = quint32(s.size() * 2);
            f.write("RIFF");
            u32(36 + bytes);
            f.write("WAVEfmt ");
            u32(16);
            u16(1);
            u16(1);
            u32(kRate);
            u32(kRate * 2);
            u16(2);
            u16(16);
            f.write("data");
            u32(bytes);
            for (const float x : s)
                u16(quint16(qint16(std::clamp(x, -1.0f, 1.0f) * 32767.0f)));
            return true;
        };
        const QString a = dir.filePath(QStringLiteral("a.wav"));
        const QString b = dir.filePath(QStringLiteral("b.wav"));
        QVERIFY(writeWav(a, record(src, 0.0, 20.0, 1.0, 0.003, 1)));
        QVERIFY(writeWav(b, record(src, 3.5, 15.0, 0.5, 0.003, 2)));
        QString error;
        const auto fa = syncFeaturesFromFile(a, nullptr, &error);
        QVERIFY2(fa, qPrintable(error));
        const auto fb = syncFeaturesFromFile(b, nullptr, &error);
        QVERIFY2(fb, qPrintable(error));
        QVERIFY(std::abs(fa->seconds() - 20.0) < 0.01);
        const SyncMatch m = findSyncOffset(*fa, *fb);
        QVERIFY(m.found);
        QVERIFY(std::abs(m.offsetSeconds - 3.5) < kTolerance);
        // Cancel and missing files fail cleanly.
        std::atomic<bool> cancel {true};
        QVERIFY(!syncFeaturesFromFile(a, &cancel, &error));
        QVERIFY(
            !syncFeaturesFromFile(dir.filePath(QStringLiteral("missing.wav")), nullptr, &error));
    }
};

QTEST_GUILESS_MAIN(TestAudioSync)
#include "test_audiosync.moc"
