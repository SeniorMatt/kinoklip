// SPDX-License-Identifier: GPL-3.0-or-later
// Behaviour parity tests: one test per item (or group of items) of docs/behavior-spec.md that
// this branch implemented or verified. Test names start with the spec item number.
#include "ParityTestUtils.h"
#include "TimelineTestUtils.h"
#include "core/EditBehaviour.h"
#include "core/MediaPool.h"
#include "core/Serialization.h"
#include "ui/inspector/ParamAccess.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;

using namespace kk::test::parity;

class TestParity : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_parity"); }
    void init() { QSettings().clear(); }

    // ---- 1 Transport ----

    void spec1_keymapMatchesSpec()
    {
        // Shortcut data only (keys are never hard-coded): spec 1.9, 1.12, 2.9, 3.6, 4.7-4.9,
        // 8.9-8.11, 9.2, 16.3.
        TimelineFixture f;
        auto keys = [&](const char* id) {
            QStringList out;
            for (const QKeySequence& k : f.actions.action(QLatin1String(id))->shortcuts())
                out << k.toString(QKeySequence::PortableText);
            return out.join(QLatin1Char('|'));
        };
        QCOMPARE(keys("transport.play_reverse"), QStringLiteral("Shift+Space"));
        QCOMPARE(keys("transport.play_around"), QStringLiteral("/"));
        QCOMPARE(keys("transport.prev_marker"), QStringLiteral("Shift+Up"));
        QCOMPARE(keys("transport.next_marker"), QStringLiteral("Shift+Down"));
        QCOMPARE(keys("mark.selection"), QStringLiteral("Shift+A"));
        QCOMPARE(keys("edit.replace"), QStringLiteral("F11"));
        QCOMPARE(keys("edit.fit_to_fill"), QStringLiteral("Shift+F11"));
        QCOMPARE(keys("edit.append_at_end"), QStringLiteral("Shift+F12"));
        QCOMPARE(keys("edit.split"), QStringLiteral("Ctrl+\\")); // owner: not Ctrl+B
        QCOMPARE(keys("edit.ripple_cut"), QStringLiteral("Ctrl+Shift+X"));
        QCOMPARE(keys("edit.ripple_overwrite"), QStringLiteral("Shift+F10"));
        QCOMPARE(keys("trim.dynamic"), QStringLiteral("W"));
        QCOMPARE(keys("trim.select_nearest_edit"), QStringLiteral("V"));
        QCOMPARE(keys("edit.paste_attributes"), QStringLiteral("Alt+V"));
        QCOMPARE(keys("mark.modify_marker"), QStringLiteral("Ctrl+M"));
        QCOMPARE(keys("clip.match_frame"), QStringLiteral("F"));
        QCOMPARE(keys("view.page_color"), QStringLiteral("Shift+6"));
        QCOMPARE(keys("file.project_settings"), QStringLiteral("Shift+9"));
        QCOMPARE(keys("edit.paste_insert"), QStringLiteral("Ctrl+Shift+V"));
        QCOMPARE(keys("trim.extend_edit"), QStringLiteral("E"));
        QCOMPARE(keys("trim.trim_start"), QStringLiteral("Shift+["));
        QCOMPARE(keys("trim.trim_end"), QStringLiteral("Shift+]"));
        QCOMPARE(keys("trim.ripple_trim_start"), QStringLiteral("Ctrl+Shift+["));
        QCOMPARE(keys("trim.ripple_trim_end"), QStringLiteral("Ctrl+Shift+]"));
        QCOMPARE(keys("trim.nudge_left_multi"), QStringLiteral("Shift+,"));
        QCOMPARE(keys("edit.delete"), QStringLiteral("Backspace"));
        QCOMPARE(keys("edit.ripple_delete"), QStringLiteral("Del|Shift+Backspace"));
    }

    // ---- 2 Playhead & navigation ----

    void spec2_3_upDownOnlyAutoSelectTracks()
    {
        Ctl t;
        t.put(kV1, solidClip(10, 20)); // edges 10, 30
        t.put(kV2, solidClip(50, 10)); // edges 50, 60
        t.tl()->setPlayhead(0);
        t.action("transport.next_edit")->trigger();
        QCOMPARE(t.tl()->playhead(), int64_t(10));
        t.c.toggleTrackFlag(kV1, TimelineController::FlagAutoSelect);
        QVERIFY(!t.f.view().videoTracks[0].autoSelect);
        t.action("transport.next_edit")->trigger();
        QCOMPARE(t.tl()->playhead(), int64_t(50)); // V1's edge at 30 is skipped
        t.action("transport.prev_edit")->trigger();
        QCOMPARE(t.tl()->playhead(), int64_t(50)); // nothing before 50 on V2
        // Locked tracks do not count either.
        t.c.toggleTrackFlag(kV2, TimelineController::FlagLocked);
        t.tl()->setPlayhead(0);
        t.action("transport.next_edit")->trigger();
        QCOMPARE(t.tl()->playhead(), int64_t(0));
    }

    void spec2_4_homeEnd()
    {
        Ctl t;
        t.put(kV1, solidClip(10, 20));
        t.tl()->setPlayhead(15);
        t.action("transport.go_to_end")->trigger();
        QCOMPARE(t.tl()->playhead(), int64_t(30));
        t.action("transport.go_to_start")->trigger();
        QCOMPARE(t.tl()->playhead(), int64_t(0));
    }

    void spec2_9_markerNavigation()
    {
        Ctl t;
        t.tl()->setPlayhead(10);
        t.action("mark.add_marker")->trigger();
        t.tl()->setPlayhead(40);
        t.action("mark.add_marker")->trigger();
        t.tl()->setPlayhead(25);
        t.action("transport.next_marker")->trigger();
        QCOMPARE(t.tl()->playhead(), int64_t(40));
        t.action("transport.next_marker")->trigger();
        QCOMPARE(t.tl()->playhead(), int64_t(40));
        t.action("transport.prev_marker")->trigger();
        QCOMPARE(t.tl()->playhead(), int64_t(10));
    }

    // ---- 3 Marking ----

    void spec3_2_3_outIncludesFrameAndInAfterOutClears()
    {
        Ctl t;
        t.tl()->setPlayhead(20);
        t.action("mark.out")->trigger();
        QCOMPARE(t.f.view().markOut, int64_t(21));
        t.tl()->setPlayhead(30);
        t.action("mark.in")->trigger();
        QCOMPARE(t.f.view().markIn, int64_t(30));
        QCOMPARE(t.f.view().markOut, int64_t(-1));
    }

    void spec3_5_markClipUsesAutoSelectTracks()
    {
        Ctl t;
        t.put(kV1, solidClip(0, 50));
        t.put(kV2, solidClip(10, 20));
        t.tl()->setPlayhead(15);
        t.action("mark.clip")->trigger();
        QCOMPARE(t.f.view().markIn, int64_t(0));
        QCOMPARE(t.f.view().markOut, int64_t(50));
        t.c.toggleTrackFlag(kV1, TimelineController::FlagAutoSelect);
        t.action("mark.clip")->trigger();
        QCOMPARE(t.f.view().markIn, int64_t(10));
        QCOMPARE(t.f.view().markOut, int64_t(30));
    }

    void spec3_6_markSelection()
    {
        Ctl t;
        const quint64 a = t.put(kV1, solidClip(10, 10));
        const quint64 b = t.put(kV2, solidClip(40, 10));
        t.f.selection.setClips({a, b});
        t.action("mark.selection")->trigger();
        QCOMPARE(t.f.view().markIn, int64_t(10));
        QCOMPARE(t.f.view().markOut, int64_t(50));
    }

    // ---- 4 Source edits ----

    void spec4_1_insertRipplesOnlySyncLockTracks()
    {
        Ctl t;
        const QString media = addMedia(t.f.project, true, false);
        t.put(kV1, solidClip(0, 100));
        t.put(kV2, solidClip(60, 10));
        t.c.toggleTrackFlag(kV2, TimelineController::FlagSyncLock);
        t.tl()->setPlayhead(40);
        t.c.insertSource(media, 0, 1'000'000, SourceEditMode::Insert); // 25 frames
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:40 40:25 65:60"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("60:10")); // not rippled
        QCOMPARE(t.tl()->playhead(), int64_t(65)); // spec 4.10
    }

    void spec4_2_editAtTimelineIn()
    {
        Ctl t;
        const QString media = addMedia(t.f.project, true, false);
        t.setMarks(30, -1);
        t.tl()->setPlayhead(80);
        t.c.insertSource(media, 0, 1'000'000, SourceEditMode::Overwrite);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("30:25"));
    }

    void spec4_3_timelineInOutSetsDuration()
    {
        Ctl t;
        const QString media = addMedia(t.f.project, true, false);
        t.setMarks(30, 40);
        t.c.insertSource(media, 0, 4'000'000, SourceEditMode::Overwrite); // source 100 frames
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("30:10"));
        // Three-point editing can be switched off (EditBehaviour): playhead, source length.
        QSettings().setValue(QStringLiteral("edit/threePointEditing"), false);
        t.tl()->setPlayhead(100);
        t.c.insertSource(media, 0, 1'000'000, SourceEditMode::Overwrite);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("30:10 100:25"));
    }

    void spec4_4_timelineOutOnlyBacktimes()
    {
        Ctl t;
        const QString media = addMedia(t.f.project, true, false);
        t.setMarks(-1, 100);
        t.c.insertSource(media, 0, 1'000'000, SourceEditMode::Overwrite);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("75:25"));
    }

    void spec4_7_replaceKeepsPlaceAndLength()
    {
        Ctl t;
        const QString media = addMedia(t.f.project, true, false);
        t.put(kV1, solidClip(0, 20));
        const quint64 mid = t.put(kV1, solidClip(20, 30));
        t.put(kV1, solidClip(50, 20));
        t.tl()->setPlayhead(25);
        const QVector<quint64> ids
            = t.c.insertSource(media, 2'000'000, 3'000'000, SourceEditMode::Replace);
        QCOMPARE(ids.size(), 1);
        QVERIFY(!ops::clipById(t.f.view(), mid));
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:20 20:30 50:20"));
        const ClipData* c = ops::clipById(t.f.view(), ids[0]);
        QCOMPARE(c->kind, ClipKind::Media);
        QCOMPARE(c->sourceInUs, int64_t(2'000'000));
        // Nothing under the playhead: no-op.
        t.tl()->setPlayhead(500);
        QVERIFY(t.c.insertSource(media, 0, 0, SourceEditMode::Replace).isEmpty());
    }

    void spec4_8_fitToFillRetimes()
    {
        Ctl t;
        const QString media = addMedia(t.f.project, true, false);
        // Without four marks nothing happens.
        QVERIFY(t.c.insertSource(media, 0, 2'000'000, SourceEditMode::FitToFill).isEmpty());
        t.setMarks(10, 35); // 25 frames = 1 s
        const QVector<quint64> ids
            = t.c.insertSource(media, 0, 2'000'000, SourceEditMode::FitToFill);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:25"));
        QCOMPARE(ops::clipById(t.f.view(), ids[0])->speed, 2.0);
    }

    void spec4_9_appendAtEnd()
    {
        Ctl t;
        const QString media = addMedia(t.f.project, true, true);
        t.put(kV2, solidClip(0, 70));
        t.tl()->setPlayhead(5);
        t.action("transport.go_to_start")->trigger();
        t.c.insertSource(media, 0, 1'000'000, SourceEditMode::AppendAtEnd);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("70:25"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("70:25"));
        QCOMPARE(t.tl()->playhead(), int64_t(95));
    }

    // ---- 6 Auto track selectors ----

    void spec6_3_autoSelectDefaultOnAndSaved()
    {
        TimelineData d = makeDefaultTimeline(QStringLiteral("t"), Rational {25, 1}, 1920, 1080, 2, 2);
        QVERIFY(d.videoTracks[0].autoSelect);
        d.audioTracks[1].autoSelect = false;
        QString error;
        const QJsonObject json = serialization::timelineToJson(d);
        TimelineData back;
        QVERIFY(serialization::timelineFromJson(json, back, &error));
        QVERIFY(back.videoTracks[0].autoSelect);
        QVERIFY(!back.audioTracks[1].autoSelect);
    }

    void spec6_5_headerButtonTogglesAutoSelect()
    {
        View v;
        const int row = v.w.layout().rowOf(kV1);
        const QRect r = v.w.layout().headerButtonRect(row, HeaderButton::AutoSelect);
        QVERIFY(!r.isEmpty());
        const int before = v.undoCount();
        v.click(r.center());
        QVERIFY(!v.f.view().videoTracks[0].autoSelect);
        QCOMPARE(v.undoCount(), before + 1);
    }

    void spec6_6_altClickSoloesAutoSelect()
    {
        View v;
        const int row = v.w.layout().rowOf(kV2);
        const QRect r = v.w.layout().headerButtonRect(row, HeaderButton::AutoSelect);
        v.click(r.center(), Qt::AltModifier);
        QVERIFY(!v.f.view().videoTracks[0].autoSelect);
        QVERIFY(v.f.view().videoTracks[1].autoSelect);
        QVERIFY(v.f.view().audioTracks[0].autoSelect); // other type untouched
    }

    // ---- 7 Selection ----

    void spec7_3_altClickSelectsOneSide()
    {
        View v;
        const quint64 a = v.put(kV1, solidClip(10, 20));
        const quint64 b = v.put(kA1, solidClip(10, 20));
        v.link({a, b});
        v.click(v.at(20, kV1));
        QCOMPARE(v.f.selection.clips().size(), 2);
        v.click(v.at(20, kV1), Qt::AltModifier);
        QCOMPARE(v.f.selection.clips(), QVector<quint64>({a}));
    }

    void spec7_3_altDragDuplicatesTheLinkedPair()
    {
        // Owner request on 0.4.0: Alt-dragging a clip copies its linked partner too; an
        // Alt-click without a drag still selects one side only.
        View v;
        const quint64 a = v.put(kV1, solidClip(10, 20));
        const quint64 b = v.put(kA1, solidClip(10, 20));
        v.link({a, b});
        v.drag(v.at(20, kV1), v.at(50, kV1), Qt::AltModifier);
        QCOMPARE(v.f.view().track(kV1)->clips.size(), size_t(2));
        QCOMPARE(v.f.view().track(kA1)->clips.size(), size_t(2));
        QCOMPARE(v.f.view().track(kV1)->clips[1].start, int64_t(40));
        QCOMPARE(v.f.view().track(kA1)->clips[1].start, int64_t(40));
        v.click(v.at(20, kV1), Qt::AltModifier);
        QCOMPARE(v.f.selection.clips(), QVector<quint64>({a}));
    }

    void spec7_3_altDragCarriesTransitions()
    {
        // Owner request on 0.4.0: the copy of an Alt-dragged clip keeps its transitions.
        // The transition at the head cut and the one at the tail cut (stored on the clip to
        // the right, which stays behind) both go with the copy.
        View v;
        v.put(kV1, mediaClip(0, 20));
        const quint64 b = v.put(kV1, mediaClip(20, 20));
        const quint64 c = v.put(kV1, mediaClip(40, 20));
        v.f.project.beginEdit(QStringLiteral("setup"));
        TransitionData t;
        t.duration = 6;
        ops::clipById(v.f.data(), b)->transitionIn = t;
        t.duration = 8;
        ops::clipById(v.f.data(), c)->transitionIn = t;
        v.f.project.endEdit();
        v.drag(v.at(30, kV1), v.at(100, kV1), Qt::AltModifier);
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:20 20:20 40:20 90:20"));
        const ClipData& copy = v.f.view().track(kV1)->clips[3];
        QVERIFY(copy.transitionIn.has_value());
        QCOMPARE(copy.transitionIn->duration, int64_t(6));
        QVERIFY(copy.transitionOut.has_value());
        QCOMPARE(copy.transitionOut->duration, int64_t(8));
        QCOMPARE(v.f.view().track(kV1)->clips[1].transitionIn->duration, int64_t(6)); // kept
        QCOMPARE(v.f.view().track(kV1)->clips[2].transitionIn->duration, int64_t(8));
        QVERIFY(!v.f.view().track(kV1)->clips[1].transitionOut);
    }

    void spec7_6_clickEdgeSelectsEditPoint()
    {
        View v;
        const quint64 a = v.put(kV1, solidClip(10, 20));
        const quint64 b = v.put(kV1, solidClip(30, 20));
        // Inside the tail zone of `a`, away from the cut centre: the tail is selected.
        v.click(v.at(30, kV1) - QPoint(5, 0));
        QCOMPARE(v.c.selectedEdit(), (EditPointSelection {a, 0}));
        QVERIFY(v.f.selection.clips().isEmpty());
        // The exact cut: both sides (a roll).
        v.click(v.at(30, kV1));
        QCOMPARE(v.c.selectedEdit(), (EditPointSelection {a, b}));
        // Selecting a clip clears the edit point (spec 7.9).
        v.click(v.at(40, kV1));
        QVERIFY(v.c.selectedEdit().isEmpty());
        QCOMPARE(v.f.selection.clips(), QVector<quint64>({b}));
    }

    // ---- 8 Trimming ----

    void spec8_2_selectToolRollsAtCutCentre()
    {
        View v;
        v.put(kV1, mediaClip(10, 20));
        v.put(kV1, mediaClip(30, 20));
        v.drag(v.at(30, kV1), v.at(35, kV1));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:25 35:15"));
    }

    void spec8_3_trimToolRipplesEitherSideOfCut()
    {
        View v;
        v.put(kV1, mediaClip(10, 20));
        v.put(kV1, mediaClip(30, 20));
        v.c.setTool(TimelineTool::Trim);
        // Left of the cut centre: ripple the tail of the left clip.
        const QPoint p = v.at(30, kV1) - QPoint(5, 0);
        v.drag(p, p - QPoint(50, 0));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:15 25:20"));
    }

    void spec8_6_trimToolNameBandSlides()
    {
        View v;
        v.put(kV1, mediaClip(0, 20));
        const quint64 mid = v.put(kV1, mediaClip(20, 20));
        v.put(kV1, mediaClip(40, 20));
        v.c.setTool(TimelineTool::Trim);
        const QRectF r = v.clipRect(mid);
        const QPoint band(int(r.center().x()), int(r.top() + 5));
        v.drag(band, band + QPoint(50, 0));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:25 25:20 45:15"));
        // Lower part slips (layout unchanged, source moves).
        const int64_t src = ops::clipById(v.f.view(), mid)->sourceInUs;
        const QPoint body(int(r.center().x() + 50), int(r.bottom() - 5));
        v.drag(body, body + QPoint(30, 0));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:25 25:20 45:15"));
        QCOMPARE(ops::clipById(v.f.view(), mid)->sourceInUs, src - 3 * 40000);
    }

    void spec8_8_nudgeTrimsSelectedEdit()
    {
        Ctl t;
        const quint64 a = t.put(kV1, mediaClip(10, 20));
        const quint64 b = t.put(kV1, mediaClip(30, 20));
        t.put(kV1, mediaClip(60, 10));
        // Tail only, selection tool: non-ripple (opens a gap).
        t.c.setSelectedEdit({a, 0});
        t.action("trim.nudge_left")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:19 30:20 60:10"));
        // Roll.
        t.c.setSelectedEdit({0, b});
        t.action("trim.nudge_right")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:19 31:19 60:10"));
        // Trim tool: ripple.
        t.c.setTool(TimelineTool::Trim);
        t.c.setSelectedEdit({b, 0});
        const int before = t.undoCount();
        t.action("trim.nudge_right_multi")->trigger(); // spec 8.9: 5 frames
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:19 31:24 65:10"));
        QCOMPARE(t.undoCount(), before + 1);
    }

    void spec8_16_cycleEditPoint()
    {
        Ctl t;
        const quint64 a = t.put(kV1, solidClip(10, 20));
        const quint64 b = t.put(kV1, solidClip(30, 20));
        t.c.setSelectedEdit({a, 0});
        t.action("trim.cycle_edit")->trigger();
        QCOMPARE(t.c.selectedEdit(), (EditPointSelection {a, b}));
        t.action("trim.cycle_edit")->trigger();
        QCOMPARE(t.c.selectedEdit(), (EditPointSelection {0, b}));
        t.action("trim.cycle_edit")->trigger();
        QCOMPARE(t.c.selectedEdit(), (EditPointSelection {a, 0}));
        // A free edge has one side only.
        t.c.setSelectedEdit({b, 0});
        t.action("trim.cycle_edit")->trigger();
        QCOMPARE(t.c.selectedEdit(), (EditPointSelection {b, 0}));
    }

    void spec8_10_rippleTrimStartEndToPlayhead()
    {
        Ctl t;
        const quint64 v = t.put(kV1, mediaClip(10, 40));
        const quint64 a = t.put(kA1, mediaClip(10, 40));
        t.link({v, a});
        t.put(kV1, mediaClip(50, 10));
        t.tl()->setPlayhead(20);
        const int before = t.undoCount();
        t.action("trim.ripple_trim_start")->trigger();
        // Ripple: 10 frames gone from the head, the following clip moves left.
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:30 40:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("10:30"));
        QCOMPARE(t.tl()->playhead(), int64_t(10));
        QCOMPARE(ops::clipById(t.f.view(), v)->sourceInUs, int64_t(60 * 40000));
        QCOMPARE(t.undoCount(), before + 1);
        t.tl()->setPlayhead(30);
        t.action("trim.ripple_trim_end")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:20 30:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("10:20"));
        QCOMPARE(t.undoCount(), before + 2);
        t.f.project.undoStack()->undo();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:30 40:10"));
    }

    void spec8_10_rippleTrimStartPlayheadLandsOnTheFirstClipsCut()
    {
        // Owner report 1.8.1: with clips on several auto-select tracks the playhead used to
        // jump to the start of the last clip trimmed, a place unrelated to the cut.
        Ctl t;
        t.put(kV1, mediaClip(10, 40));
        t.put(kV2, mediaClip(0, 40));
        t.put(kA1, mediaClip(5, 40));
        t.tl()->setPlayhead(20);
        t.action("trim.ripple_trim_start")->trigger();
        QCOMPARE(t.tl()->playhead(), int64_t(10));
        // Nothing under the playhead on the auto-select tracks: nothing happens.
        t.tl()->setPlayhead(500);
        const int before = t.undoCount();
        t.action("trim.ripple_trim_start")->trigger();
        t.action("trim.ripple_trim_end")->trigger();
        QCOMPARE(t.tl()->playhead(), int64_t(500));
        QCOMPARE(t.undoCount(), before);
    }

    void spec8_10_trimStartEndToPlayheadLeavesGap()
    {
        Ctl t;
        const quint64 v = t.put(kV1, mediaClip(10, 40));
        const quint64 a = t.put(kA1, mediaClip(10, 40));
        t.link({v, a});
        t.put(kV1, mediaClip(50, 10));
        t.tl()->setPlayhead(20);
        t.action("trim.trim_start")->trigger();
        // No ripple: the clip starts at the playhead, the next clip stays.
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("20:30 50:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("20:30"));
        QCOMPARE(t.tl()->playhead(), int64_t(20));
        t.tl()->setPlayhead(30);
        t.action("trim.trim_end")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("20:10 50:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("20:10"));
    }

    void spec8_10_rippleTrimSkipsLockedTracks()
    {
        Ctl t;
        t.put(kV1, mediaClip(10, 40));
        t.put(kV2, mediaClip(10, 40));
        t.c.toggleTrackFlag(kV2, TimelineController::FlagAutoSelect);
        // V2 auto-select on but locked: untouched.
        t.c.toggleTrackFlag(kV2, TimelineController::FlagLocked);
        t.tl()->setPlayhead(20);
        t.action("trim.ripple_trim_end")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:10"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("10:40"));
    }

    void spec8_18_rippleTrimKeepsOtherSyncLockedTracksInSync()
    {
        Ctl t;
        constexpr TrackRef kA2 {TrackType::Audio, 1};
        t.put(kV1, mediaClip(10, 40));
        t.put(kV1, mediaClip(50, 10));
        t.put(kV2, mediaClip(55, 10)); // follows
        t.put(kA1, mediaClip(52, 6)); // follows
        t.put(kA2, mediaClip(52, 6)); // sync lock off: stays
        t.c.toggleTrackFlag(kA2, TimelineController::FlagSyncLock);
        QVERIFY(!t.f.view().audioTracks[1].syncLock);
        t.tl()->setPlayhead(30);
        const int before = t.undoCount();
        t.action("trim.ripple_trim_end")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:20 30:10"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("35:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("32:6"));
        QCOMPARE(layout(t.f.view(), kA2), QStringLiteral("52:6"));
        QCOMPARE(t.undoCount(), before + 1); // one undo step for all tracks
        t.f.project.undoStack()->undo();
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("55:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("52:6"));
    }

    void spec8_18_changeClipDurationKeepsOtherTracksInSync()
    {
        // Owner report 1.8.1: every ripple trim moves the sync-locked tracks, also this one.
        Ctl t;
        const quint64 a = t.put(kV1, mediaClip(10, 40));
        t.put(kV1, mediaClip(50, 10));
        t.put(kV2, mediaClip(500, 10));
        t.f.selection.setClips({a});
        QVERIFY(t.c.setSelectionDuration(30));
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:30 40:10"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("490:10"));
    }

    void spec8_18_spanningClipIsCutAndUndoRestoresIt()
    {
        Ctl t;
        t.put(kV1, mediaClip(10, 40));
        t.put(kV2, mediaClip(0, 80)); // spans the edit point
        // Auto Select off: the playhead command trims only V1, V2 is reached by sync lock only.
        t.c.toggleTrackFlag(kV2, TimelineController::FlagAutoSelect);
        t.tl()->setPlayhead(30);
        t.action("trim.ripple_trim_end")->trigger(); // removes frames 30..50 on every track
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:20"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("0:30 30:30"));
        t.f.project.undoStack()->undo();
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("0:80"));
    }

    void spec97_syncLockDefaultOnAndSaved()
    {
        TimelineData d = makeDefaultTimeline(QStringLiteral("t"), Rational {25, 1}, 1920, 1080, 2, 2);
        QVERIFY(d.videoTracks[0].syncLock);
        d.audioTracks[1].syncLock = false;
        QString error;
        QJsonObject json = serialization::timelineToJson(d);
        TimelineData back;
        QVERIFY(serialization::timelineFromJson(json, back, &error));
        QVERIFY(back.videoTracks[0].syncLock);
        QVERIFY(!back.audioTracks[1].syncLock);
        // An older project has no "syncLock" field: every track keeps its sync lock on.
        d.audioTracks[1].syncLock = true;
        json = serialization::timelineToJson(d);
        TimelineData old;
        QVERIFY(serialization::timelineFromJson(json, old, &error));
        QVERIFY(old.audioTracks[1].syncLock);
    }

    void spec8_11_extendEdit()
    {
        Ctl t;
        const quint64 a = t.put(kV1, mediaClip(10, 20));
        t.c.setSelectedEdit({a, 0});
        t.tl()->setPlayhead(45);
        t.action("trim.extend_edit")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("10:35"));
    }

    // ---- 9 Blade ----

    void spec9_2_splitAtPlayheadUsesAutoSelect()
    {
        Ctl t;
        t.put(kV1, solidClip(0, 50));
        t.put(kV2, solidClip(0, 50));
        t.c.toggleTrackFlag(kV2, TimelineController::FlagAutoSelect);
        t.tl()->setPlayhead(20);
        t.action("edit.split")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:20 20:30"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("0:50"));
    }

    // ---- 10 Delete, ripple delete, gaps ----

    void spec10_5_gapSelectionAndDelete()
    {
        View v;
        v.put(kV1, solidClip(0, 10));
        v.put(kV1, solidClip(30, 10));
        // Nothing selected: ripple delete does nothing.
        v.tl()->setPlayhead(20);
        v.action("edit.ripple_delete")->trigger();
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:10 30:10"));
        // Click the gap, then Backspace (delete) closes it in one undo step.
        v.click(v.at(20, kV1));
        QVERIFY(v.c.selectedGap().has_value());
        QCOMPARE(v.c.selectedGap()->start, int64_t(10));
        QCOMPARE(v.c.selectedGap()->end, int64_t(30));
        const int before = v.undoCount();
        v.action("edit.delete")->trigger();
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(v.undoCount(), before + 1);
        QVERIFY(!v.c.selectedGap());
        // The old convenience can be switched on (EditBehaviour).
        v.put(kV1, solidClip(40, 5));
        QSettings().setValue(QStringLiteral("edit/rippleDeleteGapAtPlayhead"), true);
        v.f.selection.clear();
        v.tl()->setPlayhead(30);
        v.action("edit.ripple_delete")->trigger();
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:10 10:10 20:5"));
    }

    // ---- 13 Transitions ----

    void spec13_2_defaultTransitionPlacement()
    {
        Ctl t;
        const quint64 a = t.put(kV1, solidClip(0, 50));
        const quint64 b = t.put(kV1, solidClip(50, 50));
        const quint64 c = t.put(kV1, solidClip(120, 50));
        // Selected clip: both ends (the cut with `a` is stored on b's head).
        t.f.selection.setClips({b});
        t.action("timeline.add_transition")->trigger();
        QVERIFY(ops::clipById(t.f.view(), b)->transitionIn);
        QVERIFY(ops::clipById(t.f.view(), b)->transitionOut);
        QCOMPARE(ops::clipById(t.f.view(), b)->transitionIn->duration, int64_t(25));
        QVERIFY(!ops::clipById(t.f.view(), a)->transitionOut);
        // Selected edit point.
        t.c.setSelectedEdit({0, c});
        t.action("timeline.add_transition")->trigger();
        QVERIFY(ops::clipById(t.f.view(), c)->transitionIn);
        // Standard length from the project (Project Settings > General Options).
        ProjectSettings settings = t.f.project.settings();
        settings.stdTransitionFrames = 50;
        t.f.project.setSettings(settings);
        t.c.setSelectedEdit({c, 0});
        t.action("timeline.add_transition")->trigger();
        QCOMPARE(ops::clipById(t.f.view(), c)->transitionOut->duration, int64_t(50));
    }

    // ---- 14 Titles, generators, stills ----

    void spec14_1_generatorLengthFromBehaviour()
    {
        QSettings().setValue(QStringLiteral("edit/generatorSeconds"), 10.0);
        const auto title
            = TimelineController::makeGeneratorClip(QStringLiteral("text_title"), Rational {25, 1});
        QCOMPARE(title->duration, int64_t(250));
        QCOMPARE(TimelineController::defaultStillFrames(Rational {25, 1}), int64_t(125));
        QCOMPARE(EditBehaviour::frames(1.0, Rational {30000, 1001}), int64_t(30));
    }

    // ---- 15 Keyframes ----

    void spec15_6_keyInterpolation()
    {
        ClipData c = solidClip(0, 50);
        const ParamRef ref {1, ParamScope::Transform, 0, keys::Opacity};
        AnimatedValue& v = c.transform.animated(keys::Opacity, 1.0);
        v.setKey(0, 0.0);
        v.setKey(40, 1.0);
        QCOMPARE(paramaccess::keyInterp(c, ref, 0), std::optional<Interp>(Interp::Linear));
        QVERIFY(!paramaccess::keyInterp(c, ref, 10));
        QVERIFY(!paramaccess::setKeyInterp(c, ref, 10, Interp::Hold)); // no key there
        QVERIFY(paramaccess::setKeyInterp(c, ref, 0, Interp::Hold));
        QCOMPARE(c.transform.value(keys::Opacity, 20, 1.0), 0.0); // held until the next key
        QVERIFY(paramaccess::setKeyInterp(c, ref, 0, Interp::EaseInOut));
        // Per-key presets (spec 15.11): only the side leaving key 0 eases so far...
        QCOMPARE(c.transform.value(keys::Opacity, 20, 1.0), 0.375);
        // ...easing into key 40 as well makes the symmetric curve.
        QVERIFY(paramaccess::setKeyInterp(c, ref, 40, Interp::EaseInOut));
        QCOMPARE(c.transform.value(keys::Opacity, 20, 1.0), 0.5); // symmetric curve midpoint
    }

    // ---- 16 Clipboard ----

    void spec16_6_pasteCarriesTransitions()
    {
        // Owner request on 0.4.0: Ctrl+C / Ctrl+V of a clip takes its transitions along (as
        // Alt-drag, spec 7.3), shortened to fit a new neighbour.
        Ctl t;
        t.put(kV1, mediaClip(0, 20));
        const quint64 b = t.put(kV1, mediaClip(20, 20));
        const quint64 c = t.put(kV1, mediaClip(40, 20));
        t.put(kV1, mediaClip(198, 2)); // a 2-frame clip the second paste touches
        t.f.project.beginEdit(QStringLiteral("setup"));
        TransitionData tx;
        tx.duration = 6;
        ops::clipById(t.f.data(), b)->transitionIn = tx;
        tx.duration = 8;
        ops::clipById(t.f.data(), c)->transitionIn = tx;
        t.f.project.endEdit();
        t.f.selection.setClips({b});
        t.action("edit.copy")->trigger();
        t.tl()->setPlayhead(100);
        t.action("edit.paste")->trigger();
        const ClipData* copy = ops::clipById(t.f.view(), t.f.selection.clips().value(0));
        QVERIFY(copy && copy->start == 100);
        QCOMPARE(copy->transitionIn->duration, int64_t(6));
        QCOMPARE(copy->transitionOut->duration, int64_t(8));
        // After a 2-frame clip the centred head transition fits 5 frames (2 * 2 + 1).
        t.tl()->setPlayhead(200);
        t.action("edit.paste")->trigger();
        copy = ops::clipById(t.f.view(), t.f.selection.clips().value(0));
        QVERIFY(copy && copy->start == 200);
        QCOMPARE(copy->transitionIn->duration, int64_t(5));
        QString why;
        QVERIFY2(ops::checkInvariants(t.f.view(), &why), qPrintable(why));
    }

    void spec16_3_pasteInsertRipples()
    {
        Ctl t;
        const quint64 a = t.put(kV1, solidClip(0, 10));
        t.put(kV1, solidClip(10, 10));
        t.f.selection.setClips({a});
        t.action("edit.copy")->trigger();
        t.tl()->setPlayhead(10);
        t.action("edit.paste_insert")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:10 20:10"));
        QCOMPARE(t.tl()->playhead(), int64_t(20));
        // Plain paste overwrites (spec 16.2).
        t.tl()->setPlayhead(5);
        t.action("edit.paste")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:5 5:10 15:5 20:10"));
    }
};

QTEST_MAIN(TestParity)
#include "test_parity.moc"
