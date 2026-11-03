// SPDX-License-Identifier: GPL-3.0-or-later
// Transitions as timeline items (spec 13.10, alpha.2 feedback #6): click selects (exclusive with
// clips), Delete / Backspace remove (one undo step), double-click asks for the Inspector, locked
// tracks are left alone; the fx badge opens the clip's effects (spec 24.3); a clip with a
// composition shows the node badge (spec 42b.18).
#include "ParityTestUtils.h"
#include "TimelineTestUtils.h"
#include "core/Composition.h"
#include "ui/common/SchemeColors.h"
#include "ui/timeline/TimelinePainter.h"

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

    void spec42b_18_compositionBadge()
    {
        // A clip with a composition shows the node badge (keyframe-coloured node boxes) at its
        // right end, left of the fx badge when it also has effects; a plain clip shows none.
        View v;
        ClipData plain = solidClip(10, 30);
        ClipData comp = solidClip(50, 30);
        comp.composition = Composition {};
        ClipData both = solidClip(90, 30);
        both.composition = Composition {};
        EffectInstance fx;
        fx.type = QStringLiteral("gaussian_blur");
        both.effects.append(fx);
        const quint64 plainId = v.put(kV1, plain);
        const quint64 compId = v.put(kV1, comp);
        const quint64 bothId = v.put(kV1, both);
        QCoreApplication::processEvents();
        const QImage img = v.vp()->grab().toImage();
        const QColor mark = schemeColors().keyframe;
        auto marked = [&](const QRectF& r) {
            int n = 0;
            const QRect area = r.toAlignedRect() & img.rect();
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x)
                    n += img.pixelColor(x, y) == mark ? 1 : 0;
            return n;
        };
        const double right = double(v.w.layout().viewSize().width());
        const QRectF plainBadge = TimelinePainter::compBadgeRect(v.clipRect(plainId), right, false);
        const QRectF compBadge = TimelinePainter::compBadgeRect(v.clipRect(compId), right, false);
        const QRectF bothBadge = TimelinePainter::compBadgeRect(v.clipRect(bothId), right, true);
        const QRectF bothFx = TimelinePainter::fxBadgeRect(v.clipRect(bothId), right);
        QVERIFY(!compBadge.isEmpty() && !bothBadge.isEmpty());
        QCOMPARE(marked(plainBadge), 0);
        QVERIFY2(marked(compBadge) >= 20, qPrintable(QString::number(marked(compBadge))));
        QVERIFY(marked(bothBadge) >= 20);
        QVERIFY(bothBadge.right() <= bothFx.left());
        QVERIFY(!bothBadge.intersects(bothFx));
    }
};

QTEST_MAIN(TestTransitionSelect)
#include "test_transitionselect.moc"
