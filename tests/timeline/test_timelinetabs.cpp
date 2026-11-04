// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of timeline tabs (switch, reorder, close, drop-down mode, per-timeline zoom) and of
// the compound clip commands through the registry actions (New Compound Clip, Open in
// Timeline, Decompose in Place), plus drops of timeline items from the media pool.
#include "../panels/PanelTestUtils.h"
#include "app/Theme.h"
#include "core/CompoundOps.h"
#include "core/EditBehaviour.h"
#include "ui/common/Resources.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineTabs.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTabBar>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;

class TestTimelineTabs : public QObject
{
    Q_OBJECT

    /// No modal dialogs in tests: suggested names, "yes" to questions, no message boxes.
    static void quiet(TimelinePanel& panel)
    {
        panel.controller()->setNamePrompt({});
        panel.controller()->setNewTimelinePrompt({});
        panel.controller()->setConfirmPrompt({});
        panel.controller()->setMessageSink({});
    }

    static void trigger(PanelFixture& f, const char* id)
    {
        f.actions.action(QLatin1String(id))->trigger();
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_timelinetabs"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void tabsFollowProject()
    {
        PanelFixture f;
        TimelinePanel panel(f.ctx);
        quiet(panel);
        panel.resize(1200, 400);
        panel.show();
        QTabBar* bar = panel.tabs()->tabBar();
        QVERIFY(bar->isVisible());
        QCOMPARE(bar->count(), 1);
        QCOMPARE(bar->tabText(0), QStringLiteral("Timeline 1"));

        // New Timeline (action) opens a second tab and activates it.
        trigger(f, "timeline.new");
        QCOMPARE(bar->count(), 2);
        QCOMPARE(bar->currentIndex(), 1);
        const quint64 second = f.project.activeTimelineUid();
        QCOMPARE(bar->tabText(1), QStringLiteral("Timeline 2"));

        // Clicking the first tab switches the project (timeline view follows).
        bar->setCurrentIndex(0);
        const quint64 first = f.project.activeTimelineUid();
        QVERIFY(first != second);
        QCOMPARE(f.project.timeline()->data().name, QStringLiteral("Timeline 1"));

        // Reorder by moving a tab (as a drag does): the project order follows.
        bar->moveTab(0, 1);
        QCOMPARE(f.project.tabs().last(), panel.tabs()->uidAt(1));
        QCOMPARE(panel.tabs()->uidAt(0), second);

        // Rename shows in the tab; close via the action; the last tab has no close button.
        QVERIFY(f.project.renameTimeline(second, QStringLiteral("Edit B")));
        QCOMPARE(bar->tabText(0), QStringLiteral("Edit B"));
        QVERIFY(bar->tabsClosable());
        trigger(f, "timeline.close_tab");
        QCOMPARE(bar->count(), 1);
        QVERIFY(!bar->tabsClosable());

        // Next / previous tab cycle through the open tabs.
        QVERIFY(f.project.openTab(first == f.project.activeTimelineUid() ? second : first));
        const quint64 now = f.project.activeTimelineUid();
        trigger(f, "timeline.next_tab");
        QVERIFY(f.project.activeTimelineUid() != now);
        trigger(f, "timeline.prev_tab");
        QCOMPARE(f.project.activeTimelineUid(), now);
    }

    void dropDownWhenTabsAreOff()
    {
        PanelFixture f;
        QSettings().setValue(QStringLiteral("edit/timelineTabs"), false);
        TimelinePanel panel(f.ctx);
        panel.show();
        QVERIFY(!panel.tabs()->tabsMode());
        QVERIFY(!panel.tabs()->tabBar()->isVisible());
        QComboBox* combo = panel.tabs()->comboBox();
        QVERIFY(combo->isVisible());
        const quint64 b = f.project.createTimeline(QStringLiteral("B"));
        QCOMPARE(combo->count(), 2);
        emit combo->activated(1);
        QCOMPARE(f.project.activeTimelineUid(), b);
        QSettings().remove(QStringLiteral("edit/timelineTabs"));
        panel.applyPreferences();
        QVERIFY(panel.tabs()->tabsMode());
    }

    void zoomAndPlayheadPerTimeline()
    {
        PanelFixture f;
        f.addClip(TrackRef {TrackType::Video, 0}, 0, 500);
        TimelinePanel panel(f.ctx);
        quiet(panel);
        panel.resize(1200, 400);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        TimelineWidget* view = panel.timelineWidget();
        view->setPixelsPerFrame(3.0, 200);
        f.project.timeline()->setPlayhead(40);
        const quint64 first = f.project.activeTimelineUid();
        trigger(f, "timeline.new");
        view->setPixelsPerFrame(0.5, 200);
        QVERIFY(f.project.setActiveTimeline(first));
        QCOMPARE(view->pixelsPerFrame(), 3.0);
        QCOMPARE(f.project.timeline()->playhead(), int64_t(40));
    }

    void compoundCommands()
    {
        PanelFixture f;
        TimelinePanel panel(f.ctx);
        quiet(panel);
        const quint64 a = f.addClip(TrackRef {TrackType::Video, 0}, 10, 20, ClipKind::Solid);
        const quint64 b = f.addClip(TrackRef {TrackType::Video, 1}, 15, 30, ClipKind::Solid);
        const int steps = f.project.undoStack()->count();
        f.selection.setClips({a, b});
        trigger(f, "clip.new_compound");
        QCOMPARE(f.project.undoStack()->count(), steps + 1);
        const TimelineData& d = f.project.timeline()->data();
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(1));
        const ClipData inst = d.videoTracks[0].clips[0];
        QCOMPARE(inst.kind, ClipKind::Nested);
        QCOMPARE(inst.name, QStringLiteral("Compound Clip 1"));
        QCOMPARE(inst.start, int64_t(10));
        QCOMPARE(inst.duration, int64_t(35));
        QCOMPARE(f.selection.clips(), QVector<quint64> {inst.id}); // the new clip is selected

        // Open in Timeline: a tab with the compound timeline; edits there update the
        // instance's source length (after switching back).
        trigger(f, "clip.open_in_timeline");
        QCOMPARE(f.project.activeTimelineUid(), inst.nestedUid);
        QCOMPARE(panel.tabs()->tabBar()->count(), 2);
        QVERIFY(f.project.timeline()->data().compound);
        f.project.beginEdit(QStringLiteral("Extend"));
        f.project.timeline()->mutableData().videoTracks[1].clips[0].duration += 10; // [5,45)
        f.project.endEdit();
        panel.tabs()->tabBar()->setCurrentIndex(0);
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips[0].sourceDurationUs,
            framesToUs(45, f.project.timeline()->data().fps));

