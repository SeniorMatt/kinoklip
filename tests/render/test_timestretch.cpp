// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the WSOLA time stretch: length, pitch, tempo, unity at 100 %, determinism.
#include "render/TimeStretch.h"

#include <QElapsedTimer>
#include <QTest>

#include <cmath>
#include <numbers>
#include <vector>

using namespace kk::render;

namespace {

constexpr double kRate = 48000.0;

/// Synthetic stereo source: a sine of @p hz, plus an optional click (a 32-sample burst of 1.0)
/// at source sample @p click.
SourceCache::Reader sineReader(double hz, int64_t click = -1)
{
    return [hz, click](int64_t first, float* out, int count) {
        for (int i = 0; i < count; ++i) {
            const int64_t n = first + i;
            float v = float(0.5 * std::sin(2.0 * std::numbers::pi * hz * double(n) / kRate));
            if (click >= 0 && n >= click && n < click + 32)
                v = 1.0f;
            out[2 * i] = v;
            out[2 * i + 1] = v;
        }
    };
}

std::vector<float> render(
    double speed, int64_t from, int frames, const SourceCache::Reader& read, int chunk = 0)
{
    TimeStretch stretch;
    SourceCache cache;
    std::vector<float> out(size_t(frames) * 2);
    const TimeStretch::Position pos = [speed](int64_t o) { return double(o) * speed; };
    if (chunk <= 0)
        chunk = frames;
    for (int done = 0; done < frames; done += chunk) {
        const int n = std::min(chunk, frames - done);
        stretch.render(from + done, n, pos, cache, read, out.data() + size_t(done) * 2);
    }
    return out;
}

/// Zero crossings per second of the left channel over [begin, end).
double crossingsPerSecond(const std::vector<float>& s, int begin, int end)
{
    int crossings = 0;
    for (int i = begin + 1; i < end; ++i) {
        if ((s[size_t(2 * (i - 1))] < 0.0f) != (s[size_t(2 * i)] < 0.0f))
            ++crossings;
    }
    return crossings * kRate / double(end - begin);
}

double rms(const std::vector<float>& s, int begin, int end)
{
    double sum = 0.0;
    for (int i = begin; i < end; ++i)
        sum += double(s[size_t(2 * i)]) * s[size_t(2 * i)];
    return std::sqrt(sum / double(end - begin));
}

} // namespace

class TestTimeStretch : public QObject
{
    Q_OBJECT
private slots:
    void unityAtNormalSpeed()
    {
        const auto read = sineReader(440.0);
        const std::vector<float> out = render(1.0, 0, 9600, read);
        std::vector<float> ref(9600 * 2);
        read(0, ref.data(), 9600);
        for (int i = 2048; i < 9600; ++i)
            QVERIFY(std::abs(out[size_t(2 * i)] - ref[size_t(2 * i)]) < 1e-5f);
    }

    void pitchStaysAtHalfAndDoubleSpeed()
    {
        for (const double speed : {0.5, 0.75, 1.5, 2.0}) {
            const std::vector<float> out = render(speed, 0, 48000, sineReader(440.0));
            // 440 Hz = 880 zero crossings per second, whatever the speed (resampling would
            // give 880 * speed).
            const double cps = crossingsPerSecond(out, 4800, 43200);
            QVERIFY2(std::abs(cps - 880.0) < 880.0 * 0.03,
                qPrintable(QStringLiteral("speed %1: %2 crossings/s").arg(speed).arg(cps)));
            // No dropouts: every 10 ms window keeps most of the sine's energy (0.5 / sqrt 2).
            for (int w = 4800; w + 480 <= 43200; w += 480)
                QVERIFY2(rms(out, w, w + 480) > 0.25, qPrintable(QString::number(w)));
        }
    }

    void tempoFollowsTheSpeed()
    {
        // A click at source 24000 plays at output 48000 at 50 % (within the alignment range).
        const std::vector<float> out = render(0.5, 0, 72000, sineReader(220.0, 24000));
        int peak = -1;
        for (int i = 0; i < 72000; ++i) {
            if (out[size_t(2 * i)] > 0.9f) {
                peak = i;
                break;
            }
        }
        QVERIFY(peak >= 0);
        QVERIFY2(std::abs(peak - 48000) < TimeStretch::kTolerance + TimeStretch::kWindow,
            qPrintable(QString::number(peak)));
    }

    void cost()
    {
        // Measured: ~12 ms per second of 48 kHz stereo at 50 % on one core (Release build,
        // including the synthetic source).
        QElapsedTimer timer;
        timer.start();
        const std::vector<float> out = render(0.5, 0, 480000, sineReader(440.0));
        qInfo("time stretch: %.1f ms per second of output", double(timer.elapsed()) / 10.0);
        QVERIFY(!out.empty());
    }

    void randomAccessIsDeterministic()
    {
        const auto read = sineReader(330.0);
        const std::vector<float> whole = render(0.6, 0, 40000, read);
        const std::vector<float> chunks = render(0.6, 0, 40000, read, 777);
        QCOMPARE(chunks, whole);
        // Starting in the middle (a seek) gives the same samples.
        const std::vector<float> late = render(0.6, 20000, 20000, read, 1024);
        for (int i = 0; i < 20000; ++i)
            QCOMPARE(late[size_t(2 * i)], whole[size_t(2 * (20000 + i))]);
    }
};

QTEST_GUILESS_MAIN(TestTimeStretch)
#include "test_timestretch.moc"
