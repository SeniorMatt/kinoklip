// SPDX-License-Identifier: GPL-3.0-or-later
// test_audiodecoder — sample-accurate random/contiguous reads, resampling, zero-fill, errors.
#include "MediaTestUtil.h"
#include "media/AudioDecoder.h"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTest>

#include <cmath>
#include <vector>

using namespace cw;
using namespace cw::media;

namespace {

/// Value of sample `n` in mono_ramp_48k.wav: ((n mod 4800) / 4800) * 2 - 1.
float rampValue(int64_t n)
{
    return float(double(n % 4800) / 4800.0 * 2.0 - 1.0);
}

/// 16-bit quantisation of the WAV plus float rounding.
constexpr float kRampTolerance = 1e-4f;

/// Checks `frames` stereo frames read at `start` against the ramp; returns "" or a message.
QString checkRamp(const std::vector<float>& buffer, int64_t start, int frames, int64_t length)
{
    for (int i = 0; i < frames; ++i) {
        const int64_t n = start + i;
        const float expected = (n < 0 || n >= length) ? 0.0f : rampValue(n);
        const float left = buffer[size_t(i) * 2];
        const float right = buffer[size_t(i) * 2 + 1];
        if (std::abs(left - expected) > kRampTolerance || left != right) {
            return QStringLiteral("sample %1 (read at %2): expected %3, got L=%4 R=%5")
                .arg(n)
                .arg(start)
                .arg(expected)
                .arg(left)
                .arg(right);
        }
    }
    return {};
}

/// Decodes the whole file contiguously (the reference for random-access comparisons).
std::vector<float> decodeAll(AudioDecoder& decoder, int64_t frames)
{
    std::vector<float> all(size_t(frames) * 2);
    constexpr int kChunk = 4096;
    for (int64_t pos = 0; pos < frames; pos += kChunk) {
        const int n = int(std::min<int64_t>(kChunk, frames - pos));
        decoder.read(pos, all.data() + pos * 2, n);
    }
    return all;
}

} // namespace

