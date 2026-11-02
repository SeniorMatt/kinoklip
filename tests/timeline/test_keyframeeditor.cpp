// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen UI tests for the timeline keyframe editor and curve editor (KeyframeLanes,
// KeyframeTrackWidget, CurveEditorWidget): docs/behavior-spec.md 15.8 - 15.12.
#include "TimelineTestUtils.h"
#include "core/KeyframeOps.h"
#include "ui/timeline/CurveEditorWidget.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/KeyframeTrackWidget.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;
namespace ko = kk::keyframeops;

namespace {
constexpr TrackRef kV1 {TrackType::Video, 0};
const QString kPosX = QStringLiteral("transform/posX");
const QString kOpacity = QStringLiteral("transform/opacity");

struct View
{
    TimelineFixture f;
    TimelineController c {f.ctx};
    TimelineWidget w {f.ctx, &c};
    quint64 clipId = 0;

    View()
    {
        c.connectActions();
        w.setThumbnailProvider({});
        w.setWaveformProvider({});
        w.resize(1200, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setPixelsPerFrame(10.0, kHeaderWidth);
        w.scrollToFrame(0);
        // A 60-frame clip at frame 10, animated: posX keys 0 / 20 / 40, opacity key 20.
        ClipData clip = solidClip(10, 60);
        clip.transform.animated(keys::PosX, 0).setKey(0, 0.0);
        clip.transform.animated(keys::PosX, 0).setKey(20, 100.0);
        clip.transform.animated(keys::PosX, 0).setKey(40, 50.0);
        clip.transform.animated(keys::Opacity, 100).setKey(20, 50.0);
        f.project.beginEdit(QStringLiteral("setup"));
        clipId = ops::overwriteClip(f.data(), kV1, clip);
        f.project.endEdit();
        f.project.undoStack()->clear();
    }
    KeyframeLanes* lanes() { return w.keyframeLanes(); }
    const ClipData& clip() { return *ops::clipById(f.view(), clipId); }
    const AnimatedValue& value(const QString& path) { return *ko::findValue(clip(), path); }
    KeyframeTrackWidget* keys() { return lanes()->keyframeWidget(clipId); }
    CurveEditorWidget* curves() { return lanes()->curveWidget(clipId); }
    int rowOf(const QString& label)
    {
        const auto rows = keys()->rows();
        for (int i = 0; i < rows.size(); ++i) {
            if (rows[i].label == label)
                return i;
        }
        return -1;
    }
    int undoCount() const { return f.project.undoStack()->count(); }
};

void drag(QWidget* w, QPoint from, QPoint to, Qt::KeyboardModifiers mods = {})
{
    QTest::mousePress(w, Qt::LeftButton, mods, from);
    for (int i = 1; i <= 4; ++i)
        QTest::mouseMove(w, from + (to - from) * i / 4);
    QTest::mouseRelease(w, Qt::LeftButton, mods, to);
}
} // namespace

class TestKeyframeEditor : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_keyframeeditor"); }

    // 15.8: the toggle buttons on an animated clip open the keyframe / curve lanes.
    void spec15_8_clipButtonsOpenLanes()
    {
        View v;
        const TimelineLayout& L = v.w.layout();
        const int row = L.rowOf(kV1);
        const int heightBefore = L.rowRect(row).height();
        const auto [keysButton, curvesButton] = v.lanes()->clipButtonRects(row, v.clip());
        QVERIFY(!keysButton.isEmpty());
        QTest::mouseClick(v.w.viewport(), Qt::LeftButton, {}, keysButton.center());
        QVERIFY(v.lanes()->isKeyframeEditorOpen(v.clipId));
        QVERIFY(v.keys() && v.keys()->isVisible());
        QCOMPARE(L.rowRect(row).height(), heightBefore + KeyframeTrackWidget::heightFor(v.clip()));
        // The clip keeps its own height above the lane.
        QCOMPARE(int(L.clipRect(row, v.clip()).height()), heightBefore - 2);
        const auto rects = v.lanes()->clipButtonRects(row, v.clip());
        QTest::mouseClick(v.w.viewport(), Qt::LeftButton, {}, rects.second.center());
        QVERIFY(v.lanes()->isCurveEditorOpen(v.clipId));
        QVERIFY(v.curves() && v.curves()->isVisible());
        QCOMPARE(v.undoCount(), 0); // view state, not an edit
        // Clicking again closes.
        QTest::mouseClick(v.w.viewport(), Qt::LeftButton, {},
            v.lanes()->clipButtonRects(row, v.clip()).first.center());
        QVERIFY(!v.lanes()->isKeyframeEditorOpen(v.clipId));
    }

