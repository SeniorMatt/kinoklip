// SPDX-License-Identifier: GPL-3.0-or-later
// Multicam UI: creating a multicam clip from the media pool (sync by in points), Sync and
// Place on Timeline, the source viewer's multicam mode (grid following the timeline clip,
// pages, the loaded clip), angle clicks and number keys cutting / switching through the
// TimelineController, and the controller's flatten command.
#include "PanelTestUtils.h"
#include "core/MediaPool.h"
#include "core/Multicam.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/timeline/TimelineController.h"
#include "ui/viewer/MulticamOverlay.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QTest>

using namespace kk;
using namespace kk::test;

namespace {

/// A fake (unprobed, offline) clip of @p seconds with video and audio.
QString addItem(Project& project, const QString& name, double seconds)
{
    MediaItem item;
    item.path = QStringLiteral("/nonexistent/") + name;
    item.name = name;
    item.info.valid = item.info.hasVideo = item.info.hasAudio = true;
    item.info.width = item.info.displayWidth = 1280;
    item.info.height = item.info.displayHeight = 720;
    item.info.frameRate = {25, 1};
    item.info.durationUs = int64_t(seconds * 1e6);
    project.beginEdit(QStringLiteral("add"));
    const QString id = project.mediaPool()->addItem(item);
    project.endEdit();
    return id;
}

/// Places multicam clip @p uid on V1 + A1 at frame 0 (linked); returns the video part's id.
quint64 place(Project& project, quint64 uid)
{
    const auto mc = project.timelineData(uid);
    const ClipData clip = multicam::makeMulticamClip(*mc, project.timelineItemId(uid), 0);
    project.beginEdit(QStringLiteral("Place"));
    TimelineData& d = project.timeline()->mutableData();
    const QVector<quint64> ids = editops::overwriteClips(
        d, {{TrackRef {TrackType::Video, 0}, clip}, {TrackRef {TrackType::Audio, 0}, clip}});
    editops::linkClips(d, ids);
    project.endEdit();
    return ids.first();
}

} // namespace

class TestMulticamUi : public QObject
{
    Q_OBJECT

private slots:
    void createFromMediaPool()
    {
        PanelFixture f;
        MediaPoolPanel pool(f.ctx);
        const QString a = addItem(f.project, QStringLiteral("CamA.mov"), 10);
        const QString b = addItem(f.project, QStringLiteral("CamB.mov"), 8);
        const QString c = addItem(f.project, QStringLiteral("CamC.mov"), 6);
        // In points: A at 2 s, B at 0.5 s, C none: A's 2 s, B's 0.5 s and C's 0 s line up,
        // so A starts first, B 1.5 s and C 2 s later.
        pool.setInPointProvider([&](const QString& id) -> int64_t {
            return id == a ? 2000000 : id == b ? 500000 : 0;
        });
        QCOMPARE(pool.syncableIds({a, b, c, QStringLiteral("missing")}).size(), 3);
        const quint64 uid = pool.createMulticamClip(
            {a, b, c}, QStringLiteral("Show"), multicam::SyncMethod::InPoints, true, false);
        QVERIFY(uid);
        const auto mc = f.project.timelineData(uid);
        QCOMPARE(mc->name, QStringLiteral("Show"));
        QCOMPARE(mc->videoTracks[0].name, QStringLiteral("CamA"));
        QCOMPARE(mc->videoTracks[0].clips[0].start, int64_t(0)); // A starts first (-2 s)
        QCOMPARE(mc->videoTracks[1].clips[0].start, int64_t(38)); // 1.5 s later, rounded up
        QCOMPARE(mc->videoTracks[2].clips[0].start, int64_t(50));
        QCOMPARE(mc->width, 1280);
        QCOMPARE(f.project.mediaPool()->item(f.project.timelineItemId(uid))->kind,
            MediaItemKind::MulticamClip);
        // Angle numbers instead of names, one undo step.
        const quint64 second
            = pool.createMulticamClip({a, b}, QString(), multicam::SyncMethod::Start, false, false);
        QCOMPARE(f.project.timelineData(second)->videoTracks[1].name, QStringLiteral("Angle 2"));
        f.project.undo();
        QVERIFY(!f.project.hasTimeline(second));
    }

    void syncAndPlace()
    {
        PanelFixture f;
        MediaPoolPanel pool(f.ctx);
        const QString a = addItem(f.project, QStringLiteral("A.mov"), 4);
        const QString b = addItem(f.project, QStringLiteral("B.mov"), 4);
        f.project.timeline()->setPlayhead(10);
        pool.setInPointProvider([&](const QString& id) { return id == b ? int64_t(1000000) : 0; });
        QCOMPARE(pool.syncAndPlaceOnTimeline({a, b}, multicam::SyncMethod::InPoints), 4);
        const TimelineData& d = f.project.timeline()->data();
        QCOMPARE(d.videoTracks.size(), size_t(4)); // V1, V2 + two new
        QCOMPARE(d.videoTracks[3].clips[0].start, int64_t(10)); // B (in point 1 s) first
        QCOMPARE(d.videoTracks[2].clips[0].start, int64_t(35));
        f.project.undo();
        QCOMPARE(f.project.timeline()->data().videoTracks.size(), size_t(2));
    }

