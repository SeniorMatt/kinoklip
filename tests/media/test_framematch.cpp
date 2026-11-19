// SPDX-License-Identifier: GPL-3.0-or-later
// A timeline frame k of a clip with the rate of the timeline must show source frame k, for every
// k: the media time of frame k is rounded to microseconds (framesToUs), and in the rate of a
// 23.976 fps clip that rounding lands up to a microsecond before the start of the frame, which
// the decoder used to answer with the frame before, so every third picture was shown twice
// (the stutter of 24000/1001 footage, found with PlaybackEngine::Stats::uniqueFrames). Containers
// with a millisecond time base (Matroska) round the frame's own timestamp the same way.
#include "core/Rational.h"
#include "media/VideoDecoder.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace kk;

class TestFrameMatch : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString makeClip(const QString& name, const QString& rate)
    {
        const QString path = m_dir.filePath(name);
        QProcess ff;
        ff.start(QStringLiteral("ffmpeg"),
            {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                QStringLiteral("testsrc2=size=160x90:rate=%1").arg(rate), "-frames:v", "150",
                "-c:v", "libx264", "-preset", "ultrafast", "-g", "30", "-pix_fmt", "yuv420p",
                path});
        if (!ff.waitForFinished(60000) || ff.exitCode() != 0)
            return {};
        return path;
    }

    /// Frame index of the picture whose start is @p ptsUs in a clip of @p fps.
    static int64_t indexOf(int64_t ptsUs, Rational fps)
    {
        return int64_t(std::llround(double(ptsUs) * fps.num / (1e6 * fps.den)));
    }

    void check(const QString& file, Rational fps)
    {
        media::VideoDecoder decoder;
        QVERIFY(decoder.open(file));
        const int64_t frames = 140;
        int bad = 0;
        // Forward, frame by frame (playback) ...
        for (int64_t k = 0; k < frames; ++k) {
            const auto frame = decoder.decodeFrame(framesToUs(k, fps));
            QVERIFY(frame);
            if (indexOf(frame->ptsUs, fps) != k)
                ++bad;
        }
        QCOMPARE(bad, 0);
        // ... and after seeks (any order).
        for (int64_t k : {97, 3, 64, 65, 33, 120, 31, 30, 29}) {
            const auto frame = decoder.decodeFrame(framesToUs(k, fps));
            QVERIFY(frame);
            QCOMPARE(indexOf(frame->ptsUs, fps), k);
        }
    }

private slots:
    void initTestCase()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg CLI not found; cannot make the test clips");
        QVERIFY(m_dir.isValid());
    }

    void rate23976_mov()
    {
        const QString f = makeClip(QStringLiteral("a.mov"), QStringLiteral("24000/1001"));
        QVERIFY(!f.isEmpty());
        check(f, {24000, 1001});
    }
    void rate23976_matroska()
    {
        const QString f = makeClip(QStringLiteral("a.mkv"), QStringLiteral("24000/1001"));
        QVERIFY(!f.isEmpty());
        check(f, {24000, 1001});
    }
    void rate2997_mp4()
    {
        const QString f = makeClip(QStringLiteral("b.mp4"), QStringLiteral("30000/1001"));
        QVERIFY(!f.isEmpty());
        check(f, {30000, 1001});
    }
    void rate5994_matroska()
    {
        const QString f = makeClip(QStringLiteral("b.mkv"), QStringLiteral("60000/1001"));
        QVERIFY(!f.isEmpty());
        check(f, {60000, 1001});
    }
    void rate25_mp4()
    {
        const QString f = makeClip(QStringLiteral("c.mp4"), QStringLiteral("25"));
        QVERIFY(!f.isEmpty());
        check(f, {25, 1});
    }
};

QTEST_GUILESS_MAIN(TestFrameMatch)
#include "test_framematch.moc"
