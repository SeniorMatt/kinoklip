// SPDX-License-Identifier: GPL-3.0-or-later
// Moving the playhead during playback (spec 48.2, a YouTuber's Resolve nitpicks): by default
// playback carries on from the new frame, exactly there (no skip forward) and with the sound
// in sync; with Preferences > Playback "Stop playback when the playhead is moved" the move
// pauses on the new frame instead (Premiere), for timeline moves and engine seeks alike.
#include "PlaybackTestUtil.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"

#include <QGuiApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>

#include <cmath>

using namespace kk;

namespace {

/// 320x180 25 fps timeline: a red solid on V1 over [0, frames).
TimelineData solidTimeline(int64_t frames)
{
    TimelineData tl = makeDefaultTimeline(QStringLiteral("t"), {25, 1}, 320, 180, 1, 1);
    ClipData red;
    red.id = tl.allocateId();
    red.kind = ClipKind::Solid;
    red.solidColor = Qt::red;
    red.start = 0;
    red.duration = frames;
    tl.videoTracks[0].clips.push_back(red);
    return tl;
}

std::shared_ptr<Compositor> g_compositor;

std::unique_ptr<PlaybackEngine> makeEngine()
{
    if (!g_compositor)
        g_compositor = std::make_shared<Compositor>();
    PlaybackEngine::Options options;
    options.compositor = g_compositor;
    options.audio = std::make_unique<test::FakeAudioOutput>(10, 40);
    return std::make_unique<PlaybackEngine>(std::move(options));
}

} // namespace

class TestBogPlayback : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void cleanupTestCase() { g_compositor.reset(); }

    /// Default (Resolve): playback goes on from the new frame. The positions after the move
    /// start at the target and then advance frame by frame (no jump ahead), and the sound
    /// clock is in step with the picture.
    void spec48_2_playbackResumesExactlyAtTheNewFrame()
    {
        Timeline tl;
        tl.setData(solidTimeline(2000));
        auto e = makeEngine();
        e->setTimeline(&tl);
        QVERIFY(!e->behaviour().stopOnPlayheadMove);
        e->play();
        QTest::qWait(300);
        for (const int64_t target : {int64_t(900), int64_t(400), int64_t(1500)}) {
            QVector<int64_t> positions;
            auto conn = connect(e.get(), &PlaybackEngine::positionChanged, this,
                [&](int64_t frame) { positions.append(frame); });
            tl.setPlayhead(target);
            QTest::qWait(500);
            disconnect(conn);
            QVERIFY(e->isPlaying());
            QVERIFY(!positions.isEmpty());
            QCOMPARE(positions.first(), target);
            int64_t previous = target;
            for (const int64_t p : positions) {
                QVERIFY2(p >= previous && p - previous <= 3,
                    qPrintable(
                        QStringLiteral("target %1: %2 after %3").arg(target).arg(p).arg(previous)));
                previous = p;
            }
            // About 0.5 s of playing (minus the short park): 6 to 15 frames on.
            QVERIFY2(previous > target + 5 && previous < target + 16,
                qPrintable(QStringLiteral("%1 -> %2").arg(target).arg(previous)));
            QVERIFY(e->stats().audioMaster);
            QVERIFY2(std::abs(e->stats().avOffsetFrames) < 3.0,
                qPrintable(QString::number(e->stats().avOffsetFrames)));
        }
        e->pause();
    }

    /// Preference: a move while playing pauses on the new frame (timeline moves such as a
    /// ruler click or edit navigation, and engine seeks such as the scrub bar).
    void spec48_2_stopPlaybackWhenThePlayheadIsMoved()
    {
        Timeline tl;
        tl.setData(solidTimeline(2000));
        auto e = makeEngine();
        e->setTimeline(&tl);
        PlaybackBehaviour b;
        b.stopOnPlayheadMove = true;
        e->setBehaviour(b);
        e->play();
        QTest::qWait(200);
        QVERIFY(e->isPlaying());
        tl.setPlayhead(700);
        QVERIFY(!e->isPlaying());
        QCOMPARE(e->position(), int64_t(700));
        QCOMPARE(tl.playhead(), int64_t(700));
        QVERIFY(QTest::qWaitFor(
            [&] { return e->currentFrame() && e->currentFrame()->frame == 700; }, 3000));
        QTest::qWait(150);
        QCOMPARE(tl.playhead(), int64_t(700)); // stays paused there

        e->play();
        QTest::qWait(200);
        QVERIFY(e->isPlaying());
        e->seek(300);
        QVERIFY(!e->isPlaying());
        QCOMPARE(tl.playhead(), int64_t(300));
        QCOMPARE(e->position(), int64_t(300));

        // Its own moves (playing on) are no "moves" of the user.
        e->play();
        QTest::qWait(300);
        QVERIFY(e->isPlaying());
        QVERIFY(tl.playhead() > 300);
        e->pause();
    }

    void spec48_2_preferenceIsStored()
    {
        QSettings s;
        s.remove(QStringLiteral("playback/stopOnPlayheadMove"));
        QVERIFY(!PlaybackBehaviour::fromSettings().stopOnPlayheadMove); // Resolve by default
        s.setValue(QStringLiteral("playback/stopOnPlayheadMove"), true);
        QVERIFY(PlaybackBehaviour::fromSettings().stopOnPlayheadMove);
        s.remove(QStringLiteral("playback/stopOnPlayheadMove"));
    }
};

int main(int argc, char** argv)
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("test_bogplayback"));
    QGuiApplication app(argc, argv);
    TestBogPlayback test;
    const int result = QTest::qExec(&test, argc, argv);
    g_compositor.reset();
    return result;
}

#include "test_bogplayback.moc"
