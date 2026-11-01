// SPDX-License-Identifier: GPL-3.0-or-later
// test_probe — media::probe() fields for every fixture type, and robustness to broken files.
#include "MediaTestUtil.h"
#include "media/MediaInfo.h"

#include <QTest>

using namespace cw;
using namespace cw::media;

class TestProbe : public QObject
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

    void h264WithAac()
    {
        const MediaInfo info = probe(test::fixture("h264_25fps.mp4"));
        QVERIFY2(info.valid, qPrintable(info.error));
        QVERIFY(info.hasVideo);
        QVERIFY(info.hasAudio);
        QVERIFY(!info.isStillImage);
        QCOMPARE(info.width, 1280);
        QCOMPARE(info.height, 720);
        QCOMPARE(info.displayWidth, 1280);
        QCOMPARE(info.displayHeight, 720);
        QCOMPARE(info.rotation, 0);
        QCOMPARE(info.frameRate, (Rational {25, 1}));
        QVERIFY(qAbs(info.durationUs - 10'000'000) <= 40'000);
        QCOMPARE(info.audioChannels, 2);
        QCOMPARE(info.audioSampleRate, 48000);
        QCOMPARE(info.videoCodec, QStringLiteral("h264"));
        QCOMPARE(info.audioCodec, QStringLiteral("aac"));
        QCOMPARE(info.container, QStringLiteral("mp4"));
        QCOMPARE(info.pixelFormat, QStringLiteral("yuv420p"));
        QCOMPARE(info.sampleAspectRatio, (Rational {1, 1}));
        QVERIFY(!info.hasAlpha);
        QVERIFY(info.bitRate > 0);
        QCOMPARE(info.startTimeUs, 0);
    }

    void ntscFrameRateAndMonoAudio()
    {
        const MediaInfo info = probe(test::fixture("h264_2997.mp4"));
        QVERIFY2(info.valid, qPrintable(info.error));
        QCOMPARE(info.frameRate, (Rational {30000, 1001}));
        QCOMPARE(info.audioChannels, 1);
        QCOMPARE(info.audioSampleRate, 44100);
        QVERIFY(qAbs(info.durationUs - 6'000'000) <= 40'000);
    }

    void videoOnly()
    {
        const MediaInfo info = probe(test::fixture("h264_60fps.mp4"));
        QVERIFY2(info.valid, qPrintable(info.error));
        QVERIFY(info.hasVideo);
        QVERIFY(!info.hasAudio);
        QCOMPARE(info.frameRate, (Rational {60, 1}));
        QCOMPARE(info.audioChannels, 0);
        QVERIFY(info.audioCodec.isEmpty());
    }

    void transportStreamWithStartOffset()
    {
        const MediaInfo info = probe(test::fixture("h264_25fps.ts"));
        QVERIFY2(info.valid, qPrintable(info.error));
        QCOMPARE(info.container, QStringLiteral("mpegts"));
        QVERIFY(info.startTimeUs > 0); // MPEG-TS starts at ~1.4 s by default
        QVERIFY(qAbs(info.durationUs - 4'000'000) <= 80'000);
    }

    void variableFrameRateOddSize()
    {
        const MediaInfo info = probe(test::fixture("vfr_odd.mp4"));
        QVERIFY2(info.valid, qPrintable(info.error));
        QCOMPARE(info.width, 321);
        QCOMPARE(info.height, 241);
        QVERIFY(info.frameRate.isValid());
        QCOMPARE(info.pixelFormat, QStringLiteral("yuv444p"));
    }

    void proresWithAlpha()
    {
        const MediaInfo info = probe(test::fixture("prores4444_alpha.mov"));
        QVERIFY2(info.valid, qPrintable(info.error));
        QCOMPARE(info.videoCodec, QStringLiteral("prores"));
        QCOMPARE(info.container, QStringLiteral("mov"));
        QVERIFY(info.hasAlpha);
    }

    void rotation()
    {
        const MediaInfo info = probe(test::fixture("rotated_portrait.mp4"));
        QVERIFY2(info.valid, qPrintable(info.error));
        QCOMPARE(info.width, 640);
        QCOMPARE(info.height, 360);
        QCOMPARE(info.rotation, 90);
        QCOMPARE(info.displayWidth, 360);
        QCOMPARE(info.displayHeight, 640);
    }

    void audioFiles()
    {
        const MediaInfo wav = probe(test::fixture("mono_ramp_48k.wav"));
        QVERIFY2(wav.valid, qPrintable(wav.error));
        QVERIFY(!wav.hasVideo);
        QVERIFY(wav.hasAudio);
        QCOMPARE(wav.audioChannels, 1);
        QCOMPARE(wav.audioSampleRate, 48000);
        QCOMPARE(wav.audioCodec, QStringLiteral("pcm_s16le"));
        QCOMPARE(wav.container, QStringLiteral("wav"));
        QCOMPARE(wav.durationUs, 5'000'000);
        QVERIFY(!wav.frameRate.isValid());

        const MediaInfo mp3 = probe(test::fixture("sine.mp3"));
        QVERIFY2(mp3.valid, qPrintable(mp3.error));
        QVERIFY(!mp3.hasVideo);
        QCOMPARE(mp3.audioCodec, QStringLiteral("mp3"));
        QCOMPARE(mp3.audioChannels, 2);
        QCOMPARE(mp3.audioSampleRate, 44100);
        QVERIFY(qAbs(mp3.durationUs - 3'000'000) <= 60'000);
    }

    void stills()
    {
        const MediaInfo jpg = probe(test::fixture("still.jpg"));
        QVERIFY2(jpg.valid, qPrintable(jpg.error));
        QVERIFY(jpg.hasVideo);
        QVERIFY(jpg.isStillImage);
        QVERIFY(!jpg.hasAudio);
        QCOMPARE(jpg.durationUs, 0);
        QCOMPARE(jpg.width, 800);
        QCOMPARE(jpg.height, 600);
        QCOMPARE(jpg.displayWidth, 800);
        QVERIFY(!jpg.frameRate.isValid());
        QVERIFY(!jpg.hasAlpha);

        const MediaInfo png = probe(test::fixture("still_alpha.png"));
        QVERIFY2(png.valid, qPrintable(png.error));
        QVERIFY(png.isStillImage);
        QVERIFY(png.hasAlpha);
        QCOMPARE(png.width, 64);
        QCOMPARE(png.height, 48);
    }

    void brokenFiles_data()
    {
        QTest::addColumn<QString>("path");
        QTest::newRow("random bytes") << test::fixture("corrupt.mp4");
        QTest::newRow("empty file") << test::fixture("empty.mov");
        QTest::newRow("missing file") << test::fixture("does_not_exist.mp4");
        QTest::newRow("directory") << test::fixtureDir();
    }

    void brokenFiles()
    {
        QFETCH(QString, path);
        const MediaInfo info = probe(path);
        QVERIFY(!info.valid);
        QVERIFY(!info.error.isEmpty());
        QCOMPARE(info.path, path);
    }
};

QTEST_GUILESS_MAIN(TestProbe)
#include "test_probe.moc"
