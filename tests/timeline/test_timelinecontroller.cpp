// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for TimelineController: each command is one undo step and produces the expected
// TimelineData; actions are wired; insertSource() places linked V+A on the targeted tracks.
#include "TimelineTestUtils.h"
#include "core/MediaPool.h"
#include "ui/timeline/TimelineController.h"

#include <QAction>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
namespace ops = kk::editops;

namespace {
constexpr TrackRef kV1 {TrackType::Video, 0};
constexpr TrackRef kV2 {TrackType::Video, 1};
constexpr TrackRef kA1 {TrackType::Audio, 0};

QString layout(const TimelineData& d, TrackRef ref)
{
    QStringList parts;
    for (const ClipData& c : d.track(ref)->clips)
        parts.append(QStringLiteral("%1:%2").arg(c.start).arg(c.duration));
    return parts.join(QLatin1Char(' '));
}

/// Adds a fake 10 s A/V media item (no file needed) and returns its id.
QString addMedia(Project& project, bool video = true, bool audio = true)
{
    MediaItem item;
    item.path = QStringLiteral("/nonexistent/clip.mp4");
    item.name = QStringLiteral("clip.mp4");
    item.info.valid = true;
    item.info.hasVideo = video;
    item.info.hasAudio = audio;
    item.info.durationUs = 10'000'000;
    item.info.width = 1920;
    item.info.height = 1080;
    return project.mediaPool()->addItem(item);
}

struct Ctl
{
    TimelineFixture f;
    TimelineController c {f.ctx};
    Ctl() { c.connectActions(); }
    QAction* action(const char* id) { return f.actions.action(QLatin1String(id)); }
    int undoCount() const { return f.project.undoStack()->count(); }
    /// Places clips without creating undo steps (test setup).
    quint64 put(TrackRef ref, ClipData clip)
    {
        f.project.beginEdit(QStringLiteral("setup"));
        const quint64 id = ops::overwriteClip(f.data(), ref, clip);
        f.project.endEdit();
        return id;
    }
};
} // namespace

