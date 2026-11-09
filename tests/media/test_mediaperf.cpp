// SPDX-License-Identifier: GPL-3.0-or-later
// test_mediaperf — performance sanity: decode throughput, seek latency, thumbnails, per backend.
// Prints measurements; asserts only generous limits (shared CI machines vary a lot).
#include "MediaTestUtil.h"
#include "media/AudioDecoder.h"
#include "media/HwAccel.h"
#include "media/MediaCache.h"
#include "media/MediaInfo.h"
#include "media/VideoDecoder.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <vector>

using namespace kk;
using namespace kk::media;

namespace {

/// Frames decoded for the sequential measurements (8 s of 25 fps 1080p in the fixture).
constexpr int kSequentialFrames = 200;
/// Random seeks for the latency measurement.
constexpr int kSeekCount = 20;

double millis(const QElapsedTimer& timer)
{
    return double(timer.nsecsElapsed()) / 1e6;
}

} // namespace

class TestMediaPerf : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QString why;
        if (!test::ensureFixtures(&why)) {
            QSKIP(qPrintable(why));
        }
        for (const HwDeviceInfo& device : hwCapabilities()) {
            qInfo().noquote() << "backend" << device.name
                              << (device.available ? "available" : "unavailable") << device.device
                              << device.error << device.codecs.join(QLatin1Char(','));
        }
    }

    void decoding_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<QString>("fixture");
        QTest::newRow("software h264 1080p")
            << int(HwAccelMode::Off) << QStringLiteral("perf_1080p.mp4");
        QTest::newRow("software hevc10 720p")
            << int(HwAccelMode::Off) << QStringLiteral("hevc10.mp4");
        for (const HwDeviceInfo& device : hwCapabilities()) {
            if (device.available) {
                QTest::addRow("%s h264 1080p", qPrintable(device.name))
                    << int(device.mode) << QStringLiteral("perf_1080p.mp4");
                QTest::addRow("%s hevc10 720p", qPrintable(device.name))
                    << int(device.mode) << QStringLiteral("hevc10.mp4");
            }
        }
    }

    /// Sequential decodeFrame() (playback path), frameAt() at viewer size, random seeks.
    void decoding()
    {
        QFETCH(int, mode);
        QFETCH(QString, fixture);
        const QByteArray name = fixture.toLatin1();
        VideoDecoder decoder;
        decoder.setHwAccel(HwAccelMode(mode));
        QElapsedTimer timer;
        timer.start();
        QString error;
        QVERIFY2(decoder.open(test::fixture(name.constData()), &error), qPrintable(error));
        const double openMs = millis(timer);
        const MediaInfo& info = decoder.info();
        const int64_t frameUs = 1'000'000 * info.frameRate.den / info.frameRate.num;
        const int frames = int(std::min<int64_t>(kSequentialFrames, info.durationUs / frameUs));

        timer.restart();
        for (int k = 0; k < frames; ++k) {
            const auto frame = decoder.decodeFrame(k * frameUs + frameUs / 2);
            QVERIFY(frame);
        }
        const double rawFps = frames * 1000.0 / millis(timer);
        const auto sample = decoder.decodeFrame(0);
        QVERIFY(sample);

        VideoDecoder viewer;
        viewer.setHwAccel(HwAccelMode(mode));
        QVERIFY(viewer.open(test::fixture(name.constData())));
        timer.restart();
        for (int k = 0; k < frames; ++k) {
            QCOMPARE(
                test::readFrameIndex(viewer.frameAt(k * frameUs + frameUs / 2, {960, 540})), k);
        }
        const double imageFps = frames * 1000.0 / millis(timer);

        QRandomGenerator rng(5);
        double totalSeekMs = 0;
        double maxSeekMs = 0;
        for (int i = 0; i < kSeekCount; ++i) {
            const int64_t t = rng.bounded(qint64(0), qint64(info.durationUs - frameUs));
            timer.restart();
            const QImage image = viewer.frameAt(t, {960, 540});
            const double ms = millis(timer);
            QCOMPARE(test::readFrameIndex(image), int(t / frameUs));
            totalSeekMs += ms;
            maxSeekMs = std::max(maxSeekMs, ms);
        }
        qInfo().noquote() << QStringLiteral(
            "%1 [%2, %3 %4]: open %5 ms | decodeFrame %6 fps | frameAt "
            "960x540 %7 fps | random seek avg %8 ms, max %9 ms")
                                 .arg(QString::fromLatin1(QTest::currentDataTag()),
                                     hwAccelName(decoder.activeHwAccel()),
                                     QString::fromLatin1(pixelFormatName(sample->format)),
                                     QStringLiteral("%1x%2").arg(sample->width).arg(sample->height))
                                 .arg(openMs, 0, 'f', 1)
                                 .arg(rawFps, 0, 'f', 1)
                                 .arg(imageFps, 0, 'f', 1)
                                 .arg(totalSeekMs / kSeekCount, 0, 'f', 1)
                                 .arg(maxSeekMs, 0, 'f', 1);
        // Generous sanity limits only.
        QVERIFY(rawFps > 10);
        QVERIFY(totalSeekMs / kSeekCount < 1000);
    }

    void audioDecodeSpeed()
    {
        AudioDecoder decoder;
        QVERIFY(decoder.open(test::fixture("h264_25fps.mp4")));
        std::vector<float> buffer(4096 * 2);
        QElapsedTimer timer;
        timer.start();
        const int64_t length = decoder.lengthSamples();
        for (int64_t pos = 0; pos < length; pos += 4096) {
            decoder.read(pos, buffer.data(), 4096);
        }
        const double realtime = (double(length) / 48000.0) / (millis(timer) / 1000.0);
        qInfo("AAC decode + resample: %.0fx real time", realtime);
        QVERIFY(realtime > 5);
    }

    void thumbnailLatency()
    {
        QTemporaryDir cacheDir;
        MediaCache& cache = MediaCache::instance();
        cache.setCacheDirectory(cacheDir.path());
        QSignalSpy spy(&cache, &MediaCache::thumbnailReady);
        const QString path = test::fixture("perf_1080p.mp4");
        QElapsedTimer timer;
        timer.start();
        QVERIFY(cache.thumbnail(path, 4'000'000, 180).isNull());
        QVERIFY(spy.wait(20000));
        const double coldMs = millis(timer);
        QVERIFY(!cache.thumbnail(path, 4'000'000, 180).isNull());
        // Neighbouring thumbnails reuse the open decoder (decode forward).
        timer.restart();
        for (int i = 1; i <= 10; ++i) {
            cache.thumbnail(path, 4'000'000 + i * 300'000, 180);
        }
        while (spy.count() < 11 && spy.wait(20000)) { }
        const double tenMs = millis(timer);
        qInfo("thumbnail (cold, 1080p H.264): first %.1f ms, next 10 sequential %.1f ms", coldMs,
            tenMs);
        QCOMPARE(spy.count(), 11);
    }

    /// A timeline filmstrip of a cold file (20 thumbnails 64 px high spread over the clip,
    /// requested at once like a first paint), with software and with hardware preview
    /// decoders (MediaCache::setPreviewDecoding(); docs/archive/polish-v3 Round 3). KK_PERF_MEDIA adds files.
    void filmstripLatency_data()
    {
        QTest::addColumn<QString>("path");
        QStringList files {test::fixture("perf_1080p.mp4")};
        files += qEnvironmentVariable("KK_PERF_MEDIA").split(QLatin1Char(':'), Qt::SkipEmptyParts);
        for (const QString& f : files)
            QTest::newRow(qPrintable(QFileInfo(f).fileName())) << f;
    }

    void filmstripLatency()
    {
        QFETCH(QString, path);
        constexpr int kThumbs = 20;
        MediaCache& cache = MediaCache::instance();
        double ms[2] = {0, 0};
        const MediaCache::PreviewDecoding modes[2]
            = {MediaCache::PreviewDecoding::Software, MediaCache::PreviewDecoding::Hardware};
        for (int m = 0; m < 2; ++m) {
            QTemporaryDir cacheDir; // cold every time
            cache.setCacheDirectory(cacheDir.path());
            cache.clearMemoryCache();
            cache.setPreviewDecoding(modes[m]);
            QSignalSpy spy(&cache, &MediaCache::thumbnailReady);
            const int64_t length = std::max<int64_t>(1'000'000, probe(path).durationUs);
            QElapsedTimer timer;
            timer.start();
            auto missing = [&] {
                int n = 0;
                for (int i = 0; i < kThumbs; ++i)
                    n += cache.thumbnail(path, length * i / kThumbs, 64).isNull() ? 1 : 0;
                return n;
            };
            missing(); // requests all of them at once, like a first paint
            while (missing() > 0 && spy.wait(20000)) { }
            ms[m] = millis(timer);
            QCOMPARE(missing(), 0);
        }
        cache.setPreviewDecoding(MediaCache::PreviewDecoding::BySize);
        qInfo("filmstrip of 20 (cold, %s): software %.0f ms, hardware %.0f ms",
            qPrintable(QFileInfo(path).fileName()), ms[0], ms[1]);
    }

    void cleanupTestCase() { MediaCache::instance().shutdown(); }
};

QTEST_GUILESS_MAIN(TestMediaPerf)
#include "test_mediaperf.moc"