    void spec15_8_actionsToggleSelectedClip()
    {
        View v;
        v.f.selection.selectClip(v.clipId);
        v.f.actions.action(QStringLiteral("clip.toggle_keyframe_editor"))->trigger();
        v.f.actions.action(QStringLiteral("clip.toggle_curve_editor"))->trigger();
        QVERIFY(v.lanes()->isKeyframeEditorOpen(v.clipId));
        QVERIFY(v.lanes()->isCurveEditorOpen(v.clipId));
        // Rows: master, Transform group, Position X, Opacity.
        const auto rows = v.keys()->rows();
        QCOMPARE(rows.size(), 4);
        QCOMPARE(rows[0].kind, KeyRow::Kind::Master);
        QCOMPARE(rows[1].kind, KeyRow::Kind::Group);
        // Deleting the clip drops its editor.
        v.f.project.beginEdit(QStringLiteral("del"));
        ops::liftClips(v.f.data(), {v.clipId});
        v.f.project.endEdit();
        QVERIFY(!v.lanes()->isKeyframeEditorOpen(v.clipId));
    }

    // 15.9: click selects, drag moves in whole frames (one undo step), Esc-free release.
    void spec15_9_dragMovesSelectedKey()
    {
        View v;
        v.lanes()->setKeyframeEditorOpen(v.clipId, true);
        KeyframeTrackWidget* k = v.keys();
        const int row = v.rowOf(QStringLiteral("Position X"));
        QVERIFY(row > 0);
        const QPoint from = k->diamondPos(row, 20);
        drag(k, from, from + QPoint(50, 0)); // 10 px per frame
        QCOMPARE(v.value(kPosX).keys[1].frame, 25);
        QCOMPARE(v.value(kOpacity).keys[0].frame, 20); // other parameters untouched
        QCOMPARE(v.f.selection.keyframes(), QVector<KeyframeRef>({{v.clipId, kPosX, 25}}));
        QCOMPARE(v.undoCount(), 1);
        v.f.project.undo();
        QCOMPARE(v.value(kPosX).keys[1].frame, 20);
    }

    // The master diamond grabs every key at its frame; drags clamp to the clip.
    void spec15_9_masterDiamondMovesAllKeysAtFrame()
    {
        View v;
        v.lanes()->setKeyframeEditorOpen(v.clipId, true);
        KeyframeTrackWidget* k = v.keys();
        const QPoint from = k->diamondPos(0, 20);
        drag(k, from, from + QPoint(-1000, 0));
        // Clamped at clip-local frame 0? posX already has a key there: the moved key replaces
        // it; opacity's key lands at 0 too.
        QCOMPARE(v.value(kOpacity).keys[0].frame, 0);
        QCOMPARE(v.value(kPosX).keys.size(), 2);
        QCOMPARE(v.value(kPosX).keys[0].value, 100.0);
    }

