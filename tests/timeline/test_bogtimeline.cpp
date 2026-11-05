// SPDX-License-Identifier: GPL-3.0-or-later
// A YouTuber's Resolve nitpicks, checked on Kinoklip's timeline (spec 48): the playhead does
// not jump when its head is grabbed, and the ruler can be set to move it only on a click
// (48.1); the vertical scroll position stays put while clicking, editing, undoing and adding
// tracks (48.3); short transitions are easy to select and keep the cut's trim zone around
// them (48.4); Open in Timeline opens a compound clip on the frame under the playhead (48.5).
#include "ParityTestUtils.h"
#include "core/EditBehaviour.h"
#include "playback/PlaybackEngine.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QCheckBox>
#include <QScrollBar>
#include <QSettings>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
using namespace kk::test::parity;

namespace {

/// View y in the ruler (below the marker band's top, above the ticks).
constexpr int kRulerY = 8;

QPoint rulerAt(View& v, double x)
{
    return QPoint(int(std::lround(x)), kRulerY);
}

double playheadX(View& v)
{
    return v.w.layout().frameToX(double(v.tl()->playhead()));
}

} // namespace

class TestBogTimeline : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_bogtimeline"); }
    void cleanup() { QSettings().remove(QStringLiteral("edit/rulerPressJumpsPlayhead")); }

    // ---- 48.1 ----

    void spec48_1_grabbingTheHeadDoesNotJump()
    {
        View v;
        v.w.setPixelsPerFrame(20.0, kHeaderWidth); // a frame is wider than half the head
        v.w.scrollToFrame(0);
        v.tl()->setPlayhead(20);
        const double x = playheadX(v);
        // A press on the left half of the head lies in the previous frame: it used to step
        // the playhead back.
        QTest::mousePress(v.vp(), Qt::LeftButton, {}, rulerAt(v, x - 5));
        QCOMPARE(v.tl()->playhead(), int64_t(20));
        // Dragging moves it by the pointer's motion: 2 frames = 40 px.
        QTest::mouseMove(v.vp(), rulerAt(v, x - 5 + 20));
        QTest::mouseMove(v.vp(), rulerAt(v, x - 5 + 40));
        QCOMPARE(v.tl()->playhead(), int64_t(22));
        QTest::mouseRelease(v.vp(), Qt::LeftButton, {}, rulerAt(v, x - 5 + 40));
        QCOMPARE(v.tl()->playhead(), int64_t(22));
        // A tiny slip of the mouse on the head does nothing.
        const double x2 = playheadX(v);
        v.drag(rulerAt(v, x2 + 4), rulerAt(v, x2 + 6));
        QCOMPARE(v.tl()->playhead(), int64_t(22));
    }

    void spec48_1_rulerPressJumpsByDefault()
    {
        View v;
        v.tl()->setPlayhead(10);
        QVERIFY(EditBehaviour::fromSettings().rulerPressJumpsPlayhead);
        const double x = v.w.layout().frameToX(60.5);
        QTest::mousePress(v.vp(), Qt::LeftButton, {}, rulerAt(v, x));
        QCOMPARE(v.tl()->playhead(), int64_t(60)); // Resolve: at the press
        QTest::mouseMove(v.vp(), rulerAt(v, x + 30));
        QTest::mouseMove(v.vp(), rulerAt(v, x + 50));
        QCOMPARE(v.tl()->playhead(), int64_t(65));
        QTest::mouseRelease(v.vp(), Qt::LeftButton, {}, rulerAt(v, x + 50));
        // Clicks on clips and in the track area never move it (spec 1.27, 2.6).
        const quint64 id = v.put(kV1, solidClip(0, 100));
        QCoreApplication::processEvents();
        v.click(v.at(30.5, kV1));
        QCOMPARE(v.f.selection.clips(), QVector<quint64> {id});
        v.drag(v.at(80.5, kV1), v.at(80.5, kV1) + QPoint(2, 0));
        QCOMPARE(v.tl()->playhead(), int64_t(65));
    }

    void spec48_1_preferenceMovesOnlyOnAClick()
    {
        QSettings().setValue(QStringLiteral("edit/rulerPressJumpsPlayhead"), false);
        View v;
        v.tl()->setPlayhead(10);
        const double x = v.w.layout().frameToX(60.5);
        // A drag away from the head leaves the playhead alone.
        QTest::mousePress(v.vp(), Qt::LeftButton, {}, rulerAt(v, x));
        QCOMPARE(v.tl()->playhead(), int64_t(10));
        QTest::mouseMove(v.vp(), rulerAt(v, x + 30));
        QTest::mouseMove(v.vp(), rulerAt(v, x + 60));
        QTest::mouseRelease(v.vp(), Qt::LeftButton, {}, rulerAt(v, x + 60));
        QCOMPARE(v.tl()->playhead(), int64_t(10));
        // A click moves it on release.
        QTest::mousePress(v.vp(), Qt::LeftButton, {}, rulerAt(v, x));
        QCOMPARE(v.tl()->playhead(), int64_t(10));
        QTest::mouseRelease(v.vp(), Qt::LeftButton, {}, rulerAt(v, x));
        QCOMPARE(v.tl()->playhead(), int64_t(60));
        // The head still drags.
        const double h = playheadX(v);
        v.drag(rulerAt(v, h + 1), rulerAt(v, h + 1 + 50));
        QCOMPARE(v.tl()->playhead(), int64_t(65));
    }

    /// Both options are in Preferences, off the Resolve way by default, and are stored.
    void spec48_preferencesShowAndStoreTheOptions()
    {
        QSettings().remove(QStringLiteral("playback/stopOnPlayheadMove"));
        {
            PreferencesDialog dialog(nullptr);
            auto* ruler = dialog.findChild<QCheckBox*>(QStringLiteral("RulerPressJumpsPlayhead"));
            auto* stop = dialog.findChild<QCheckBox*>(QStringLiteral("StopOnPlayheadMove"));
            QVERIFY(ruler && stop);
            QVERIFY(ruler->isChecked());
            QVERIFY(!stop->isChecked());
            ruler->setChecked(false);
            stop->setChecked(true);
            dialog.accept();
        }
        QVERIFY(!EditBehaviour::fromSettings().rulerPressJumpsPlayhead);
        QVERIFY(PlaybackBehaviour::fromSettings().stopOnPlayheadMove);
        QSettings().remove(QStringLiteral("playback/stopOnPlayheadMove"));
    }

    // ---- 48.3 ----

    void spec48_3_verticalScrollStaysWhileEditing()
    {
        View v;
        // 12 video tracks and 4 audio tracks, a clip on each video track.
        for (int i = 1; i < 12; ++i)
            v.c.addTrack(TrackType::Video);
        for (int i = 1; i < 4; ++i)
            v.c.addTrack(TrackType::Audio);
        QVector<quint64> clips;
        for (int i = 0; i < 12; ++i)
            clips.append(v.put(TrackRef {TrackType::Video, i}, solidClip(10 + i, 40)));
        QCoreApplication::processEvents();
        QScrollBar* bar = v.w.verticalScrollBar();
        QVERIFY(bar->maximum() > 200);
        bar->setValue(bar->maximum() / 2);
        QCoreApplication::processEvents();
        const TrackRef v5 {TrackType::Video, 4};
        auto rowY = [&] { return v.w.layout().rowRect(v.w.layout().rowOf(v5)).top(); };
        const int y0 = rowY();
        const int scroll0 = bar->value();
        QVERIFY(y0 > kRulerHeight && y0 < v.vp()->height() - 40); // V5 is on screen
        auto check = [&](const char* step) {
            QCoreApplication::processEvents();
            QVERIFY2(rowY() == y0, step);
        };
        // Clicks: a clip, the empty track area, the ruler.
        v.click(v.at(30.5, v5));
        QCOMPARE(v.f.selection.clips(), QVector<quint64> {clips[4]});
        check("click on a clip");
        QCOMPARE(bar->value(), scroll0);
        v.click(v.at(120.5, v5));
        check("click in the empty track area");
        v.click(QPoint(int(v.w.layout().frameToX(70.5)), kRulerY));
        check("click in the ruler");
        // Edits: a move by dragging, a trim, Delete, undo and redo.
        v.drag(v.at(30.5, v5), v.at(45.5, v5));
        QCOMPARE(ops::clipById(v.f.view(), clips[4])->start, int64_t(29));
        check("move");
        v.f.project.undoStack()->undo();
        check("undo");
        v.f.project.undoStack()->redo();
        check("redo");
        v.f.selection.setClips({clips[4]});
        v.action("edit.delete")->trigger();
        check("delete");
        v.f.project.undoStack()->undo();
        check("undo delete");
        QCOMPARE(bar->value(), scroll0);
        // Tracks added and removed above it (video tracks stack upwards): the rows the user
        // looks at stay where they are.
        v.c.addTrack(TrackType::Video);
        check("track added on top");
        v.f.project.undoStack()->undo();
        check("track addition undone");
        QCOMPARE(bar->value(), scroll0);
        // Scrolled to the very top, a new top track comes into view.
        bar->setValue(0);
        QCoreApplication::processEvents();
        v.c.addTrack(TrackType::Video);
        QCoreApplication::processEvents();
        QCOMPARE(bar->value(), 0);
    }

    // ---- 48.4 ----

    void spec48_4_shortTransitionIsEasyToSelect()
    {
        View v;
        v.w.setPixelsPerFrame(1.0, kHeaderWidth); // zoomed out: 4 frames = 4 px
        v.w.scrollToFrame(0);
        const quint64 left = v.put(kV1, solidClip(100, 100));
        const quint64 right = v.put(kV1, solidClip(200, 100));
        v.f.project.beginEdit(QStringLiteral("setup"));
        TransitionData t;
        t.duration = 4;
        ops::clipById(v.f.data(), right)->transitionIn = t;
        v.f.project.endEdit();
        QCoreApplication::processEvents();
        const double cut = v.w.layout().frameToX(200.0);
        const int y = v.at(150, kV1).y();
        const TransitionRef ref {right, true};
        // Anywhere within 4 px of its centre selects it, not the cut under it.
        for (const double dx : {-3.5, -1.0, 0.0, 2.0, 3.5}) {
            QCOMPARE(v.w.transitionAt(QPointF(cut + dx, y)), std::optional<TransitionRef>(ref));
            v.f.selection.clear();
            v.click(QPoint(int(std::floor(cut + dx)), y));
            QCOMPARE(v.f.selection.transition(), ref);
        }
        // Farther out (still in the cut's 7 px trim zone) the edit point is selected.
        v.click(QPoint(int(std::lround(cut - 6)), y));
        QVERIFY(!v.f.selection.transition());
        QCOMPARE(v.c.selectedEdit().tailClip, left);
        v.click(QPoint(int(std::lround(cut + 6)), y));
        QCOMPARE(v.c.selectedEdit().headClip, right);
        // Dragging the outer part trims, the centre is no length handle.
        const int64_t before = ops::clipById(v.f.view(), left)->end();
        v.drag(QPoint(int(std::lround(cut - 6)), y), QPoint(int(std::lround(cut - 26)), y));
        QVERIFY(ops::clipById(v.f.view(), left)->end() < before);
    }

    void spec48_4_wideTransitionKeepsItsLengthHandles()
    {
        View v; // 10 px per frame: a 10-frame transition is 100 px wide
        const quint64 right = v.put(kV1, solidClip(40, 30));
        v.put(kV1, solidClip(10, 30));
        v.f.project.beginEdit(QStringLiteral("setup"));
        TransitionData t;
        t.duration = 10;
        ops::clipById(v.f.data(), right)->transitionIn = t;
        v.f.project.endEdit();
        QCoreApplication::processEvents();
        const int y = v.at(40, kV1).y();
        const double outer = v.w.layout().frameToX(35.0);
        QVERIFY(v.w.transitionAt(QPointF(outer + 20, y)));
        // The outer edge drags the length (spec 13.9).
        v.drag(QPoint(int(outer), y), QPoint(int(outer) - 20, y));
        QVERIFY(ops::clipById(v.f.view(), right)->transitionIn->duration > 10);
    }

    // ---- 48.5 ----

    void spec48_5_openInTimelineMatchesThePlayhead()
    {
        View v;
        const quint64 a = v.put(kV1, solidClip(10, 40));
        v.f.selection.setClips({a});
        QVERIFY(v.c.newCompoundClip(QStringLiteral("C")));
        const quint64 outer = v.f.project.activeTimelineUid();
        // Trim 5 frames off the instance's head: it now shows inner frames 5..39 at 15..49.
        v.f.project.beginEdit(QStringLiteral("trim"));
        ClipData& inst = v.f.data().videoTracks[0].clips[0];
        QCOMPARE(inst.kind, ClipKind::Nested);
        inst.start += 5;
        inst.duration -= 5;
        inst.sourceInUs += 5 * 40000;
        const quint64 instId = inst.id;
        const quint64 nested = inst.nestedUid;
        v.f.project.endEdit();
        v.f.selection.setClips({instId});
        // The playhead over the clip: the inner frame it shows there.
        v.tl()->setPlayhead(30);
        QVERIFY(v.c.openSelectedInTimeline());
        QCOMPARE(v.f.project.activeTimelineUid(), nested);
        QCOMPARE(v.tl()->playhead(), int64_t(20));
        const auto [first, last] = v.w.visibleFrameRange();
        QVERIFY(first <= 20.0 && last >= 20.0);
        // Outside the clip: its first visible frame.
        QVERIFY(v.f.project.setActiveTimeline(outer));
        v.tl()->setPlayhead(80);
        v.f.selection.setClips({instId});
        QVERIFY(v.c.openSelectedInTimeline());
        QCOMPARE(v.f.project.activeTimelineUid(), nested);
        QCOMPARE(v.tl()->playhead(), int64_t(5));
    }
};

QTEST_MAIN(TestBogTimeline)
#include "test_bogtimeline.moc"
