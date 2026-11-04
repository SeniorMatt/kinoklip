// SPDX-License-Identifier: GPL-3.0-or-later
// KK_PLAYBACK_STATS: while playing, one "stats:" line per second; when playback stops, one
// "stats summary:" line (also for plays shorter than a second). Own executable because the
// variable is read once per process. Also: FFmpeg's log stays routed into kk.media after
// Qt Multimedia (whose FFmpeg backend installs its own av_log callback) initialised.
#include "core/Timeline.h"
#include "playback/AudioOutput.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include <QTest>

extern "C" {
#include <libavutil/log.h>
}

using namespace kk;

namespace {

QStringList g_lines; ///< kk.playback info lines
QStringList g_mediaWarnings; ///< kk.media warnings
QStringList g_otherFfmpeg; ///< any other category mentioning the probe text
QtMessageHandler g_previous = nullptr;

void handler(QtMsgType type, const QMessageLogContext& context, const QString& text)
{
    const QByteArray category = context.category ? QByteArray(context.category) : QByteArray();
    if (type == QtInfoMsg && category == "kk.playback") {
        g_lines << text;
    }
    if (type == QtWarningMsg && category == "kk.media") {
        g_mediaWarnings << text;
    } else if (text.contains(QLatin1String("routing check"))) {
        g_otherFfmpeg << QString::fromLatin1(category) + QStringLiteral(": ") + text;
    }
    if (g_previous) {
        g_previous(type, context, text);
    }
}

int countStarting(const char* prefix)
{
    int n = 0;
    for (const QString& line : g_lines) {
        n += line.startsWith(QLatin1String(prefix)) ? 1 : 0;
    }
    return n;
}

TimelineData solidTimeline(int64_t frames)
{
    TimelineData tl = makeDefaultTimeline(QStringLiteral("t"), {25, 1}, 320, 180, 2, 2);
    ClipData red;
    red.id = tl.allocateId();
    red.kind = ClipKind::Solid;
    red.solidColor = Qt::red;
    red.start = 0;
    red.duration = frames;
    tl.videoTracks[0].clips.push_back(red);
    return tl;
}

} // namespace

class TestPlaybackStats : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        qputenv("KK_PLAYBACK_STATS", "yes"); // any truthy value, read by the first engine
        g_previous = qInstallMessageHandler(handler);
    }
    void cleanupTestCase() { qInstallMessageHandler(g_previous); }

    void linesWhilePlayingAndSummaryOnStop()
    {
        Timeline tl;
        tl.setData(solidTimeline(500));
        PlaybackEngine::Options options;
        options.compositor = std::make_shared<Compositor>();
        options.noAudio = true;
        PlaybackEngine engine(std::move(options));
        engine.setTimeline(&tl);
        QCOMPARE(countStarting("KK_PLAYBACK_STATS:"), 1);

        engine.play();
        QTest::qWait(2300);
        engine.stop();
        QVERIFY2(countStarting("stats: ") >= 1, qPrintable(g_lines.join(u'\n')));
        QCOMPARE(countStarting("stats summary: played "), 1);
        const QString summary = g_lines.filter(QStringLiteral("stats summary")).first();
        QVERIFY2(summary.contains(QLatin1String("of 25.00")), qPrintable(summary));

        // A play shorter than one statistics window still ends with a summary.
        g_lines.clear();
        engine.play();
        QTest::qWait(300);
        engine.stop();
        QCOMPARE(countStarting("stats: "), 0);
        QCOMPARE(countStarting("stats summary: played "), 1);
    }

    void ffmpegLogStaysRoutedAfterQtMultimedia()
    {
        // Loads Qt Multimedia's backend (the bundle uses QT_MEDIA_BACKEND=ffmpeg, whose
        // initialisation replaces FFmpeg's log callback), the way the first player does.
        (void)QMediaDevices::audioOutputs();
        av_log(nullptr, AV_LOG_ERROR, "routing check before\n");
        qInfo("after Qt Multimedia init: %s", qPrintable(g_otherFfmpeg.join(u';')));
        const std::unique_ptr<AudioOutput> audio = createDefaultAudioOutput();
        (void)audio->isAvailable(); // initialises the device query
        g_mediaWarnings.clear();
        av_log(nullptr, AV_LOG_ERROR, "routing check after\n");
        QCOMPARE(g_mediaWarnings.filter(QStringLiteral("ffmpeg: routing check after")).size(), 1);
    }
};

QTEST_MAIN(TestPlaybackStats)
#include "test_playbackstats.moc"
