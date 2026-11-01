// SPDX-License-Identifier: GPL-3.0-or-later
// test_mediacache — asynchronous thumbnails/posters/waveforms, dedupe, disk cache, shutdown.
#include "MediaTestUtil.h"
#include "media/MediaCache.h"

#include <QDirIterator>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

using namespace cw;
using namespace cw::media;

namespace {

/// Generous wait for background work (first-time decoder/GPU initialisation included).
constexpr int kWaitMs = 30000;

int countFiles(const QString& directory)
{
    int count = 0;
    QDirIterator it(directory, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

} // namespace

class TestMediaCache : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_cacheDir;
    MediaCache* m_cache = nullptr;

private slots:
    void initTestCase()
    {
        QString why;
        if (!test::ensureFixtures(&why)) {
            QSKIP(qPrintable(why));
        }
        QVERIFY(m_cacheDir.isValid());
        m_cache = &MediaCache::instance();
        m_cache->setCacheDirectory(m_cacheDir.path());
        QCOMPARE(m_cache->cacheDirectory(), m_cacheDir.path());
    }

    void snapping()
    {
        QCOMPARE(MediaCache::snapThumbnailHeight(64), 64);
        QCOMPARE(MediaCache::snapThumbnailHeight(65), 72);
        QCOMPARE(MediaCache::snapThumbnailHeight(1), 8);
        QCOMPARE(MediaCache::snapThumbnailHeight(100000), 2160);
        QCOMPARE(MediaCache::snapThumbnailTimeUs(1'240'000, 64), 1'000'000);
        QCOMPARE(MediaCache::snapThumbnailTimeUs(1'260'000, 64), 1'500'000);
        QCOMPARE(MediaCache::snapThumbnailTimeUs(1'260'000, 240), 1'300'000);
        QCOMPARE(MediaCache::snapThumbnailTimeUs(-5, 64), 0);
    }

    void thumbnailIsDeliveredAsynchronously()
    {
        const QString path = test::fixture("h264_25fps.mp4");
        QSignalSpy spy(m_cache, &MediaCache::thumbnailReady);
        QVERIFY(m_cache->thumbnail(path, 4'000'000, 180).isNull()); // scheduled
        // Identical requests while in flight are deduplicated.
        for (int i = 0; i < 5; ++i) {
            QVERIFY(m_cache->thumbnail(path, 4'020'000, 180).isNull());
        }
        QVERIFY(spy.wait(kWaitMs));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), path);
        const QImage thumb = m_cache->thumbnail(path, 4'000'000, 180);
        QVERIFY(!thumb.isNull());
        QCOMPARE(thumb.height(), MediaCache::snapThumbnailHeight(180)); // 184: multiple of 8
        QCOMPARE(thumb.width(), 327);
        QCOMPARE(thumb.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(test::readFrameIndex(thumb), 100); // 4.0 s × 25 fps
        QTest::qWait(200);
        QCOMPARE(spy.count(), 1); // no duplicate deliveries
    }

    void thumbnailComesBackFromDisk()
    {
        const QString path = test::fixture("h264_25fps.mp4");
        QVERIFY(countFiles(m_cacheDir.path() + QStringLiteral("/thumbs")) >= 1);
        m_cache->clearMemoryCache();
        QSignalSpy spy(m_cache, &MediaCache::thumbnailReady);
        QVERIFY(m_cache->thumbnail(path, 4'000'000, 180).isNull()); // disk load is async too
        QVERIFY(spy.wait(kWaitMs));
        const QImage thumb = m_cache->thumbnail(path, 4'000'000, 180);
        QVERIFY(!thumb.isNull());
        QCOMPARE(test::readFrameIndex(thumb), 100); // JPEG round trip keeps the barcode
    }

    void stillAndAlphaThumbnails()
    {
        QSignalSpy spy(m_cache, &MediaCache::thumbnailReady);
        const QString png = test::fixture("still_alpha.png");
        QVERIFY(m_cache->thumbnail(png, 7'000'000, 24).isNull());
        QVERIFY(spy.wait(kWaitMs));
        const QImage thumb = m_cache->thumbnail(png, 0, 24); // stills ignore the time
        QVERIFY(!thumb.isNull());
        QCOMPARE(thumb.size(), QSize(32, 24));
        QCOMPARE(qAlpha(thumb.pixel(28, 12)), 0); // alpha kept (PNG on disk)
        QCOMPARE(qAlpha(thumb.pixel(4, 12)), 255);
    }

    void posterFrame()
    {
        const QString path = test::fixture("h264_25fps.mp4");
        QSignalSpy spy(m_cache, &MediaCache::posterFrameReady);
        QVERIFY(m_cache->posterFrame(path, 180).isNull());
        QVERIFY(spy.wait(kWaitMs));
        const QImage poster = m_cache->posterFrame(path, 180);
        QVERIFY(!poster.isNull());
        QCOMPARE(test::readFrameIndex(poster), 25); // 10 % of 10 s = frame 25
    }

