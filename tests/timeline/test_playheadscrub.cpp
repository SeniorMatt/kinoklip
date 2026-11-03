// SPDX-License-Identifier: GPL-3.0-or-later
// Scrub audio only from the playhead's head (spec 1.27, 1.28; Reddit feedback on
// 1.0.0-beta.1): a drag that starts in the ruler, on the head, plays scrub snippets; a click or
// drag on the line through the tracks acts on what is under it and plays nothing; the Cut
// page's upper timeline moves the line silently below its ruler strip. The engine runs on a
// fake sound device (FakeAudioOutput), so "scrub audio" means the device was started.
#include "../playback/PlaybackTestUtil.h"
#include "TimelineTestUtils.h"
#include "playback/PlaybackEngine.h"
#include "render/Compositor.h"
#include "ui/cut/CutOverview.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QTest>

#include <memory>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};
constexpr TrackRef kA1 {TrackType::Audio, 0};

std::shared_ptr<Compositor> g_compositor;

/// Fixture with a program engine on a fake sound device, a timeline with a clip on V1 and
/// A1 over [0, 200), the playhead at frame 50.
struct Rig
{
    TimelineFixture f;
    FakeAudioOutput* device = nullptr;
    std::unique_ptr<PlaybackEngine> engine;

    Rig()
    {
        if (!g_compositor)
            g_compositor = std::make_shared<Compositor>();
        PlaybackEngine::Options options;
        options.compositor = g_compositor;
        auto fake = std::make_unique<FakeAudioOutput>();
        device = fake.get();
        options.audio = std::move(fake);
        engine = std::make_unique<PlaybackEngine>(std::move(options));
        PlaybackEngine::setAudioScrubbing(true);
        f.project.beginEdit(QStringLiteral("setup"));
        ops::overwriteClip(f.data(), kV1, solidClip(0, 200));
        ops::overwriteClip(f.data(), kA1, solidClip(0, 200, QStringLiteral("Sound")));
        f.project.endEdit();
        f.project.timeline()->setPlayhead(50);
        engine->setTimeline(f.project.timeline());
        f.ctx.playback = engine.get();
    }
    [[nodiscard]] bool scrubbed() const { return device->starts() > 0; }
    [[nodiscard]] int64_t playhead() const { return f.project.timeline()->playhead(); }
};

/// The Edit page timeline of @p rig, shown at 2 px per frame from frame 0.
struct View
{
    TimelineController c;
    TimelineWidget w;

    explicit View(Rig& rig)
        : c(rig.f.ctx)
        , w(rig.f.ctx, &c)
    {
        c.connectActions();
        w.setThumbnailProvider({});
        w.setWaveformProvider({});
        w.resize(1200, 400);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setPixelsPerFrame(2.0, kHeaderWidth);
        w.scrollToFrame(0);
    }
    QWidget* vp() { return w.viewport(); }
    [[nodiscard]] int x(double frame) const { return int(std::lround(w.layout().frameToX(frame))); }
    /// The middle of track @p ref at frame @p frame.
    [[nodiscard]] QPoint onTrack(double frame, TrackRef ref) const
    {
        const TimelineLayout& L = w.layout();
        return {x(frame), L.rowRect(L.rowOf(ref)).center().y()};
    }
    void drag(QPoint from, QPoint to)
    {
        QTest::mousePress(vp(), Qt::LeftButton, {}, from);
        for (int i = 1; i <= 4; ++i)
            QTest::mouseMove(vp(), from + (to - from) * i / 4);
        QTest::mouseRelease(vp(), Qt::LeftButton, {}, to);
    }
};

} // namespace

class TestPlayheadScrub : public QObject
{
    Q_OBJECT

private slots:
    void cleanup() { PlaybackEngine::setAudioScrubbing(true); }
    void cleanupTestCase() { g_compositor.reset(); } // before QApplication goes

    /// spec 1.27: dragging the head (in the ruler) moves the playhead and scrubs audio.
    void spec1_27_dragFromTheHeadScrubsAudio()
    {
        Rig rig;
        View v(rig);
        QVERIFY(!rig.scrubbed());
        const QPoint head(v.x(50), kRulerHeight / 2); // inside the drawn head
        v.drag(head, QPoint(v.x(90), head.y()));
        QCOMPARE(rig.playhead(), int64_t(90));
        QVERIFY(rig.scrubbed());
        QVERIFY(rig.engine->isScrubbingAudio());
    }

    /// spec 1.27: a press in the ruler away from the head also grabs the playhead there
    /// (jump + scrub, as in Resolve's ruler), with audio.
    void spec1_27_rulerClickScrubsAudio()
    {
        Rig rig;
        View v(rig);
        QTest::mouseClick(v.vp(), Qt::LeftButton, {}, QPoint(v.x(120), kRulerHeight / 2));
        QCOMPARE(rig.playhead(), int64_t(120));
        QVERIFY(rig.scrubbed());
    }