    void multicamViewerFollowsTimelineAndCuts()
    {
        PanelFixture f;
        MediaPoolPanel pool(f.ctx);
        QStringList ids;
        for (const char* name : {"1.mov", "2.mov", "3.mov", "4.mov", "5.mov"})
            ids.append(addItem(f.project, QLatin1String(name), 10));
        const quint64 uid = pool.createMulticamClip(
            ids, QStringLiteral("Five"), multicam::SyncMethod::Start, true, false);
        QVERIFY(uid);
        const quint64 video = place(f.project, uid);
        TimelineController controller(f.ctx);
        ViewerPanel viewer(ViewerPanel::Mode::Source, f.ctx);
        connect(&viewer, &ViewerPanel::multicamAngleChosen, &controller,
            &TimelineController::chooseMulticamAngle);
        viewer.resize(640, 400);
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));

        // Off: nothing. View > Multicam Viewer: the grid of the clip under the playhead.
        QCOMPARE(viewer.multicamUid(), quint64(0));
        f.actions.action(QStringLiteral("view.multicam_viewer"))->setChecked(true);
        QVERIFY(viewer.multicamMode());
        QCOMPARE(viewer.multicamUid(), uid);
        QVERIFY(viewer.multicamFollowsTimeline());
        MulticamOverlay* overlay = viewer.multicamOverlay();
        QVERIFY(overlay->isVisible());
        QCOMPARE(overlay->columns(), 3); // five angles -> 3x3
        QCOMPARE(overlay->pageCount(), 1);
        QVERIFY(!overlay->cellRect(4).isEmpty());
        // The source player shows the grid: one track per angle on the page.
        QCOMPARE(viewer.sourcePlayer()->timeline()->data().videoTracks.size(), size_t(5));
        viewer.setMulticamColumns(2);
        QCOMPARE(overlay->pageCount(), 2);
        QCOMPARE(viewer.sourcePlayer()->timeline()->data().videoTracks.size(), size_t(4));
        viewer.setMulticamPage(1);
        QCOMPARE(overlay->page(), 1);
        QCOMPARE(viewer.sourcePlayer()->timeline()->data().videoTracks.size(), size_t(1));
        viewer.setMulticamPage(0);

        // Click on angle 2 at frame 60 (cut mode, both): cut at 60.
        f.project.timeline()->setPlayhead(60);
        const QPoint centre = overlay->cellRect(1).center();
        QTest::mouseClick(overlay, Qt::LeftButton, {}, centre);
        const TimelineData& d = f.project.timeline()->data();
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(2));
        QCOMPARE(d.videoTracks[0].clips[1].start, int64_t(60));
        QCOMPARE(d.videoTracks[0].clips[1].multicamAngle, 1);
        QCOMPARE(d.audioTracks[0].clips[1].multicamAngle, 1);

        // Number key 3 at frame 100 (video only): cut, only the video switches.
        f.actions.action(QStringLiteral("multicam.target_video"))->setChecked(true);
        f.project.timeline()->setPlayhead(100);
        f.actions.action(QStringLiteral("multicam.angle_3"))->trigger();
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(3));
        QCOMPARE(d.videoTracks[0].clips[2].multicamAngle, 2);
        QCOMPARE(d.audioTracks[0].clips[2].multicamAngle, 1);

        // Switch mode: the segment under the playhead changes, no cut.
        f.actions.action(QStringLiteral("multicam.mode_switch"))->setChecked(true);
        f.actions.action(QStringLiteral("multicam.target_both"))->setChecked(true);
        f.project.timeline()->setPlayhead(20);
        f.actions.action(QStringLiteral("multicam.angle_4"))->trigger();
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(3));
        QCOMPARE(d.videoTracks[0].clips[0].multicamAngle, 3);
        QCOMPARE(d.audioTracks[0].clips[0].multicamAngle, 3);
        f.project.undo();
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips[0].multicamAngle, 0);

        // Outside the clip: the message, no grid.
        f.project.timeline()->setPlayhead(100000);
        QCOMPARE(viewer.multicamUid(), quint64(0));
        // Opening the multicam clip from the media pool shows its own grid.
        viewer.setSourceMedia(f.project.timelineItemId(uid));
        QCOMPARE(viewer.multicamUid(), uid);
        QVERIFY(!viewer.multicamFollowsTimeline());
        // Off again: the overlay goes away.
        f.actions.action(QStringLiteral("view.multicam_viewer"))->setChecked(false);
        QVERIFY(!overlay->isVisible());
        QVERIFY(!viewer.multicamMode());

        // Flatten the first segment through the controller.
        f.project.timeline()->setPlayhead(10);
        f.selection.setClips({video});
        QVERIFY(controller.flattenMulticamSelection());
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips[0].kind, ClipKind::Media);
    }
};

QTEST_MAIN(TestMulticamUi)
#include "test_multicamui.moc"