    void spec15_9_shiftClickAndRubberBand()
    {
        View v;
        v.lanes()->setKeyframeEditorOpen(v.clipId, true);
        KeyframeTrackWidget* k = v.keys();
        const int row = v.rowOf(QStringLiteral("Position X"));
        QTest::mouseClick(k, Qt::LeftButton, {}, k->diamondPos(row, 0));
        QTest::mouseClick(k, Qt::LeftButton, Qt::ShiftModifier, k->diamondPos(row, 40));
        QCOMPARE(v.f.selection.keyframes().size(), 2);
        QTest::mouseClick(k, Qt::LeftButton, Qt::ControlModifier, k->diamondPos(row, 0));
        QCOMPARE(v.f.selection.keyframes(), QVector<KeyframeRef>({{v.clipId, kPosX, 40}}));
        // Rubber band over frames 15..25 of all parameter rows: posX 20 and opacity 20.
        const QPoint a = k->diamondPos(row, 15) - QPoint(0, 4);
        const QPoint b = k->diamondPos(v.rowOf(QStringLiteral("Opacity")), 25) + QPoint(0, 4);
        drag(k, a, b);
        QCOMPARE(v.f.selection.keyframes().size(), 2);
        QVERIFY(v.f.selection.keyframes().contains({v.clipId, kOpacity, 20}));
        QVERIFY(v.f.selection.isSelected(v.clipId));
    }

    void spec15_9_deleteKeyWithDeleteKey()
    {
        View v;
        v.lanes()->setKeyframeEditorOpen(v.clipId, true);
        KeyframeTrackWidget* k = v.keys();
        const int row = v.rowOf(QStringLiteral("Opacity"));
        QTest::mouseClick(k, Qt::LeftButton, {}, k->diamondPos(row, 20));
        k->setFocus();
        QTest::keyClick(k, Qt::Key_Delete);
        QVERIFY(!v.value(kOpacity).isAnimated());
        QVERIFY(ops::clipById(v.f.view(), v.clipId)); // the clip itself stays
        QCOMPARE(v.undoCount(), 1);
    }

    void spec15_9_doubleClickJumpsPlayhead()
    {
        View v;
        v.lanes()->setKeyframeEditorOpen(v.clipId, true);
        KeyframeTrackWidget* k = v.keys();
        QTest::mouseDClick(k, Qt::LeftButton, {}, k->diamondPos(0, 40));
        QCOMPARE(v.f.project.timeline()->playhead(), int64_t(50));
    }

    void spec15_9_snapsToPlayhead()
    {
        View v;
        v.lanes()->setKeyframeEditorOpen(v.clipId, true);
        v.f.project.timeline()->setPlayhead(10 + 33);
        KeyframeTrackWidget* k = v.keys();
        const int row = v.rowOf(QStringLiteral("Position X"));
        const QPoint from = k->diamondPos(row, 20);
        drag(k, from, from + QPoint(125, 0)); // 12.5 frames -> near 33: snaps
        QCOMPARE(v.value(kPosX).keys[1].frame, 33);
    }

    void spec15_9_copyPasteAndInterpViaActions()
    {
        View v;
        v.f.selection.selectClip(v.clipId);
        v.f.selection.setKeyframes({{v.clipId, kPosX, 20}, {v.clipId, kOpacity, 20}});
        v.f.actions.action(QStringLiteral("clip.copy_keyframes"))->trigger();
        v.f.project.timeline()->setPlayhead(10 + 50);
        v.f.actions.action(QStringLiteral("clip.paste_keyframes"))->trigger();
        QVERIFY(v.value(kPosX).hasKeyAt(50));
        QVERIFY(v.value(kOpacity).hasKeyAt(50));
        QCOMPARE(v.f.selection.keyframes().size(), 2);
        QVERIFY(v.lanes()->setSelectedInterp(Interp::Hold));
        QVERIFY(v.value(kPosX).keys.back().hold);
        v.f.actions.action(QStringLiteral("clip.delete_keyframe"))->trigger();
        QVERIFY(!v.value(kPosX).hasKeyAt(50));
    }