    void waveform()
    {
        const QString path = test::fixture("h264_25fps.mp4");
        QSignalSpy spy(m_cache, &MediaCache::waveformReady);
        QVERIFY(m_cache->waveform(path) == nullptr);
        QVERIFY(spy.wait(kWaitMs));
        const std::shared_ptr<const WaveformData> data = m_cache->waveform(path);
        QVERIFY(data);
        QCOMPARE(data->peaksPerSecond, 100);
        QVERIFY(qAbs(data->size() - 1000) <= 2); // 10 s
        QCOMPARE(data->maxL.size(), data->size());
        // 440 Hz sine at amplitude 1/8, upmixed to stereo by ffmpeg's `-ac 2` at -3 dB
        // → peaks ≈ ±0.125 × 0.707 × 127 ≈ ±11.
        const int maxL = *std::max_element(data->maxL.begin(), data->maxL.end());
        const int minL = *std::min_element(data->minL.begin(), data->minL.end());
        QVERIFY2(maxL >= 9 && maxL <= 14, qPrintable(QString::number(maxL)));
        QVERIFY2(minL <= -9 && minL >= -14, qPrintable(QString::number(minL)));
        // Same sine on both channels (AAC stereo coding may differ by a quantisation step).
        for (int i = 0; i < data->size(); ++i) {
            QVERIFY(qAbs(data->maxR[i] - data->maxL[i]) <= 2);
        }
        QVERIFY(countFiles(m_cacheDir.path() + QStringLiteral("/waveforms")) == 1);

        // From disk after clearing memory.
        m_cache->clearMemoryCache();
        QVERIFY(m_cache->waveform(path) == nullptr);
        QVERIFY(spy.wait(kWaitMs));
        const std::shared_ptr<const WaveformData> reloaded = m_cache->waveform(path);
        QVERIFY(reloaded);
        QCOMPARE(reloaded->maxL, data->maxL);
        QCOMPARE(reloaded->minR, data->minR);
    }

    void failuresAreRememberedAndQuiet()
    {
        QSignalSpy thumbSpy(m_cache, &MediaCache::thumbnailReady);
        QSignalSpy waveSpy(m_cache, &MediaCache::waveformReady);
        const QString corrupt = test::fixture("corrupt.mp4");
        const QString silent = test::fixture("h264_60fps.mp4"); // no audio
        QVERIFY(m_cache->thumbnail(corrupt, 0, 64).isNull());
        QVERIFY(m_cache->waveform(silent) == nullptr);
        QVERIFY(m_cache->thumbnail(QString(), 0, 64).isNull());
        QVERIFY(m_cache->thumbnail(corrupt, 0, 0).isNull());
        QTest::qWait(1500);
        QCOMPARE(thumbSpy.count(), 0);
        QCOMPARE(waveSpy.count(), 0);
        QVERIFY(m_cache->thumbnail(corrupt, 0, 64).isNull());
        QVERIFY(m_cache->waveform(silent) == nullptr);
    }

    void invalidate()
    {
        const QString path = test::fixture("h264_25fps.mp4");
        QSignalSpy spy(m_cache, &MediaCache::thumbnailReady);
        if (m_cache->thumbnail(path, 4'000'000, 180).isNull()) {
            QVERIFY(spy.wait(kWaitMs));
        }
        QVERIFY(!m_cache->thumbnail(path, 4'000'000, 180).isNull());
        m_cache->invalidate(path);
        QVERIFY(m_cache->thumbnail(path, 4'000'000, 180).isNull()); // re-requested
        QVERIFY(spy.wait(kWaitMs));
        QVERIFY(!m_cache->thumbnail(path, 4'000'000, 180).isNull());
    }

    void shutdownCancelsQueuedWork()
    {
        const QString path = test::fixture("perf_1080p.mp4");
        for (int i = 0; i < 40; ++i) {
            m_cache->thumbnail(path, i * 190'000, 360);
        }
        m_cache->waveform(test::fixture("h264_2997.mp4"));
        QElapsedTimer timer;
        timer.start();
        m_cache->shutdown(); // must not hang; queued jobs are dropped
        QVERIFY(timer.elapsed() < 10000);
        QVERIFY(m_cache->thumbnail(path, 7'000'000, 360).isNull());
        QSignalSpy spy(m_cache, &MediaCache::thumbnailReady);
        QVERIFY(!spy.wait(300)); // nothing new is scheduled after shutdown
    }
};

QTEST_GUILESS_MAIN(TestMediaCache)
#include "test_mediacache.moc"
