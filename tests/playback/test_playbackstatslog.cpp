// SPDX-License-Identifier: GPL-3.0-or-later
// The per-second playback statistics are always collected for the session log (spec section
// 64), and printed only with KK_PLAYBACK_STATS (test_playbackstats covers the printed lines).
#include "core/SessionLog.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"

#include <QTest>

using namespace kk;

namespace {
QStringList g_printed;
QtMessageHandler g_previous = nullptr;
void handler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (QByteArray(ctx.category) == "kk.playback")
        g_printed.append(msg);
    if (g_previous)
        g_previous(type, ctx, msg);
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

class TestPlaybackStatsLog : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qunsetenv("KK_PLAYBACK_STATS");
        g_previous = qInstallMessageHandler(handler);
    }
    void cleanupTestCase() { qInstallMessageHandler(g_previous); }

    // 64.11: without KK_PLAYBACK_STATS nothing is printed, yet the session log has the lines.
    void statsAreKeptButNotPrinted()
    {
        sessionlog::buffer().clear();
        Timeline tl;
        tl.setData(solidTimeline(500));
        PlaybackEngine::Options options;
        options.compositor = std::make_shared<Compositor>();
        options.noAudio = true;
        PlaybackEngine engine(std::move(options));
        engine.setTimeline(&tl);
        engine.play();
        QTest::qWait(2300);
        engine.stop();
        QStringList kept;
        for (const sessionlog::Entry& e : sessionlog::buffer().entries()) {
            if (e.category == QLatin1String("kk.playback"))
                kept.append(e.text);
        }
        QVERIFY2(!kept.filter(QStringLiteral("stats: ")).isEmpty(), qPrintable(kept.join(u'\n')));
        QCOMPARE(kept.filter(QStringLiteral("stats summary: played ")).size(), 1);
        QVERIFY2(
            g_printed.filter(QStringLiteral("stats")).isEmpty(), qPrintable(g_printed.join(u'\n')));
    }
};

QTEST_MAIN(TestPlaybackStatsLog)
#include "test_playbackstatslog.moc"
