// SPDX-License-Identifier: GPL-3.0-or-later
// test_trimeditor — Trim > Trim Editor (spec 197): the window of its own with the outgoing and
// incoming frames side by side and the trim buttons.
#include "TimelineTestUtils.h"
#include "ui/common/ActionRegistry.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TrimEditorWindow.h"
#include "ui/viewer/TrimTwoUpView.h"

#include <QAction>
#include <QLabel>
#include <QPushButton>
#include <QTest>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

namespace {

struct Rig
{
    TimelineFixture f;
    TimelineController c {f.ctx};
    quint64 a = 0, b = 0, d = 0;
    Rig()
    {
        c.connectActions();
        f.project.beginEdit(QStringLiteral("setup"));
        a = ops::overwriteClip(
            f.data(), {TrackType::Video, 0}, solidClip(0, 100, QStringLiteral("Out")));
        b = ops::overwriteClip(
            f.data(), {TrackType::Video, 0}, solidClip(100, 100, QStringLiteral("In")));
        d = ops::overwriteClip(
            f.data(), {TrackType::Video, 0}, solidClip(250, 50, QStringLiteral("Far")));
        f.project.endEdit();
    }
    const ClipData* clip(quint64 id) const { return ops::clipById(f.view(), id); }
};

} // namespace

class TestTrimEditor : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_trimeditor"); }

    void previewIsTheTwoFramesAroundTheCut()
    {
        Rig r;
        // Tail only, head only and both give the same cut when the clips touch.
        for (const EditPointSelection& edit : {EditPointSelection {r.a, 0},
                 EditPointSelection {0, r.b}, EditPointSelection {r.a, r.b}}) {
            const TrimPreview p = TrimEditorWindow::previewOf(r.f.view(), edit);
            QCOMPARE(p.left.clipId, r.a);
            QCOMPARE(p.left.timelineFrame, int64_t(99)); // the last frame of the outgoing clip
            QCOMPARE(p.right.clipId, r.b);
            QCOMPARE(p.right.timelineFrame, int64_t(100)); // the first frame of the incoming clip
            QCOMPARE(p.left.name, QStringLiteral("Out"));
        }
        // A clip whose neighbour does not touch has one side only.
        const TrimPreview gap = TrimEditorWindow::previewOf(r.f.view(), {r.b, 0});
        QCOMPARE(gap.left.clipId, r.b);
        QCOMPARE(gap.right.clipId, quint64(0));
        QVERIFY(!TrimEditorWindow::previewOf(r.f.view(), {}).isActive());
    }

    void buttonsTrimTheSelectedEdit()
    {
        Rig r;
        TrimEditorWindow w(r.f.ctx, &r.c);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.view()->isHidden()); // nothing selected
        QVERIFY(!w.findChild<QLabel*>(QStringLiteral("trimEditorHint"))->text().isEmpty());
        QVERIFY(!w.findChild<QPushButton*>(QStringLiteral("trim_roll_1"))->isEnabled());

        r.c.setSelectedEdit({r.a, r.b});
        QVERIFY(w.view()->preview().isActive());
        QVERIFY(w.findChild<QPushButton*>(QStringLiteral("trim_roll_1"))->isEnabled());
        QCOMPARE(w.view()->preview().left.clipId, r.a);

        // Roll +5: the cut moves right, the clips keep touching, one undo step.
        const int undo = r.f.project.undoStack()->count();
        w.findChild<QPushButton*>(QStringLiteral("trim_roll_5"))->click();
        QCOMPARE(r.clip(r.a)->duration, int64_t(105));
        QCOMPARE(r.clip(r.b)->start, int64_t(105));
        QCOMPARE(r.f.project.undoStack()->count(), undo + 1);
        // The pictures follow the new cut.
        QTRY_COMPARE(w.view()->preview().left.timelineFrame, int64_t(104));
        QCOMPARE(w.view()->preview().right.timelineFrame, int64_t(105));
        // Roll -10 the other way.
        w.findChild<QPushButton*>(QStringLiteral("trim_roll_-10"))->click();
        QCOMPARE(r.clip(r.a)->duration, int64_t(95));
        QCOMPARE(r.clip(r.b)->start, int64_t(95));

        // The outgoing buttons shorten only the outgoing clip (selection tool: a gap opens);
        // the window follows the selection it set.
        w.findChild<QPushButton*>(QStringLiteral("trim_out_-5"))->click();
        QCOMPARE(r.clip(r.a)->duration, int64_t(90));
        QCOMPARE(r.clip(r.b)->start, int64_t(95));
        QCOMPARE(r.c.selectedEdit(), (EditPointSelection {r.a, 0}));
        QVERIFY(!w.findChild<QPushButton*>(QStringLiteral("trim_in_1"))->isEnabled()); // a gap now

        // Undo takes the trim back.
        r.f.project.undoStack()->undo();
        QCOMPARE(r.clip(r.a)->duration, int64_t(95));
    }

    void incomingButtonsTrimTheHead()
    {
        Rig r;
        TrimEditorWindow w(r.f.ctx, &r.c);
        r.c.setSelectedEdit({0, r.b});
        QVERIFY(w.trim(TrimEditorWindow::Side::Incoming, 5));
        QCOMPARE(r.clip(r.b)->start, int64_t(105));
        QCOMPARE(r.clip(r.a)->duration, int64_t(100)); // the outgoing clip is untouched
        // No clip on that side, nothing selected: refused.
        r.c.setSelectedEdit({});
        QVERIFY(!w.trim(TrimEditorWindow::Side::Roll, 1));
        r.c.setSelectedEdit({r.d, 0});
        QVERIFY(!w.trim(TrimEditorWindow::Side::Roll, 1)); // no neighbour touches Far
    }

    void stepsToTheNextEdit()
    {
        Rig r;
        TrimEditorWindow w(r.f.ctx, &r.c);
        r.c.setSelectedEdit({r.a, r.b});
        QVERIFY(w.step(true));
        QVERIFY(!(r.c.selectedEdit() == (EditPointSelection {r.a, r.b})));
        QVERIFY(w.step(false)); // and back
        r.c.setSelectedEdit({});
        QVERIFY(!w.step(true));
    }

    void menuActionExists()
    {
        Rig r;
        QVERIFY(r.f.actions.contains(QStringLiteral("trim.trim_editor")));
        QAction* act = r.f.actions.action(QStringLiteral("trim.trim_editor"));
        QCOMPARE(act->text().remove(QLatin1Char('&')), QStringLiteral("Trim Editor…"));
    }
};

QTEST_MAIN(TestTrimEditor)
#include "test_trimeditor.moc"
