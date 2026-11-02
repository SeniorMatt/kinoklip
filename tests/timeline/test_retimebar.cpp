// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen UI tests for the retime controls: Ctrl+R / Shift+R / Ctrl+Shift+R actions, the speed
// bar geometry, the speed menu, speed point handle drags (ripple, one undo step) and the Change
// Clip Speed dialog.
#include "TimelineTestUtils.h"
#include "core/Retime.h"
#include "ui/timeline/ChangeSpeedDialog.h"
#include "ui/timeline/CurveEditorWidget.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QMenu>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;

namespace {
constexpr TrackRef kV1 {TrackType::Video, 0};

struct View
{
    TimelineFixture f;
    TimelineController c {f.ctx};
    TimelineWidget w {f.ctx, &c};

    View()
    {
        c.connectActions();
        w.setThumbnailProvider({});
        w.setWaveformProvider({});
        w.resize(1200, 500);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setPixelsPerFrame(10.0, kHeaderWidth);
        w.scrollToFrame(0);
    }
    QWidget* vp() { return w.viewport(); }
    quint64 put(ClipData clip)
    {
        f.project.beginEdit(QStringLiteral("setup"));
        const quint64 id = ops::overwriteClip(f.data(), kV1, clip);
        f.project.endEdit();
        return id;
    }
    const ClipData& clip(quint64 id) { return *ops::clipById(f.view(), id); }
    void trigger(const char* action) { f.actions.action(QLatin1String(action))->trigger(); }
    int undoCount() const { return f.project.undoStack()->count(); }
    void drag(QPointF from, QPointF to)
    {
        const QPoint a = from.toPoint();
        const QPoint b = to.toPoint();
        QTest::mousePress(vp(), Qt::LeftButton, {}, a);
        for (int i = 1; i <= 4; ++i)
            QTest::mouseMove(vp(), a + (b - a) * i / 4);
        QTest::mouseRelease(vp(), Qt::LeftButton, {}, b);
    }
};

QAction* menuAction(QMenu* menu, const QString& name)
{
    return menu ? menu->findChild<QAction*>(name) : nullptr;
}

} // namespace