    // 15.10 / 15.12: the curve editor moves keys in value and drags Bezier handles.
    void spec15_10_curveDragChangesValue()
    {
        View v;
        v.lanes()->setCurveEditorOpen(v.clipId, true);
        CurveEditorWidget* cw = v.curves();
        cw->setCurveShown(kOpacity, false);
        const QPointF p = cw->keyPos(kPosX, 40);
        QVERIFY(!p.isNull());
        QTest::mouseClick(cw, Qt::LeftButton, {}, p.toPoint());
        QCOMPARE(v.f.selection.keyframes(), QVector<KeyframeRef>({{v.clipId, kPosX, 40}}));
        drag(cw, p.toPoint(), p.toPoint() + QPoint(0, -30), Qt::ShiftModifier);
        QCOMPARE(v.value(kPosX).keys[2].frame, 40); // Shift: value only
        QVERIFY(v.value(kPosX).keys[2].value > 50.0);
        QCOMPARE(v.undoCount(), 1);
    }

    void spec15_12_curveHandleDrag()
    {
        View v;
        v.lanes()->setCurveEditorOpen(v.clipId, true);
        CurveEditorWidget* cw = v.curves();
        cw->setCurveShown(kOpacity, false);
        v.f.selection.selectClip(v.clipId);
        v.f.selection.setKeyframes({{v.clipId, kPosX, 20}});
        QVERIFY(v.lanes()->setSelectedInterp(Interp::Smooth));
        const QPointF tip = cw->handlePos(kPosX, 20, ko::HandleSide::Out);
        QVERIFY(!tip.isNull());
        drag(cw, tip.toPoint(), tip.toPoint() + QPoint(30, 20));
        const Keyframe& key = v.value(kPosX).keys[1];
        QCOMPARE(key.out.mode, KeyHandle::Mode::Free);
        QCOMPARE(key.in.mode, KeyHandle::Mode::Free); // kept smooth (collinear)
        QVERIFY(std::abs(key.out.dv / key.out.dtFrames - key.in.dv / key.in.dtFrames) < 1e-9);
        QCOMPARE(v.undoCount(), 2);
    }

    void spec15_10_curveToolbarAndDoubleClickAddsKey()
    {
        View v;
        v.lanes()->setCurveEditorOpen(v.clipId, true);
        CurveEditorWidget* cw = v.curves();
        cw->setCurveShown(kOpacity, false);
        cw->setCurveShown(kPosX, true);
        const QPointF a = cw->keyPos(kPosX, 0);
        const QPointF b = cw->keyPos(kPosX, 20);
        // Double-click halfway between two keys, away from the curve: a key with the curve's
        // value there.
        QTest::mouseDClick(cw, Qt::LeftButton, {}, QPoint(int((a.x() + b.x()) / 2), 5));
        QVERIFY(v.value(kPosX).hasKeyAt(10));
        QCOMPARE(v.value(kPosX).valueAt(10), 50.0);
        v.f.selection.setKeyframes({{v.clipId, kPosX, 10}});
        auto* ease = cw->findChild<QToolButton*>(QStringLiteral("interp_ease_in_out"));
        QVERIFY(ease);
        ease->click();
        QCOMPARE(keyInterp(v.value(kPosX).keys[1]), Interp::EaseInOut);
    }

    // 15.20: a clip too narrow to edit its keys inline offers the Keyframes panel.
    void spec15_20_narrowClipOffersKeyframesPanel()
    {
        View v;
        v.lanes()->setCurveEditorOpen(v.clipId, true);
        QApplication::processEvents();
        QToolButton* open = v.lanes()->openPanelButton(v.clipId);
        QVERIFY(!open || !open->isVisible()); // 600 px wide at 10 px per frame
        v.w.setPixelsPerFrame(1.0, kHeaderWidth); // 60 px: cramped
        QApplication::processEvents();
        open = v.lanes()->openPanelButton(v.clipId);
        QVERIFY(open && open->isVisible());
        QSignalSpy spy(v.lanes(), &KeyframeLanes::openInPanelRequested);
        QTest::mouseClick(open, Qt::LeftButton);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.front().front().toULongLong(), v.clipId);
    }
};

QTEST_MAIN(TestKeyframeEditor)
#include "test_keyframeeditor.moc"