        // Decompose in Place: the two solids are back on their tracks.
        f.selection.setClips({inst.id});
        trigger(f, "clip.decompose");
        const TimelineData& back = f.project.timeline()->data();
        QCOMPARE(back.videoTracks[0].clips.size(), size_t(1));
        QCOMPARE(back.videoTracks[0].clips[0].kind, ClipKind::Solid);
        QCOMPARE(back.videoTracks[0].clips[0].start, int64_t(10));
        QCOMPARE(back.videoTracks[0].clips[0].duration, int64_t(20));
        QCOMPARE(back.videoTracks[1].clips[0].start, int64_t(15));
        // The instance still shows 35 frames: the extended clip is cut at its end.
        QCOMPARE(back.videoTracks[1].clips[0].duration, int64_t(30));
        QCOMPARE(f.selection.clips().size(), 2);
    }

    void doubleClickOpensCompoundAtHoveredFrame()
    {
        PanelFixture f;
        TimelinePanel panel(f.ctx);
        quiet(panel);
        panel.resize(1400, 500);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        const quint64 a = f.addClip(TrackRef {TrackType::Video, 0}, 10, 40, ClipKind::Solid);
        f.selection.setClips({a});
        trigger(f, "clip.new_compound");
        const ClipData inst = f.project.timeline()->data().videoTracks[0].clips[0];
        QCOMPARE(inst.kind, ClipKind::Nested);
        const quint64 outer = f.project.activeTimelineUid();

        // Double-click 20 frames into the instance (timeline frame 30): the compound timeline
        // opens in a tab with its playhead at the matching inner frame.
        TimelineWidget* w = panel.timelineWidget();
        const TimelineLayout& layout = w->layout();
        const QRectF rect = layout.clipRect(layout.rowOf(TrackRef {TrackType::Video, 0}), inst);
        const QPoint at(qRound(layout.frameToX(30.5)), qRound(rect.center().y()));
        const int64_t expected = nestedFrameAt(inst, 30, f.project.timeline()->data().fps,
            f.project.timelineData(inst.nestedUid)->fps);
        QTest::mouseDClick(w->viewport(), Qt::LeftButton, {}, at);
        QCOMPARE(f.project.activeTimelineUid(), inst.nestedUid);
        QCOMPARE(f.project.timeline()->playhead(), expected);
        QCOMPARE(panel.tabs()->tabBar()->count(), 2);

        // Back in the outer timeline, a double-click on an empty track does not switch.
        QVERIFY(f.project.setActiveTimeline(outer));
        const QPoint empty(qRound(layout.frameToX(80.5)), at.y());
        QTest::mouseDClick(w->viewport(), Qt::LeftButton, {}, empty);
        QCOMPARE(f.project.activeTimelineUid(), outer);
    }

    void timelineItemDropsAsNestedClipWithoutRecursion()
    {
        PanelFixture f;
        TimelinePanel panel(f.ctx);
        const quint64 main = f.project.activeTimelineUid();
        const quint64 other = f.project.createTimeline(QStringLiteral("Other"));
        // Give "Other" content so it has a length.
        QVERIFY(f.project.setActiveTimeline(other));
        f.addClip(TrackRef {TrackType::Video, 0}, 0, 50, ClipKind::Solid);
        QVERIFY(f.project.setActiveTimeline(main));
        TimelineController* tc = panel.controller();
        const QString item = f.project.timelineItemId(other);
        const auto clips = tc->makeMediaClips(item, 0, 0, 0, 0);
        QCOMPARE(clips.size(), 1); // only video content
        QCOMPARE(clips[0].clip.kind, ClipKind::Nested);
        QCOMPARE(clips[0].clip.nestedUid, other);
        QCOMPARE(clips[0].clip.duration, int64_t(50));
        // Placing "Timeline 1" into itself, or into "Other" once Other contains it, is refused.
        QVERIFY(tc->makeMediaClips(f.project.timelineItemId(main), 0, 0, 0, 0).isEmpty());
        QVERIFY(tc->insertSource(item, 0, 0, SourceEditMode::Overwrite).size() == 1);
        QVERIFY(f.project.setActiveTimeline(other));
        QVERIFY(tc->makeMediaClips(f.project.timelineItemId(main), 0, 0, 0, 0).isEmpty());
    }
};

QTEST_MAIN(TestTimelineTabs)
#include "test_timelinetabs.moc"