class TestRetimeBar : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_retimebar"); }

    void ctrlRTogglesTheSpeedBar()
    {
        View v;
        const quint64 id = v.put(solidClip(0, 40));
        v.f.selection.setClips({id});
        QVERIFY(v.w.retimeGeometry(id).isEmpty());
        v.trigger("clip.retime_controls");
        QVERIFY(v.c.retimeControlsShown(id));
        const retimebar::Geometry g = v.w.retimeGeometry(id);
        QVERIFY(!g.isEmpty());
        QCOMPARE(g.fields.size(), 1);
        QCOMPARE(g.fields[0].segment.speed, 1.0);
        QVERIFY(g.points.isEmpty());
        v.trigger("clip.retime_controls");
        QVERIFY(!v.c.retimeControlsShown(id));
        QCOMPARE(v.undoCount(), 1); // view state only
    }

    void speedMenuSlowsASegmentWithRipple()
    {
        View v;
        const quint64 id = v.put(solidClip(0, 40));
        const quint64 next = v.put(solidClip(40, 10));
        v.f.selection.setClips({id});
        v.trigger("clip.retime_controls");
        v.f.project.timeline()->setPlayhead(20);
        const int steps = v.undoCount();
        QVERIFY(v.c.addSpeedPointAtPlayhead(id));
        retimebar::Geometry g = v.w.retimeGeometry(id);
        QCOMPARE(g.fields.size(), 2);
        QCOMPARE(g.points.size(), 1);
        QCOMPARE(g.points[0].localFrame, 20);
        // A click on the second field's arrow opens the speed menu.
        QTest::mouseClick(v.vp(), Qt::LeftButton, {}, g.fields[1].arrow.center().toPoint());
        QMenu* menu = v.w.retimeMenu();
        QVERIFY(menu);
        QCOMPARE(menu->objectName(), QStringLiteral("SpeedMenu"));
        QAction* half = menuAction(menu, QStringLiteral("SpeedPreset_50"));
        QVERIFY(half);
        half->trigger();
        menu->close();
        QCOMPARE(v.clip(id).duration, 60);
        QCOMPARE(v.clip(next).start, 60);
        QCOMPARE(v.undoCount(), steps + 2); // add point, change speed
        g = v.w.retimeGeometry(id);
        QCOMPARE(g.fields[1].segment.speed, 0.5);
        v.f.project.undo();
        QCOMPARE(v.clip(id).duration, 40);
        QCOMPARE(v.clip(next).start, 40);
    }

    void speedMenuReverseFreezeReset()
    {
        View v;
        const quint64 id = v.put(solidClip(0, 40));
        v.c.setRetimeControlsShown(id, true);
        v.f.project.timeline()->setPlayhead(10);
        v.w.showSpeedMenu(id, 0, QPoint(300, 100));
        menuAction(v.w.retimeMenu(), QStringLiteral("SpeedFreeze"))->trigger();
        QCOMPARE(v.clip(id).duration, 65); // + 1 s at 25 fps
        QVERIFY(retime::segments(v.clip(id))[1].isFreeze());
        v.w.showSpeedMenu(id, 2, QPoint(300, 100));
        menuAction(v.w.retimeMenu(), QStringLiteral("SpeedReverse"))->trigger();
        QVERIFY(retime::segments(v.clip(id))[2].isReverse());
        v.w.showSpeedMenu(id, 0, QPoint(300, 100));
        menuAction(v.w.retimeMenu(), QStringLiteral("Process_opticalFlow"))->trigger();
        QCOMPARE(v.clip(id).retimeProcess, RetimeProcess::OpticalFlow);
        v.w.showSpeedMenu(id, 0, QPoint(300, 100));
        menuAction(v.w.retimeMenu(), QStringLiteral("SpeedReset"))->trigger();
        QVERIFY(!retime::isRetimed(v.clip(id)));
        v.w.retimeMenu()->close();
    }

    void handleDragsRetimeAndSlide()
    {
        View v;
        const quint64 id = v.put(solidClip(0, 40));
        const quint64 next = v.put(solidClip(40, 10));
        v.c.setRetimeControlsShown(id, true);
        v.f.project.timeline()->setPlayhead(20);
        v.c.addSpeedPointAtPlayhead(id);
        const int steps = v.undoCount();
        // Bottom (retime) handle 10 frames right: the first segment takes 30 frames (66.7 %).
        retimebar::Geometry g = v.w.retimeGeometry(id);
        const QPointF bottom = g.points[0].bottom.center();
        v.drag(bottom, bottom + QPointF(100, 0));
        QCOMPARE(v.undoCount(), steps + 1);
        QCOMPARE(v.clip(id).duration, 50);
        QCOMPARE(v.clip(next).start, 50);
        QVERIFY(std::abs(retime::segments(v.clip(id))[0].speed - 20.0 / 30.0) < 1e-9);
        // Top handle 5 frames left: both speeds stay, the point moves to 25.
        g = v.w.retimeGeometry(id);
        const QPointF top = g.points[0].top.center();
        v.drag(top, top - QPointF(50, 0));
        QCOMPARE(v.undoCount(), steps + 2);
        const QVector<retime::Segment> segs = retime::segments(v.clip(id));
        QCOMPARE(segs[0].endLocal, 25);
        QVERIFY(std::abs(segs[0].speed - 20.0 / 30.0) < 1e-9);
        QVERIFY(std::abs(segs[1].speed - 1.0) < 1e-6);
        // The clip end's retime handle: the last segment.
        g = v.w.retimeGeometry(id);
        const QPointF end = g.endHandle.center();
        v.drag(end, end + QPointF(80, 0));
        QCOMPARE(v.undoCount(), steps + 3);
        QVERIFY(retime::segments(v.clip(id))[1].speed < 1.0);
        v.f.project.undo();
        v.f.project.undo();
        v.f.project.undo();
        QCOMPARE(v.clip(id).duration, 40);
        QCOMPARE(v.clip(next).start, 40);
    }

    void speedPointMenuRampAndDelete()
    {
        View v;
        const quint64 id = v.put(solidClip(0, 40));
        v.c.setRetimeControlsShown(id, true);
        v.f.project.timeline()->setPlayhead(20);
        v.c.addSpeedPointAtPlayhead(id);
        v.w.showSpeedPointMenu(id, 20, QPoint(300, 100));
        menuAction(v.w.retimeMenu(), QStringLiteral("RampSmooth"))->trigger();
        QVERIFY(retime::segments(v.clip(id))[0].rampOut);
        v.w.showSpeedPointMenu(id, 20, QPoint(300, 100));
        menuAction(v.w.retimeMenu(), QStringLiteral("SpeedPointDelete"))->trigger();
        QVERIFY(retime::speedPoints(v.clip(id)).isEmpty());
        v.w.retimeMenu()->close();
    }

    void freezeFrameAndRetimeCurveActions()
    {
        View v;
        const quint64 id = v.put(solidClip(0, 40));
        v.f.selection.setClips({id});
        v.f.project.timeline()->setPlayhead(12);
        v.trigger("clip.freeze_frame");
        QCOMPARE(v.clip(id).speed, 0.0);
        QCOMPARE(v.clip(id).duration, 40);
        v.trigger("clip.retime_curve");
        QVERIFY(v.w.keyframeLanes()->isCurveEditorOpen(id));
        CurveEditorWidget* curves = v.w.keyframeLanes()->curveWidget(id);
        QVERIFY(curves);
        QCOMPARE(curves->shownPaths(), (QVector<QString> {QStringLiteral("retime/retimeFrame")}));
    }

    void changeSpeedDialogLinksTheFields()
    {
        ClipData c = solidClip(0, 50);
        ChangeSpeedDialog d(c, Rational {25, 1}, Rational {50, 1}, nullptr);
        d.speedBox()->setValue(50.0);
        QCOMPARE(d.fpsBox()->value(), 25.0); // 50 % of a 50 fps source
        QCOMPARE(d.durationEdit()->text(), QStringLiteral("00:00:04:00")); // 100 frames
        d.durationEdit()->setText(QStringLiteral("00:00:01:00"));
        emit d.durationEdit()->editingFinished();
        QCOMPARE(d.speedBox()->value(), 200.0);
        d.reverseBox()->setChecked(true);
        d.rippleBox()->setChecked(false);
        d.pitchBox()->setChecked(false);
        const retimeops::SpeedChange r = d.result();
        QCOMPARE(r.speed, 2.0);
        QVERIFY(r.reverse);
        QVERIFY(!r.ripple);
        QVERIFY(!r.pitchCorrection);
        d.freezeBox()->setChecked(true);
        QVERIFY(!d.speedBox()->isEnabled());
    }
};

QTEST_MAIN(TestRetimeBar)
#include "test_retimebar.moc"