class TestAudioDecoder : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QString why;
        if (!test::ensureFixtures(&why)) {
            QSKIP(qPrintable(why));
        }
    }

    void openFailures_data()
    {
        QTest::addColumn<QString>("path");
        QTest::newRow("video only") << test::fixture("h264_60fps.mp4");
        QTest::newRow("still") << test::fixture("still.jpg");
        QTest::newRow("random bytes") << test::fixture("corrupt.mp4");
        QTest::newRow("empty") << test::fixture("empty.mov");
        QTest::newRow("missing") << test::fixture("missing.wav");
    }

    void openFailures()
    {
        QFETCH(QString, path);
        AudioDecoder decoder;
        QString error;
        QVERIFY(!decoder.open(path, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!decoder.isOpen());
        std::vector<float> buffer(64, 1.0f);
        QCOMPARE(decoder.read(0, buffer.data(), 32), 32);
        QCOMPARE(buffer[0], 0.0f); // not open → silence
    }

    /// Random positions and sizes: every sample must be exactly the one at that position.
    void randomReadsAreSampleAccurate()
    {
        AudioDecoder decoder;
        QString error;
        QVERIFY2(decoder.open(test::fixture("mono_ramp_48k.wav"), &error), qPrintable(error));
        const int64_t length = 5 * 48000;
        QCOMPARE(decoder.lengthSamples(), length);
        QRandomGenerator rng(7);
        std::vector<float> buffer;
        for (int i = 0; i < 200; ++i) {
            const int64_t start = rng.bounded(qint64(-2000), qint64(length + 2000));
            const int frames = int(rng.bounded(1, 6000));
            buffer.assign(size_t(frames) * 2, -42.0f);
            QCOMPARE(decoder.read(start, buffer.data(), frames), frames);
            const QString problem = checkRamp(buffer, start, frames, length);
            QVERIFY2(problem.isEmpty(), qPrintable(problem));
        }
    }

    void contiguousReads()
    {
        AudioDecoder decoder;
        QVERIFY(decoder.open(test::fixture("mono_ramp_48k.wav")));
        const int64_t length = 5 * 48000;
        std::vector<float> buffer(1024 * 2);
        QElapsedTimer timer;
        timer.start();
        for (int64_t pos = 0; pos < length; pos += 1024) {
            decoder.read(pos, buffer.data(), 1024);
            const QString problem = checkRamp(buffer, pos, 1024, length);
            QVERIFY2(problem.isEmpty(), qPrintable(problem));
        }
        qInfo("contiguous 1024-frame reads of 5 s WAV: %lld ms", timer.elapsed());
    }

    void zeroFillOutsideMedia()
    {
        AudioDecoder decoder;
        QVERIFY(decoder.open(test::fixture("mono_ramp_48k.wav")));
        const int64_t length = 5 * 48000;
        std::vector<float> buffer(1000 * 2, 5.0f);
        // Straddling the end: 100 real samples, then silence.
        decoder.read(length - 100, buffer.data(), 1000);
        QVERIFY(checkRamp(buffer, length - 100, 1000, length).isEmpty());
        QVERIFY(buffer[99 * 2] != 0.0f);
        QCOMPARE(buffer[100 * 2], 0.0f);
        // Entirely past the end.
        std::fill(buffer.begin(), buffer.end(), 5.0f);
        decoder.read(length * 3, buffer.data(), 1000);
        QVERIFY(std::all_of(buffer.begin(), buffer.end(), [](float v) { return v == 0.0f; }));
        // Straddling the start.
        decoder.read(-300, buffer.data(), 1000);
        QVERIFY(checkRamp(buffer, -300, 1000, length).isEmpty());
        QCOMPARE(buffer[299 * 2], 0.0f);
        QCOMPARE(buffer[300 * 2], -1.0f);
        QCOMPARE(decoder.read(0, buffer.data(), 0), 0);
    }

    /// 44.1 kHz impulses must land on the matching 48 kHz sample (resampler delay compensated).
    void resampledImpulsePositions()
    {
        AudioDecoder decoder;
        QVERIFY(decoder.open(test::fixture("impulses_44k.wav")));
        QCOMPARE(decoder.info().audioSampleRate, 44100);
        const int64_t length = 3 * 48000;
        std::vector<float> all = decodeAll(decoder, length);
        for (int k = 0; k < 6; ++k) {
            const double expected = (22050.0 * k + 1000.0) * 48000.0 / 44100.0;
            int64_t best = -1;
            float bestValue = 0;
            for (int64_t n = int64_t(expected) - 50; n <= int64_t(expected) + 50; ++n) {
                if (all[size_t(n) * 2] > bestValue) {
                    bestValue = all[size_t(n) * 2];
                    best = n;
                }
            }
            QVERIFY2(std::abs(double(best) - expected) <= 1.0,
                qPrintable(QStringLiteral("impulse %1: expected %2, found %3")
                               .arg(k)
                               .arg(expected)
                               .arg(best)));
            // Random access to the impulse gives the same samples as the contiguous decode.
            AudioDecoder random;
            QVERIFY(random.open(test::fixture("impulses_44k.wav")));
            std::vector<float> window(64 * 2);
            random.read(best - 32, window.data(), 64);
            for (int i = 0; i < 64; ++i) {
                QVERIFY(std::abs(window[size_t(i) * 2] - all[size_t(best - 32 + i) * 2]) < 1e-4f);
            }
        }
    }

    void randomMatchesContiguous_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("aac 48k stereo") << QStringLiteral("h264_25fps.mp4");
        QTest::newRow("aac 44.1k mono") << QStringLiteral("h264_2997.mp4");
        QTest::newRow("mp3 44.1k") << QStringLiteral("sine.mp3");
    }

    /// For compressed formats the reference is a contiguous decode from the start; random
    /// reads (seek + pre-roll + discard) must reproduce it.
    void randomMatchesContiguous()
    {
        QFETCH(QString, name);
        AudioDecoder reference;
        QString error;
        QVERIFY2(
            reference.open(test::fixture(name.toLatin1().constData()), &error), qPrintable(error));
        const int64_t length = reference.lengthSamples();
        QVERIFY(length > 48000);
        const std::vector<float> all = decodeAll(reference, length);
        // The test tones are not silent.
        QVERIFY(*std::max_element(all.begin(), all.end()) > 0.05f);

        AudioDecoder decoder;
        QVERIFY(decoder.open(test::fixture(name.toLatin1().constData())));
        QRandomGenerator rng(11);
        std::vector<float> buffer;
        for (int i = 0; i < 25; ++i) {
            const int64_t start = rng.bounded(qint64(0), qint64(length - 5000));
            const int frames = int(rng.bounded(1, 4096));
            buffer.assign(size_t(frames) * 2, 0.0f);
            decoder.read(start, buffer.data(), frames);
            float maxDiff = 0;
            for (size_t j = 0; j < buffer.size(); ++j) {
                maxDiff = std::max(maxDiff, std::abs(buffer[j] - all[size_t(start) * 2 + j]));
            }
            QVERIFY2(maxDiff < 1e-4f,
                qPrintable(QStringLiteral("read at %1 (%2 frames): diff %3")
                               .arg(start)
                               .arg(frames)
                               .arg(maxDiff)));
        }
    }

    void monoIsCopiedToBothChannelsAtUnityGain()
    {
        AudioDecoder decoder;
        QVERIFY(decoder.open(test::fixture("mono_ramp_48k.wav")));
        std::vector<float> buffer(10 * 2);
        decoder.read(2400, buffer.data(), 10); // ramp value 0 at 2400
        QVERIFY(std::abs(buffer[0]) < kRampTolerance);
        decoder.read(4799, buffer.data(), 1); // the ramp's maximum, ~+1.0
        QVERIFY(buffer[0] > 0.999f && buffer[1] == buffer[0]);
    }

    void lengthAndInfo()
    {
        AudioDecoder decoder;
        QVERIFY(decoder.open(test::fixture("h264_25fps.mp4")));
        QVERIFY(decoder.info().hasAudio);
        QVERIFY(qAbs(decoder.lengthSamples() - 480000) < 2048);
        AudioDecoder mp3;
        QVERIFY(mp3.open(test::fixture("sine.mp3")));
        QVERIFY(qAbs(mp3.lengthSamples() - 3 * 48000) < 2048);
    }
};

QTEST_GUILESS_MAIN(TestAudioDecoder)
#include "test_audiodecoder.moc"
