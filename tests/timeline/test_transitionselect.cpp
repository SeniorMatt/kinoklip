// SPDX-License-Identifier: GPL-3.0-or-later
// Transitions as timeline items (spec 13.10, alpha.2 feedback #6): click selects (exclusive with
// clips), Delete / Backspace remove (one undo step), double-click asks for the Inspector, locked
// tracks are left alone; the fx badge opens the clip's effects (spec 24.3).
#include "ParityTestUtils.h"
#include "TimelineTestUtils.h"

#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
using namespace kk::test::parity;

namespace {

/// Two touching clips [10,40) [40,70) on V1 with a 10-frame centred transition at the cut,
/// window [35,45).
struct TransitionView : View
{
    quint64 left = 0;
    quint64 right = 0;
    TransitionView()
    {
        left = put(kV1, solidClip(10, 30));
        right = put(kV1, solidClip(40, 30));
        f.project.beginEdit(QStringLiteral("setup"));
        TransitionData t;
        t.duration = 10;
        ops::clipById(f.data(), right)->transitionIn = t;
        f.project.endEdit();
        QCoreApplication::processEvents();
    }
    [[nodiscard]] bool hasTransition() const
    {
        return ops::clipById(f.view(), right)->transitionIn.has_value();
    }
};

} // namespace

class TestTransitionSelect : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_transitionselect"); }

    void spec13_10_clickSelectsTransition()
    {
        TransitionView v;
        v.f.selection.setClips({v.left});
        v.click(v.at(42, kV1));
        QCOMPARE(v.f.selection.transition(), (TransitionRef {v.right, true}));
        QVERIFY(v.f.selection.clips().isEmpty());
        QCOMPARE(v.w.transitionAt(v.at(36, kV1)), (TransitionRef {v.right, true}));
        QVERIFY(!v.w.transitionAt(v.at(50, kV1)));
        // A click on a clip body selects the clip and drops the transition.
        v.click(v.at(60, kV1));
        QVERIFY(!v.f.selection.transition());
        QCOMPARE(v.f.selection.clips(), QVector<quint64> {v.right});
    }

    void spec13_10_deleteKeysRemoveSelectedTransition_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("Backspace") << QStringLiteral("edit.delete");
        QTest::newRow("Delete") << QStringLiteral("edit.ripple_delete");
    }
    void spec13_10_deleteKeysRemoveSelectedTransition()
    {
        QFETCH(QString, action);
        TransitionView v;
        v.click(v.at(42, kV1));
        const int before = v.undoCount();
        v.f.actions.action(action)->trigger();
        QVERIFY(!v.hasTransition());
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:30 40:30")); // clips untouched
        QCOMPARE(v.undoCount(), before + 1);
        QVERIFY(v.f.selection.isEmpty());
        v.f.project.undoStack()->undo();
        QVERIFY(v.hasTransition());
    }

    void spec13_10_lockedTrackKeepsTransition()
    {
        TransitionView v;
        v.c.toggleTrackFlag(kV1, TimelineController::FlagLocked);
        QVERIFY(!v.c.deleteTransition({v.right, true}));
        QVERIFY(v.hasTransition());
    }

    void spec13_10_doubleClickOpensInspector()
    {
        TransitionView v;
        QSignalSpy spy(&v.w, &TimelineWidget::inspectRequested);
        QTest::mouseDClick(v.vp(), Qt::LeftButton, {}, v.at(42, kV1));
        QVERIFY(!spy.isEmpty());
        QCOMPARE(spy.last().at(0).toULongLong(), v.right);
        QCOMPARE(spy.last().at(1).value<TimelineWidget::InspectTarget>(),
            TimelineWidget::InspectTarget::Transition);
        QCOMPARE(v.f.selection.transition(), (TransitionRef {v.right, true}));
    }

    void spec13_10_durationAndAlignmentCommands()
    {
        TransitionView v;
        QVERIFY(v.c.setTransitionDuration(v.right, true, 6));
        QCOMPARE(ops::clipById(v.f.view(), v.right)->transitionIn->duration, int64_t(6));
        QVERIFY(v.c.setTransitionAlignment(v.right, true, TransitionAlignment::StartOnCut));
        QCOMPARE(v.w.transitionAt(v.at(42, kV1)), (TransitionRef {v.right, true}));
        QVERIFY(!v.w.transitionAt(v.at(38, kV1))); // now starts on the cut
    }

    void spec24_3_fxBadgeOpensEffects()
    {
        View v;
        ClipData c = solidClip(10, 60);
        EffectInstance fx;
        fx.type = QStringLiteral("gaussian_blur");
        c.effects.append(fx);
        const quint64 id = v.put(kV1, c);
        QCoreApplication::processEvents();
        const TimelineLayout& L = v.w.layout();
        const ClipData* clip = ops::clipById(v.f.view(), id);
        const QRectF badge = TimelinePainter::fxBadgeRect(
            L.clipRect(L.rowOf(kV1), *clip), double(L.viewSize().width()));
        QVERIFY(!badge.isEmpty());
        QCOMPARE(v.w.fxBadgeAt(badge.center()), id);
        QSignalSpy spy(&v.w, &TimelineWidget::inspectRequested);
        v.click(badge.center().toPoint());
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.last().at(1).value<TimelineWidget::InspectTarget>(),
            TimelineWidget::InspectTarget::Effects);
        QCOMPARE(v.f.selection.currentClip(), id);
    }
};

QTEST_MAIN(TestTransitionSelect)
#include "test_transitionselect.moc"
