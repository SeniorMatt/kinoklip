// SPDX-License-Identifier: GPL-3.0-or-later
// test_videodecoder — frame-exact seeking, sequential decoding, VFR, rotation, alpha, stills.
#include "MediaTestUtil.h"
#include "media/VideoDecoder.h"

#include <QRandomGenerator>
#include <QTest>
#include <QtConcurrent>

#include <algorithm>
#include <functional>
#include <vector>

using namespace cw;
using namespace cw::media;

namespace {

/// Constant-frame-rate barcode fixture: name and exact rate.
struct CfrFixture
{
    const char* name;
    int64_t num;
    int64_t den;
};

/// Opens `name` or fails the current test.
#define OPEN_OR_FAIL(decoder, name)                                                                \
    do {                                                                                           \
        QString openError;                                                                         \
        QVERIFY2((decoder).open(test::fixture(name), &openError), qPrintable(openError));          \
    } while (false)

/// Decodes at `timeUs` and checks the barcode against `expected`.
bool checkFrame(
    VideoDecoder& decoder, int64_t timeUs, int64_t expected, QSize maxSize, QString* message)
{
    const QImage image = decoder.frameAt(timeUs, maxSize);
    const int actual = test::readFrameIndex(image);
    if (actual != expected) {
        *message = QStringLiteral("at %1 us: expected frame %2, got %3 (image %4x%5)")
                       .arg(timeUs)
                       .arg(expected)
                       .arg(actual)
                       .arg(image.width())
                       .arg(image.height());
        return false;
    }
    return true;
}

/// Frame index shown at `timeUs` in vfr_odd.mp4 (0..29 every 1/30 s, then 30..59 every 1/15 s).
int64_t vfrFrameIndex(int64_t timeUs)
{
    if (timeUs < 1'000'000) {
        return timeUs * 30 / 1'000'000;
    }
    return std::min<int64_t>(59, 30 + (timeUs - 1'000'000) * 15 / 1'000'000);
}

} // namespace