    /// spec 1.27: the line through the tracks is no drag handle. A drag that starts on it
    /// over a clip moves the clip (or selects it), never the playhead, and plays nothing.
    void spec1_27_dragOnTheLineOverAClipIsSilent()
    {
        Rig rig;
        View v(rig);
        const QPoint line = v.onTrack(50, kV1);
        QTest::mouseClick(v.vp(), Qt::LeftButton, {}, line);
        QCOMPARE(rig.playhead(), int64_t(50));
        QVERIFY(rig.f.selection.clips().contains(rig.f.view().track(kV1)->clips[0].id));
        v.drag(line, v.onTrack(80, kV1));
        QCOMPARE(rig.playhead(), int64_t(50));
        QVERIFY(!rig.scrubbed());
        QVERIFY(!rig.engine->isScrubbingAudio());
    }

    /// spec 1.27: on the line over empty track space a click selects the gap / clears the
    /// selection and a drag draws a rubber band; the playhead stays and nothing is heard.
    void spec1_27_clickOnTheLineInEmptySpaceIsSilent()
    {
        Rig rig;
        rig.f.project.timeline()->setPlayhead(300); // after the clips: empty track space
        View v(rig);
        rig.f.selection.setClips({rig.f.view().track(kV1)->clips[0].id});
        QCOMPARE(rig.device->starts(), 1); // the setup's own playhead move above
        QTRY_VERIFY_WITH_TIMEOUT(!rig.engine->isScrubbingAudio(), 2000);
        const int startsBefore = rig.device->starts();
        const QPoint line = v.onTrack(300, kA1);
        QTest::mouseClick(v.vp(), Qt::LeftButton, {}, line);
        QVERIFY(rig.f.selection.clips().isEmpty());
        v.drag(line, v.onTrack(340, kA1));
        QCOMPARE(rig.playhead(), int64_t(300));
        QCOMPARE(rig.device->starts(), startsBefore);
        QVERIFY(!rig.engine->isScrubbingAudio());
    }

    /// spec 1.28: the Cut page's upper timeline scrubs with audio only from its ruler strip;
    /// below it a press or drag moves the line (the playhead) silently.
    void spec1_28_cutOverviewScrubsOnlyFromTheRuler()
    {
        {
            Rig rig;
            CutOverview overview(rig.f.ctx);
            overview.resize(800, 120);
            overview.show();
            QVERIFY(QTest::qWaitForWindowExposed(&overview));
            const int y = overview.height() / 2;
            QVERIFY(!CutOverview::isRulerY(y));
            const int x0 = int(overview.frameToX(50));
            QTest::mousePress(&overview, Qt::LeftButton, {}, QPoint(x0, y));
            QTest::mouseMove(&overview, QPoint(x0 + 40, y));
            QTest::mouseRelease(&overview, Qt::LeftButton, {}, QPoint(x0 + 40, y));
            QCOMPARE(rig.playhead(), overview.xToFrame(x0 + 40));
            QVERIFY(!rig.scrubbed());
        }
        {
            Rig rig;
            CutOverview overview(rig.f.ctx);
            overview.resize(800, 120);
            overview.show();
            QVERIFY(QTest::qWaitForWindowExposed(&overview));
            const int y = 4;
            QVERIFY(CutOverview::isRulerY(y));
            const int x0 = int(overview.frameToX(50));
            QTest::mousePress(&overview, Qt::LeftButton, {}, QPoint(x0, y));
            QTest::mouseMove(&overview, QPoint(x0 + 40, y));
            QTest::mouseRelease(&overview, Qt::LeftButton, {}, QPoint(x0 + 40, y));
            QCOMPARE(rig.playhead(), overview.xToFrame(x0 + 40));
            QVERIFY(rig.scrubbed());
        }
    }

    /// The engine side: moves inside a SilentPlayheadMove render but play no snippet; the
    /// next move outside one scrubs again.
    void spec1_27_silentPlayheadMoveScope()
    {
        Rig rig;
        {
            const PlaybackEngine::SilentPlayheadMove silent;
            QVERIFY(PlaybackEngine::playheadMovesSilent());
            rig.f.project.timeline()->setPlayhead(70);
            rig.engine->seek(80);
        }
        QVERIFY(!PlaybackEngine::playheadMovesSilent());
        QCOMPARE(rig.engine->position(), int64_t(80));
        QVERIFY(!rig.scrubbed());
        rig.f.project.timeline()->setPlayhead(90);
        QVERIFY(rig.scrubbed());
    }
};

QTEST_MAIN(TestPlayheadScrub)
#include "test_playheadscrub.moc"