class TestTimelineController : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_timelinecontroller"); }

    void vimStateIsHeldForThePainter()
    {
        Ctl t;
        // The clipboard can be replaced (the Vim registers paste through it).
        QVERIFY(t.c.clipboard().isEmpty());
        const quint64 a = t.put(kV1, solidClip(0, 40));
        t.f.selection.selectClip(a);
        t.c.copy();
        const ops::Clipboard board = t.c.clipboard();
        QVERIFY(!board.isEmpty());
        t.c.setClipboard({});
        QVERIFY(t.c.clipboard().isEmpty());
        t.c.setClipboard(board);
        QCOMPARE(t.c.clipboard().clips.size(), board.clips.size());
        // The Vim overlay (cursor track, VISUAL selection) is state: setting it repaints once.
        int changes = 0;
        connect(&t.c, &TimelineController::stateChanged, this, [&] { ++changes; });
        TimelineController::VimOverlay overlay;
        overlay.cursor = true;
        overlay.cursorTrack = kV2;
        overlay.visual = 3;
        overlay.frameFrom = 3;
        overlay.frameTo = 9;
        t.c.setVimOverlay(overlay);
        t.c.setVimOverlay(overlay); // unchanged: no signal
        QCOMPARE(changes, 1);
        QCOMPARE(t.c.vimOverlay().cursorTrack, kV2);
        QCOMPARE(t.c.vimOverlay().visual, 3);
        t.c.setVimOverlay({});
        QCOMPARE(changes, 2);
        QVERIFY(!t.c.vimOverlay().cursor);
    }

    void splitAtPlayheadAllOrSelected()
    {
        Ctl t;
        const quint64 a = t.put(kV1, solidClip(0, 100));
        t.put(kV2, solidClip(0, 100));
        t.f.project.timeline()->setPlayhead(40);
        t.f.selection.selectClip(a);
        const int before = t.undoCount();
        t.action("edit.split")->trigger();
        QCOMPARE(t.undoCount(), before + 1);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:40 40:60"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("0:100")); // not selected
        t.f.selection.clear();
        t.f.project.timeline()->setPlayhead(70);
        t.c.splitAtPlayhead();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:40 40:30 70:30"));
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("0:70 70:30"));
        t.f.project.undo();
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("0:100"));
    }

    void deleteAndRippleDeleteWithLinks()
    {
        Ctl t;
        t.put(kV1, solidClip(0, 10));
        const quint64 v = t.put(kV1, solidClip(10, 10));
        t.put(kV1, solidClip(20, 10));
        const quint64 a = t.put(kA1, solidClip(10, 10));
        t.put(kA1, solidClip(20, 10));
        t.f.project.beginEdit(QStringLiteral("link"));
        ops::linkClips(t.f.data(), {v, a});
        t.f.project.endEdit();

        t.f.selection.selectClip(v); // linked selection on: a goes too
        t.action("edit.ripple_delete")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("10:10"));
        QVERIFY(t.f.selection.clips().isEmpty());
        t.f.project.undo();

        t.c.setLinkedSelection(false);
        QVERIFY(!t.action("timeline.linked_selection")->isChecked());
        t.f.selection.selectClip(v);
        t.action("edit.delete")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 20:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("10:10 20:10"));
    }

    void rippleDeleteWithoutSelectionClosesGap()
    {
        // Opt-in convenience (EditBehaviour::rippleDeleteGapAtPlayhead, behavior-spec 10.5);
        // by default nothing happens without a selection.
        Ctl t;
        t.put(kV1, solidClip(0, 10));
        t.put(kV1, solidClip(30, 10));
        t.f.project.timeline()->setPlayhead(15);
        t.c.rippleDeleteSelection();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 30:10"));
        QSettings().setValue(QStringLiteral("edit/rippleDeleteGapAtPlayhead"), true);
        t.c.rippleDeleteSelection();
        QSettings().remove(QStringLiteral("edit/rippleDeleteGapAtPlayhead"));
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:10 10:10"));
    }

    void nudgeAndToggleEnabled()
    {
        Ctl t;
        const quint64 id = t.put(kV1, solidClip(10, 10));
        t.f.selection.selectClip(id);
        t.action("trim.nudge_right")->trigger();
        t.action("trim.nudge_right")->trigger();
        t.action("trim.nudge_left")->trigger();
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("11:10"));
        t.action("clip.toggle_enabled")->trigger();
        QVERIFY(!ops::clipById(t.f.view(), id)->enabled);
        t.action("clip.toggle_enabled")->trigger();
        QVERIFY(ops::clipById(t.f.view(), id)->enabled);
    }

    void marksAndMarkers()
    {
        Ctl t;
        const quint64 id = t.put(kV1, solidClip(20, 30));
        Timeline* tl = t.f.project.timeline();
        tl->setPlayhead(5);
        t.action("mark.in")->trigger();
        tl->setPlayhead(9);
        t.action("mark.out")->trigger();
        QCOMPARE(t.f.view().markIn, int64_t(5));
        QCOMPARE(t.f.view().markOut, int64_t(10)); // exclusive: frame 9 included
        t.action("mark.go_to_in")->trigger();
        QCOMPARE(tl->playhead(), int64_t(5));
        t.action("mark.go_to_out")->trigger();
        QCOMPARE(tl->playhead(), int64_t(9));
        // Marking in after the out drops the out.
        tl->setPlayhead(50);
        t.action("mark.in")->trigger();
        QCOMPARE(t.f.view().markOut, int64_t(-1));
        t.action("mark.clear_both")->trigger();
        QCOMPARE(t.f.view().markIn, int64_t(-1));
        t.f.selection.selectClip(id);
        t.action("mark.clip")->trigger();
        QCOMPARE(t.f.view().markIn, int64_t(20));
        QCOMPARE(t.f.view().markOut, int64_t(50));
        t.action("mark.clear_in")->trigger();
        t.action("mark.clear_out")->trigger();
        QCOMPARE(t.f.view().markOut, int64_t(-1));

        t.f.selection.clear(); // with the clip selected, M would add a clip marker (spec 18.4)
        tl->setPlayhead(30);
        t.action("mark.add_marker")->trigger();
        tl->setPlayhead(12);
        t.action("mark.add_marker")->trigger();
        QCOMPARE(t.f.view().markers.size(), 2);
        QCOMPARE(t.f.view().markers[0].frame, int64_t(12)); // kept sorted
        const int steps = t.undoCount();
        t.action("mark.add_marker")->trigger(); // same frame: M toggles it off (spec 18.16)
        QCOMPARE(t.undoCount(), steps + 1);
        QCOMPARE(t.f.view().markers.size(), 1);
        QCOMPARE(t.f.view().markers[0].frame, int64_t(30));
        t.action("mark.add_marker")->trigger(); // and on again
        QCOMPARE(t.f.view().markers.size(), 2);
    }

    void navigation()
    {
        Ctl t;
        t.put(kV1, solidClip(10, 10));
        t.put(kV2, solidClip(15, 20));
        Timeline* tl = t.f.project.timeline();
        tl->setPlayhead(0);
        t.action("transport.next_edit")->trigger();
        QCOMPARE(tl->playhead(), int64_t(10));
        t.action("transport.next_edit")->trigger();
        QCOMPARE(tl->playhead(), int64_t(15));
        t.action("transport.prev_edit")->trigger();
        QCOMPARE(tl->playhead(), int64_t(10));
        t.action("transport.go_to_end")->trigger();
        QCOMPARE(tl->playhead(), int64_t(35));
        t.action("transport.go_to_start")->trigger();
        QCOMPARE(tl->playhead(), int64_t(0));
    }

    void copyCutPaste()
    {
        Ctl t;
        const quint64 id = t.put(kV2, solidClip(0, 10));
        t.f.selection.selectClip(id);
        t.action("edit.copy")->trigger();
        t.f.project.timeline()->setPlayhead(50);
        t.action("edit.paste")->trigger();
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("0:10 50:10"));
        QCOMPARE(t.f.project.timeline()->playhead(), int64_t(60));
        QVERIFY(t.f.selection.currentClip() != id); // the pasted clip is selected
        t.f.selection.selectClip(id);
        t.action("edit.cut")->trigger();
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("50:10"));
        t.f.project.timeline()->setPlayhead(0);
        t.c.paste();
        QCOMPARE(layout(t.f.view(), kV2), QStringLiteral("0:10 50:10"));
    }

    void selectAllAndDeselect()
    {
        Ctl t;
        t.put(kV1, solidClip(0, 10));
        t.put(kA1, solidClip(0, 10));
        t.action("edit.select_all")->trigger();
        QCOMPARE(t.f.selection.clips().size(), 2);
        t.action("edit.deselect_all")->trigger();
        QVERIFY(t.f.selection.clips().isEmpty());
    }

    void defaultTransitionAtNearestEdit()
    {
        Ctl t;
        const quint64 a = t.put(kV1, solidClip(0, 50));
        const quint64 b = t.put(kV1, solidClip(50, 50));
        t.f.project.timeline()->setPlayhead(47);
        t.action("timeline.add_transition")->trigger();
        // The nearest edge is a's tail, which touches b: stored as b's transitionIn.
        QVERIFY(!ops::clipById(t.f.view(), a)->transitionOut);
        const auto& tin = ops::clipById(t.f.view(), b)->transitionIn;
        QVERIFY(tin);
        QCOMPARE(tin->type, QStringLiteral("cross_dissolve"));
        QCOMPARE(tin->duration, int64_t(25)); // 1 s at 25 fps
        t.f.project.timeline()->setPlayhead(99);
        t.c.addDefaultTransition();
        QVERIFY(ops::clipById(t.f.view(), b)->transitionOut);
    }

    void insertSourceOnTargets()
    {
        Ctl t;
        const QString media = addMedia(t.f.project);
        t.put(kV1, solidClip(0, 100));
        t.f.project.timeline()->setPlayhead(40);
        // 2 s from 1 s: 50 frames, linked V + A.
        QVector<quint64> ids
            = t.c.insertSource(media, 1'000'000, 3'000'000, SourceEditMode::Overwrite);
        QCOMPARE(ids.size(), 2);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:40 40:50 90:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("40:50"));
        const ClipData* v = ops::clipById(t.f.view(), ids[0]);
        QCOMPARE(v->sourceInUs, int64_t(1'000'000));
        QVERIFY(v->linkGroup != 0);
        QCOMPARE(v->linkGroup, ops::clipById(t.f.view(), ids[1])->linkGroup);
        QCOMPARE(t.f.project.timeline()->playhead(), int64_t(90));

        // Insert ripples; untargeted audio stays out.
        t.c.setTargetTrack(TrackType::Audio, -1);
        t.f.project.timeline()->setPlayhead(0);
        ids = t.c.insertSource(media, 0, 0, SourceEditMode::Insert); // whole 10 s = 250 frames
        QCOMPARE(ids.size(), 1);
        QCOMPARE(layout(t.f.view(), kV1), QStringLiteral("0:250 250:40 290:50 340:10"));
        QCOMPARE(layout(t.f.view(), kA1), QStringLiteral("290:50"));

        // Place on top: V1 is busy, V2 free.
        t.f.project.timeline()->setPlayhead(10);
        ids = t.c.insertSource(media, 0, 1'000'000, SourceEditMode::PlaceOnTop);
        QCOMPARE(ops::findClip(t.f.view(), ids[0])->track, kV2);

        // Through the source provider and the F9 action.
        t.c.setSourceProvider(
            [media] { return std::optional<SourceSelection>({media, 0, 400'000}); });
        t.f.project.timeline()->setPlayhead(0);
        t.action("edit.insert")->trigger();
        QCOMPARE(t.f.view().videoTracks[0].clips.front().duration, int64_t(10));
        KK_VERIFY_TIMELINE(t.f.view());
    }

    void stillsAndGenerators()
    {
        Ctl t;
        MediaItem still;
        still.path = QStringLiteral("/nonexistent/still.png");
        still.info.valid = true;
        still.info.hasVideo = true;
        still.info.isStillImage = true;
        const QString id = t.f.project.mediaPool()->addItem(still);
        const QVector<quint64> ids = t.c.insertSource(id, 0, 0, SourceEditMode::Overwrite);
        QCOMPARE(ids.size(), 1);
        QCOMPARE(ops::clipById(t.f.view(), ids[0])->duration, int64_t(125));
        QCOMPARE(ops::clipById(t.f.view(), ids[0])->sourceDurationUs, int64_t(0));

        const auto title
            = TimelineController::makeGeneratorClip(QStringLiteral("text_title"), Rational {25, 1});
        QVERIFY(title);
        QCOMPARE(title->kind, ClipKind::Title);
        QCOMPARE(title->duration, int64_t(125));
        const auto solid = TimelineController::makeGeneratorClip(
            QStringLiteral("solid_color"), Rational {25, 1});
        QCOMPARE(solid->kind, ClipKind::Solid);
        QVERIFY(solid->solidColor.isValid());
        QVERIFY(!TimelineController::makeGeneratorClip(
            QStringLiteral("gaussian_blur"), Rational {25, 1}));
    }

    void toolsAndTracks()
    {
        Ctl t;
        t.action("tool.blade")->trigger();
        QCOMPARE(t.c.tool(), TimelineTool::Blade);
        t.c.setTool(TimelineTool::Trim);
        QVERIFY(t.action("tool.trim")->isChecked());
        t.action("timeline.snapping")->trigger();
        QVERIFY(!t.c.snapping());

        t.action("timeline.add_video_track")->trigger();
        QCOMPARE(t.f.view().videoTracks.size(), size_t(3));
        t.c.renameTrack(TrackRef {TrackType::Video, 2}, QStringLiteral("Titles"));
        QCOMPARE(t.f.view().videoTracks[2].name, QStringLiteral("Titles"));
        t.c.toggleTrackFlag(kA1, TimelineController::FlagMuted);
        QVERIFY(t.f.view().audioTracks[0].muted);
        t.c.setTargetTrack(TrackType::Video, 2);
        t.c.removeTrack(TrackRef {TrackType::Video, 2});
        QCOMPARE(t.f.view().videoTracks.size(), size_t(2));
        QCOMPARE(t.c.targetVideoTrack(), 1); // re-clamped
        t.c.toggleTarget(TrackRef {TrackType::Video, 1});
        QCOMPARE(t.c.targetVideoTrack(), -1);
    }
};

QTEST_MAIN(TestTimelineController)
#include "test_timelinecontroller.moc"
