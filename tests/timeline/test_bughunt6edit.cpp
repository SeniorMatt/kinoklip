// SPDX-License-Identifier: GPL-3.0-or-later
// Sixth bug-hunting round, Edit page trimming and markers on the timeline controller and
// widget: one test per fix, named after the spec row it covers.
#include "ParityTestUtils.h"
#include "TimelineTestUtils.h"

#include <QAction>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
using namespace kk::test::parity;

class TestBughunt6Edit : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_bughunt6edit"); }

    // Spec 8.20: in the trim tool , and . slip the selected clip (the content moves under a
    // fixed window, as a one-frame drag of the lower part would); after a click in the name
    // band they slide it between its neighbours. The selection tool still moves the clip.
    void spec8_20_trimToolKeysSlipAndSlide()
    {
        View v;
        v.put(kV1, mediaClip(0, 20));
        const quint64 mid = v.put(kV1, mediaClip(20, 20));
        v.put(kV1, mediaClip(40, 20));
        v.c.setTool(TimelineTool::Trim);
        const QRectF r = v.clipRect(mid);
        // A click in the lower part selects the clip for a slip.
        v.click(QPoint(int(r.center().x()), int(r.bottom() - 5)));
        QVERIFY(v.f.selection.isSelected(mid));
        const int64_t src = ops::clipById(v.f.view(), mid)->sourceInUs;
        const int before = v.undoCount();
        v.action("trim.nudge_right")->trigger(); // .
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:20 20:20 40:20"));
        QCOMPARE(ops::clipById(v.f.view(), mid)->sourceInUs, src - 40000);
        QCOMPARE(v.undoCount(), before + 1);
        QCOMPARE(v.f.project.undoStack()->text(v.undoCount() - 1), QStringLiteral("Slip Clip"));
        v.action("trim.nudge_left_multi")->trigger(); // Shift+, : five frames back
        QCOMPARE(ops::clipById(v.f.view(), mid)->sourceInUs, src + 4 * 40000);
        // A click in the name band selects it for a slide.
        v.click(QPoint(int(r.center().x()), int(r.top() + 5)));
        v.action("trim.nudge_right")->trigger();
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:21 21:20 41:19"));
        QCOMPARE(v.f.project.undoStack()->text(v.undoCount() - 1), QStringLiteral("Slide Clip"));
        // The selection tool moves it (overwriting the neighbour), as before.
        v.c.setTool(TimelineTool::Select);
        v.action("trim.nudge_left")->trigger();
        QCOMPARE(layout(v.f.view(), kV1).section(QLatin1Char(' '), 1, 1), QStringLiteral("20:20"));
    }

    // Spec 8.21: dynamic trim (W) with a clip selected instead of an edit point: playback slides
    // the clip along with the playhead in the selection tool and slips it in the trim tool; one
    // undo step per run.
    void spec8_21_dynamicTrimSlipsAndSlidesClips()
    {
        Ctl t;
        bool playing = false;
        t.c.setPlayingProvider([&] { return playing; });
        t.put(kV1, mediaClip(0, 20));
        const quint64 mid = t.put(kV1, mediaClip(20, 20));
        t.put(kV1, mediaClip(40, 20));
        t.f.selection.setClips({mid});
        t.tl()->setPlayhead(30);
        t.action("trim.dynamic")->trigger();
        int before = t.undoCount();
        playing = true;
        for (int64_t f = 31; f <= 33; ++f)
            t.tl()->setPlayhead(f);
        playing = false;
        t.c.playbackStopped();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:23 23:20 43:17")); // slid by 3
        QCOMPARE(t.undoCount(), before + 1);
        t.f.project.undoStack()->undo();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:20 20:20 40:20"));

        // Trim tool: a slip (layout unchanged, the source moves like a drag to the right).
        t.c.setTool(TimelineTool::Trim);
        t.f.selection.setClips({mid});
        const int64_t src = ops::clipById(t.f.view(), mid)->sourceInUs;
        before = t.f.project.undoStack()->index(); // the undone step is replaced
        playing = true;
        for (int64_t f = 34; f <= 35; ++f)
            t.tl()->setPlayhead(f);
        playing = false;
        t.c.playbackStopped();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:20 20:20 40:20"));
        QCOMPARE(ops::clipById(t.f.view(), mid)->sourceInUs, src - 2 * 40000);
        QCOMPARE(t.f.project.undoStack()->index(), before + 1);
        // Without dynamic trim, playing just moves the playhead.
        t.action("trim.dynamic")->trigger();
        playing = true;
        t.tl()->setPlayhead(40);
        QCOMPARE(ops::clipById(t.f.view(), mid)->sourceInUs, src - 2 * 40000);
    }
};

QTEST_MAIN(TestBughunt6Edit)
#include "test_bughunt6edit.moc"
