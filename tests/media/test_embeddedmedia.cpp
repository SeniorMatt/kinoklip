// SPDX-License-Identifier: GPL-3.0-or-later
// test_embeddedmedia — media decoded in place from inside a single-file project: probing, frame
// exact video seeking (identical to the same file outside), hardware decoding, audio samples,
// still images of every backend (Qt, libheif, FFmpeg, LibRaw), animations, thumbnails and
// waveforms through MediaCache, and many decoders reading one container at once.
#include "MediaTestUtil.h"
#include "media/AudioDecoder.h"
#include "media/HwAccel.h"
#include "media/ImageLoader.h"
#include "media/MediaCache.h"
#include "media/MediaInfo.h"
#include "media/MediaSource.h"
#include "media/VideoDecoder.h"
#include "media/WaveformData.h"
#include "pack/EmbeddedMedia.h"
#include "pack/ProjectPackage.h"

#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrent>

#include <atomic>

using namespace kk;
using namespace kk::media;

class TestEmbeddedMedia : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QTemporaryDir m_cacheDir;
    QString m_skip;
    QString m_container;
    QHash<QString, QString> m_embedded; ///< original path -> embedded path

    QString embedded(const QString& original) const { return m_embedded.value(original); }

    static QStringList sources()
    {
        QStringList list;
        for (const char* name : {"h264_25fps.mp4", "vfr_odd.mp4", "hevc10.mp4", "h264_25fps.ts",
                 "sine.mp3", "impulses_44k.wav", "still.jpg", "still_alpha.png"}) {
            list.append(test::fixture(name));
        }
        for (const char* name :
            {"still.heic", "still.avif", "still.exr", "still.dpx", "raw_rggb.dng", "anim.gif",
                "anim.webp", "still.webp", "still.tiff", "still.svg"}) {
            const QString p
                = test::fixtureDir() + QStringLiteral("/formats/") + QLatin1String(name);
            if (QFileInfo::exists(p))
                list.append(p);
        }
        return list;
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (!test::ensureFixtures(&m_skip))
            return;
        m_container = m_dir.filePath(QStringLiteral("media.kinoklip"));
        QString error;
        auto package = pack::ProjectPackage::create(m_container, &error);
        QVERIFY2(package, qPrintable(error));
        QVector<pack::ProjectPackage::Added> added;
        QCOMPARE(package->addMedia(sources(), &added, {}, &error), pack::Status::Ok);
        for (const auto& a : std::as_const(added))
            m_embedded.insert(a.source, a.path);
        QVERIFY(m_cacheDir.isValid());
        MediaCache::instance().setCacheDirectory(m_cacheDir.path());
    }

    void cleanupTestCase() { MediaCache::instance().shutdown(); }

    void sourceResolution()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        const QString clip = embedded(test::fixture("h264_25fps.mp4"));
        const MediaSource s = MediaSource::resolve(clip);
        QVERIFY(s.exists());
        QVERIFY(s.isEmbedded());
        QCOMPARE(s.size(), QFileInfo(test::fixture("h264_25fps.mp4")).size());
        QVERIFY(mediaExists(clip));
        QVERIFY(!mediaExists(m_container + QStringLiteral("/media/nothing.mp4")));
        QVERIFY(!probe(m_container + QStringLiteral("/media/nothing.mp4")).valid);
        const MediaSource plain = MediaSource::resolve(test::fixture("h264_25fps.mp4"));
        QVERIFY(plain.exists() && !plain.isEmbedded());
        QVERIFY(plain.identity() != s.identity());
        std::unique_ptr<QIODevice> device = s.openDevice();
        QVERIFY(device);
        QFile original(test::fixture("h264_25fps.mp4"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(device->readAll(), original.readAll());
    }

    void probeMatches()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        for (auto it = m_embedded.cbegin(); it != m_embedded.cend(); ++it) {
            const MediaInfo a = probe(it.key());
            const MediaInfo b = probe(it.value());
            const QByteArray what = QFileInfo(it.key()).fileName().toUtf8();
            QVERIFY2(b.valid == a.valid, what + ": " + b.error.toUtf8());
            QVERIFY2(b.durationUs == a.durationUs, what);
            QVERIFY2(b.width == a.width && b.height == a.height, what);
            QVERIFY2(b.videoCodec == a.videoCodec && b.audioCodec == a.audioCodec, what);
            QVERIFY2(b.container == a.container, what + " " + b.container.toUtf8());
            QVERIFY2(b.isStillImage == a.isStillImage, what);
            QVERIFY2(b.frameRate == a.frameRate, what);
        }
    }

    void videoSeekAccuracy_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("hw"); // HwAccelMode
        QTest::newRow("h264 software") << QStringLiteral("h264_25fps.mp4") << int(HwAccelMode::Off);
        QTest::newRow("ts software") << QStringLiteral("h264_25fps.ts") << int(HwAccelMode::Off);
        QTest::newRow("vfr software") << QStringLiteral("vfr_odd.mp4") << int(HwAccelMode::Off);
        QTest::newRow("h264 hardware")
            << QStringLiteral("h264_25fps.mp4") << int(HwAccelMode::Auto);
        QTest::newRow("hevc10 hardware") << QStringLiteral("hevc10.mp4") << int(HwAccelMode::Auto);
    }

    /// Random seeks inside the container return exactly the frames of the file outside.
    void videoSeekAccuracy()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        QFETCH(QString, name);
        QFETCH(int, hw);
        const QString original = test::fixtureDir() + QLatin1Char('/') + name;
        VideoDecoder outside;
        VideoDecoder inside;
        outside.setHwAccel(HwAccelMode(hw));
        inside.setHwAccel(HwAccelMode(hw));
        QString error;
        QVERIFY2(outside.open(original, &error), qPrintable(error));
        QVERIFY2(inside.open(embedded(original), &error), qPrintable(error));
        QCOMPARE(inside.activeHwAccel(), outside.activeHwAccel());
        qInfo("%s decoding: %s", qPrintable(name), qPrintable(hwAccelName(inside.activeHwAccel())));
        const int64_t duration = inside.info().durationUs;
        QVERIFY(duration > 0);
        QRandomGenerator rng(42);
        std::vector<int64_t> times {0, duration - 1, duration / 2, 40'000, duration - 500'000};
        for (int i = 0; i < 30; ++i)
            times.push_back(int64_t(rng.bounded(quint32(duration))));
        for (const int64_t t : times) {
            const QImage a = outside.frameAt(t, QSize(320, 180));
            const QImage b = inside.frameAt(t, QSize(320, 180));
            QVERIFY2(!b.isNull(), qPrintable(QStringLiteral("t=%1").arg(t)));
            QCOMPARE(test::readFrameIndex(b), test::readFrameIndex(a));
            QCOMPARE(inside.lastFrameTimeUs(), outside.lastFrameTimeUs());
            QCOMPARE(b, a);
        }
        // decodeFrame (the GPU compositor's path) agrees as well.
        const auto fa = outside.decodeFrame(duration / 3);
        const auto fb = inside.decodeFrame(duration / 3);
        QVERIFY(fa && fb);
        QCOMPARE(fb->ptsUs, fa->ptsUs);
    }

    void audioSamplesMatch()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        for (const char* name : {"sine.mp3", "impulses_44k.wav", "h264_25fps.mp4"}) {
            const QString original = test::fixture(name);
            AudioDecoder outside;
            AudioDecoder inside;
            QString error;
            QVERIFY2(outside.open(original, &error), qPrintable(error));
            QVERIFY2(inside.open(embedded(original), &error), qPrintable(error));
            QCOMPARE(inside.lengthSamples(), outside.lengthSamples());
            std::vector<float> a(4800 * 2);
            std::vector<float> b(a.size());
            for (const int64_t start :
                {int64_t(0), int64_t(48000), int64_t(12345), int64_t(96000), int64_t(500)}) {
                QCOMPARE(inside.read(start, b.data(), 4800), outside.read(start, a.data(), 4800));
                QVERIFY2(a == b, name);
            }
        }
    }

    void stillImagesMatch()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        int checked = 0;
        for (auto it = m_embedded.cbegin(); it != m_embedded.cend(); ++it) {
            if (!ImageLoader::handlesExtension(it.key()))
                continue;
            const LoadedImage a = ImageLoader::load(it.key());
            const LoadedImage b = ImageLoader::load(it.value());
            const QByteArray what = QFileInfo(it.key()).fileName().toUtf8();
            if (a.image.isNull())
                continue; // backend not built in: not this test's business
            QVERIFY2(!b.image.isNull(), what + ": " + b.error.toUtf8());
            QVERIFY2(b.backend == a.backend, what);
            QVERIFY2(b.image == a.image, what);
            const ImageHeader ha = ImageLoader::readHeader(it.key());
            const ImageHeader hb = ImageLoader::readHeader(it.value());
            QVERIFY2(hb.valid && hb.size == ha.size && hb.animated == ha.animated, what);
            ++checked;
        }
        qInfo("%d still formats compared", checked);
        QVERIFY(checked >= 3);
        // The still cache keeps embedded and plain copies apart.
        const LoadedImage cached = ImageLoader::loadCached(embedded(test::fixture("still.jpg")));
        QVERIFY(!cached.image.isNull());
    }

    void animationsDecode()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        for (const char* name : {"anim.gif", "anim.webp"}) {
            const QString original
                = test::fixtureDir() + QStringLiteral("/formats/") + QLatin1String(name);
            if (!m_embedded.contains(original))
                continue;
            VideoDecoder outside;
            VideoDecoder inside;
            if (!outside.open(original))
                continue;
            QVERIFY2(inside.open(embedded(original)), name);
            for (const int64_t t :
                {int64_t(0), int64_t(150'000), int64_t(50'000), int64_t(400'000)})
                QCOMPARE(inside.frameAt(t, QSize(64, 64)), outside.frameAt(t, QSize(64, 64)));
        }
    }

    /// Thumbnails and waveforms of embedded media through the asynchronous cache (disk cache
    /// identity: container, entry and CRC).
    void thumbnailsAndWaveforms()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        MediaCache& cache = MediaCache::instance();
        const QString clip = embedded(test::fixture("h264_25fps.mp4"));
        QSignalSpy thumbs(&cache, &MediaCache::thumbnailReady);
        cache.thumbnail(clip, 2'000'000, 90);
        QImage thumb;
        QTRY_VERIFY_WITH_TIMEOUT(!(thumb = cache.thumbnail(clip, 2'000'000, 90)).isNull(), 20000);
        VideoDecoder decoder;
        QVERIFY(decoder.open(test::fixture("h264_25fps.mp4")));
        QCOMPARE(test::readFrameIndex(thumb),
            test::readFrameIndex(decoder.frameAt(2'000'000, QSize(320, 180))));
        const QString audio = embedded(test::fixture("sine.mp3"));
        cache.waveform(audio);
        std::shared_ptr<const WaveformData> wave;
        QTRY_VERIFY_WITH_TIMEOUT((wave = cache.waveform(audio)) != nullptr, 20000);
        const auto direct = computeWaveform(test::fixture("sine.mp3"));
        QVERIFY(direct);
        QCOMPARE(wave->size(), direct->size());
        QVERIFY(wave->maxL == direct->maxL);
        QVERIFY(!QDir(m_cacheDir.path() + QStringLiteral("/waveforms"))
                     .entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                     .isEmpty());
    }

    /// Many decoders on many threads read the same container at once (own handles, pread).
    void concurrentDecoders()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        const QString clip = embedded(test::fixture("h264_25fps.mp4"));
        const QString audio = embedded(test::fixture("sine.mp3"));
        std::atomic<int> failures {0};
        QList<int> workers {0, 1, 2, 3, 4, 5, 6, 7};
        QtConcurrent::blockingMap(workers, [&](int w) {
            QRandomGenerator rng(quint32(w + 7));
            if (w % 4 == 3) {
                AudioDecoder decoder;
                std::vector<float> buf(2 * 2048);
                if (!decoder.open(audio)) {
                    ++failures;
                    return;
                }
                for (int i = 0; i < 40; ++i) {
                    if (decoder.read(int64_t(rng.bounded(48000u * 4)), buf.data(), 2048) <= 0)
                        ++failures;
                }
                return;
            }
            VideoDecoder decoder;
            decoder.setHwAccel(HwAccelMode::Off);
            if (!decoder.open(clip)) {
                ++failures;
                return;
            }
            const int64_t duration = decoder.info().durationUs;
            for (int i = 0; i < 25; ++i) {
                const int64_t t = int64_t(rng.bounded(quint32(duration)));
                const int index = test::readFrameIndex(decoder.frameAt(t, QSize(160, 90)));
                if (index != test::expectedFrameIndex(t, 25, 1))
                    ++failures;
            }
        });
        QCOMPARE(failures.load(), 0);
    }

    /// A save (append) while decoders read keeps them working; their entries never move.
    void readWhileSaving()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        const QString clip = embedded(test::fixture("h264_25fps.mp4"));
        VideoDecoder decoder;
        QVERIFY(decoder.open(clip));
        QString error;
        auto package = pack::ProjectPackage::open(m_container, true, &error);
        QVERIFY2(package, qPrintable(error));
        for (int i = 0; i < 3; ++i) {
            QVERIFY(package->saveStoredDocument("{\"mediaPool\":{}}", &error));
            QCOMPARE(test::readFrameIndex(decoder.frameAt(1'000'000 + i * 40'000, QSize(160, 90))),
                25 + i);
        }
        VideoDecoder fresh;
        QVERIFY(fresh.open(clip));
        QCOMPARE(test::readFrameIndex(fresh.frameAt(80'000, QSize(160, 90))), 2);
    }
};

QTEST_GUILESS_MAIN(TestEmbeddedMedia)
#include "test_embeddedmedia.moc"