class TestVideoDecoder : public QObject
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

    void fitSize()
    {
        QCOMPARE(VideoDecoder::fitSize({1920, 1080}, {}), QSize(1920, 1080));
        QCOMPARE(VideoDecoder::fitSize({1920, 1080}, {960, 960}), QSize(960, 540));
        QCOMPARE(VideoDecoder::fitSize({1920, 1080}, {0, 90}), QSize(160, 90));
        QCOMPARE(VideoDecoder::fitSize({1920, 1080}, {320, 0}), QSize(320, 180));
        QCOMPARE(VideoDecoder::fitSize({100, 100}, {300, 200}), QSize(200, 200));
        QCOMPARE(VideoDecoder::fitSize({1000, 10}, {10, 10}), QSize(10, 1));
        QCOMPARE(VideoDecoder::fitSize({}, {10, 10}), QSize());
    }

    void openFailures_data()
    {
        QTest::addColumn<QString>("path");
        QTest::newRow("random bytes") << test::fixture("corrupt.mp4");
        QTest::newRow("empty") << test::fixture("empty.mov");
        QTest::newRow("missing") << test::fixture("nope.mp4");
        QTest::newRow("audio only") << test::fixture("mono_ramp_48k.wav");
    }

    void openFailures()
    {
        QFETCH(QString, path);
        VideoDecoder decoder;
        QString error;
        QVERIFY(!decoder.open(path, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!decoder.isOpen());
        QVERIFY(decoder.frameAt(0).isNull());
    }

    void exactSeek_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<qint64>("num");
        QTest::addColumn<qint64>("den");
        QTest::addColumn<QSize>("maxSize");
        const CfrFixture fixtures[] = {
            {"h264_25fps.mp4", 25, 1},
            {"h264_2997.mp4", 30000, 1001},
            {"h264_60fps.mp4", 60, 1},
            {"h264_25fps.ts", 25, 1},
        };
        for (const CfrFixture& f : fixtures) {
            QTest::addRow("%s native", f.name)
                << QString::fromLatin1(f.name) << qint64(f.num) << qint64(f.den) << QSize();
            QTest::addRow("%s 320x180", f.name)
                << QString::fromLatin1(f.name) << qint64(f.num) << qint64(f.den) << QSize(320, 180);
        }
    }

    /// Random, ascending, descending and interval-boundary targets must all hit the exact frame.
    void exactSeek()
    {
        QFETCH(QString, name);
        QFETCH(qint64, num);
        QFETCH(qint64, den);
        QFETCH(QSize, maxSize);
        VideoDecoder decoder;
        OPEN_OR_FAIL(decoder, name.toLatin1().constData());
        const int64_t durationUs = decoder.info().durationUs;
        QVERIFY(durationUs > 0);
        const int64_t frameCount = test::expectedFrameIndex(durationUs - 1, num, den) + 1;

        QRandomGenerator rng(1234);
        std::vector<int64_t> targets;
        for (int i = 0; i < 40; ++i) {
            targets.push_back(rng.bounded(qint64(0), qint64(durationUs)));
        }
        std::vector<int64_t> ascending(targets.begin(), targets.begin() + 15);
        std::sort(ascending.begin(), ascending.end());
        std::vector<int64_t> descending(targets.begin() + 15, targets.begin() + 30);
        std::sort(descending.begin(), descending.end(), std::greater<>());
        std::vector<int64_t> all = targets;
        all.insert(all.end(), ascending.begin(), ascending.end());
        all.insert(all.end(), descending.begin(), descending.end());
        // Exact interval boundaries: first and last microsecond of a few frames.
        for (int64_t k : {int64_t(1), int64_t(7), frameCount / 2, frameCount - 2}) {
            all.push_back(test::frameStartUs(k, num, den));
            all.push_back(test::frameStartUs(k + 1, num, den) - 1);
        }

        for (const int64_t t : all) {
            QString message;
            QVERIFY2(
                checkFrame(decoder, t, test::expectedFrameIndex(t, num, den), maxSize, &message),
                qPrintable(message));
            const int64_t expectedStart
                = test::frameStartUs(test::expectedFrameIndex(t, num, den), num, den);
            QVERIFY(qAbs(decoder.lastFrameTimeUs() - expectedStart) <= 1);
        }
    }

    void sequentialDecode_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<qint64>("num");
        QTest::addColumn<qint64>("den");
        QTest::newRow("25") << QStringLiteral("h264_25fps.mp4") << qint64(25) << qint64(1);
        QTest::newRow("29.97") << QStringLiteral("h264_2997.mp4") << qint64(30000) << qint64(1001);
        QTest::newRow("60") << QStringLiteral("h264_60fps.mp4") << qint64(60) << qint64(1);
    }

    /// Stepping frame by frame returns consecutive frames (the playback path).
    void sequentialDecode()
    {
        QFETCH(QString, name);
        QFETCH(qint64, num);
        QFETCH(qint64, den);
        VideoDecoder decoder;
        OPEN_OR_FAIL(decoder, name.toLatin1().constData());
        const int64_t frameCount
            = test::expectedFrameIndex(decoder.info().durationUs - 1, num, den) + 1;
        for (int64_t k = 0; k < frameCount; ++k) {
            QString message;
            // Middle of the frame interval, as a playback clock would ask.
            const int64_t t
                = (test::frameStartUs(k, num, den) + test::frameStartUs(k + 1, num, den)) / 2;
            QVERIFY2(checkFrame(decoder, t, k, QSize(320, 180), &message), qPrintable(message));
            // Asking again for the same frame must return the cached image.
            QVERIFY2(checkFrame(decoder, t, k, QSize(320, 180), &message), qPrintable(message));
        }
    }

    void clampsOutOfRange()
    {
        VideoDecoder decoder;
        OPEN_OR_FAIL(decoder, "h264_25fps.mp4");
        QCOMPARE(test::readFrameIndex(decoder.frameAt(-5'000'000)), 0);
        QCOMPARE(decoder.lastFrameTimeUs(), 0);
        QCOMPARE(test::readFrameIndex(decoder.frameAt(60'000'000)), 249);
        QCOMPARE(test::readFrameIndex(decoder.frameAt(10'000'000)), 249);
        QCOMPARE(test::readFrameIndex(decoder.frameAt(0)), 0);
    }

    void variableFrameRate()
    {
        VideoDecoder decoder;
        OPEN_OR_FAIL(decoder, "vfr_odd.mp4");
        const QImage first = decoder.frameAt(0);
        QCOMPARE(first.size(), QSize(321, 241));
        QCOMPARE(first.format(), QImage::Format_ARGB32_Premultiplied);
        QRandomGenerator rng(99);
        for (int i = 0; i < 60; ++i) {
            const int64_t t = rng.bounded(qint64(0), qint64(3'000'000));
            QString message;
            QVERIFY2(checkFrame(decoder, t, vfrFrameIndex(t), {}, &message), qPrintable(message));
        }
        // Past the container duration (2.9 s) the true last frame (2.933 s) is still reachable.
        QCOMPARE(test::readFrameIndex(decoder.frameAt(5'000'000)), 59);
    }

    void nearestKeyframe()
    {
        VideoDecoder decoder;
        OPEN_OR_FAIL(decoder, "h264_25fps.mp4"); // keyframe every 50 frames
        for (const int64_t t :
            {int64_t(3'300'000), int64_t(9'000'000), int64_t(1'000'000), int64_t(5'500'000)}) {
            const QImage image
                = decoder.frameAt(t, QSize(320, 180), VideoDecoder::Accuracy::NearestKeyframe);
            const int index = test::readFrameIndex(image);
            const int64_t exact = test::expectedFrameIndex(t, 25, 1);
            QVERIFY2(index >= 0 && index <= exact && exact - index < 50,
                qPrintable(QStringLiteral("t=%1 index=%2").arg(t).arg(index)));
        }
        // Exact mode keeps working after keyframe-mode requests.
        QCOMPARE(test::readFrameIndex(decoder.frameAt(5'560'000, QSize(320, 180))), 139);
    }

    void rotation()
    {
        VideoDecoder decoder;
        OPEN_OR_FAIL(decoder, "rotated_portrait.mp4");
        QCOMPARE(decoder.info().rotation, 90);
        const QImage image = decoder.frameAt(500'000);
        QCOMPARE(image.size(), QSize(360, 640));
        // The red box sat in the coded top-left corner; rotated 90° clockwise it is top-right.
        const QColor topRight = image.pixelColor(image.width() - 10, 10);
        const QColor topLeft = image.pixelColor(10, 10);
        QVERIFY2(topRight.red() > 200 && topRight.blue() < 60, qPrintable(topRight.name()));
        QVERIFY2(topLeft.blue() > 200 && topLeft.red() < 60, qPrintable(topLeft.name()));
        QCOMPARE(decoder.frameAt(500'000, QSize(0, 320)).size(), QSize(180, 320));
    }

    void alphaPreserved()
    {
        VideoDecoder decoder;
        OPEN_OR_FAIL(decoder, "prores4444_alpha.mov");
        QVERIFY(decoder.info().hasAlpha);
        const QImage image = decoder.frameAt(1'000'000);
        QCOMPARE(image.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(qAlpha(image.pixel(40, 120)), 255);
        QCOMPARE(qAlpha(image.pixel(280, 120)), 0);
        QCOMPARE(image.pixel(280, 120), 0u); // premultiplied: transparent = all zero

        VideoDecoder opaque;
        OPEN_OR_FAIL(opaque, "h264_60fps.mp4");
        const QImage opaqueImage = opaque.frameAt(0);
        QCOMPARE(qAlpha(opaqueImage.pixel(5, 5)), 255);
        QCOMPARE(qAlpha(opaqueImage.pixel(600, 300)), 255);
    }

    void stills()
    {
        VideoDecoder jpg;
        OPEN_OR_FAIL(jpg, "still.jpg");
        QVERIFY(jpg.info().isStillImage);
        QCOMPARE(jpg.frameAt(0).size(), QSize(800, 600));
        QCOMPARE(jpg.frameAt(123'456'789).size(), QSize(800, 600));
        QCOMPARE(jpg.frameAt(0, QSize(400, 0)).size(), QSize(400, 300));
        QCOMPARE(jpg.frameAt(0).format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(jpg.lastFrameTimeUs(), 0);

        VideoDecoder png;
        OPEN_OR_FAIL(png, "still_alpha.png");
        const QImage image = png.frameAt(0);
        QCOMPARE(image.size(), QSize(64, 48));
        QCOMPARE(image.pixel(8, 24), qRgba(255, 0, 0, 255));
        QCOMPARE(qAlpha(image.pixel(56, 24)), 0);
    }

    void truncatedFile()
    {
        VideoDecoder decoder;
        OPEN_OR_FAIL(decoder, "truncated.mp4");
        QCOMPARE(test::readFrameIndex(decoder.frameAt(0)), 0);
        // Data stops at ~40 %: late requests return the last decodable frame, not garbage.
        const QImage late = decoder.frameAt(9'000'000);
        QVERIFY(!late.isNull());
        const int lastIndex = test::readFrameIndex(late);
        QVERIFY2(lastIndex > 0 && lastIndex <= 225, qPrintable(QString::number(lastIndex)));
        QCOMPARE(test::readFrameIndex(decoder.frameAt(1'000'000)), 25);
    }

    /// Independent decoders on several threads must not interfere.
    void concurrentInstances()
    {
        auto worker = [](int seed) {
            VideoDecoder decoder;
            if (!decoder.open(test::fixture("h264_2997.mp4"))) {
                return false;
            }
            QRandomGenerator rng(seed);
            for (int i = 0; i < 15; ++i) {
                const int64_t t = rng.bounded(qint64(0), qint64(5'900'000));
                const QImage image = decoder.frameAt(t, QSize(320, 180));
                if (test::readFrameIndex(image) != test::expectedFrameIndex(t, 30000, 1001)) {
                    return false;
                }
            }
            return true;
        };
        const QList<int> seeds = {1, 2, 3, 4};
        const QList<bool> results = QtConcurrent::blockingMapped(seeds, worker);
        QCOMPARE(results, QList<bool>({true, true, true, true}));
    }
};

QTEST_GUILESS_MAIN(TestVideoDecoder)
#include "test_videodecoder.moc"
