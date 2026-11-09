// SPDX-License-Identifier: GPL-3.0-or-later
// Editing aids of the polish-v3 round (docs/archive/polish-v3.md): the drag offset label (spec 21.9),
// the trim preview of the two-up view (spec 8.15), typed timecodes (spec 2.10) and the
// edits of the viewer's edit overlay (spec 4.14), driven through the widget and controller.
#include "TimelineTestUtils.h"
#include "core/MediaPool.h"
#include "ui/common/TimecodeEntry.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QKeyEvent>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;
namespace ops = kk::editops;

namespace {

constexpr TrackRef kV1 {TrackType::Video, 0};
constexpr TrackRef kA1 {TrackType::Audio, 0};
constexpr Rational kFps25 {25, 1};

QString layout(const TimelineData& d, TrackRef ref)
{
    QStringList parts;
    for (const ClipData& c : d.track(ref)->clips)
        parts.append(QStringLiteral("%1:%2").arg(c.start).arg(c.duration));
    return parts.join(QLatin1Char(' '));
}

/// Adds a fake 10 s A/V media item (no file needed) and returns its id.
QString addMedia(Project& project, const QString& name = QStringLiteral("clip.mp4"))
{
    MediaItem item;
    item.path = QStringLiteral("/nonexistent/") + name;
    item.name = name;
    item.info.valid = true;
    item.info.hasVideo = true;
    item.info.hasAudio = true;
    item.info.durationUs = 10'000'000;
    item.info.width = 1920;
    item.info.height = 1080;
    return project.mediaPool()->addItem(item);
}

/// A media clip of @p mediaId showing source frame @p sourceFrame at its start.
ClipData mediaClip(const QString& mediaId, int64_t start, int64_t duration, int64_t sourceFrame)
{
    ClipData c;
    c.kind = ClipKind::Media;
    c.mediaId = mediaId;
    c.name = QStringLiteral("m");
    c.start = start;
    c.duration = duration;
    c.sourceInUs = sourceFrame * 40'000;
    c.sourceDurationUs = 10'000'000;
    return c;
}

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
    QPoint at(double frame, TrackRef ref)
    {
        const TimelineLayout& L = w.layout();
        return QPoint(int(std::lround(L.frameToX(frame))), L.rowRect(L.rowOf(ref)).center().y());
    }
    quint64 put(TrackRef ref, ClipData clip)
    {
        f.project.beginEdit(QStringLiteral("setup"));
        const quint64 id = ops::overwriteClip(f.data(), ref, clip);
        f.project.endEdit();
        return id;
    }
    /// Press at @p from and move to @p to in steps, without releasing.
    void press(QPoint from, QPoint to)
    {
        QTest::mousePress(vp(), Qt::LeftButton, {}, from);
        for (int i = 1; i <= 4; ++i)
            QTest::mouseMove(vp(), from + (to - from) * i / 4);
    }
    void release(QPoint at) { QTest::mouseRelease(vp(), Qt::LeftButton, {}, at); }
    int undoCount() const { return f.project.undoStack()->count(); }
};

/// Sends a key press (with its shortcut override first, like Qt does) to @p target.
void typeKey(QWidget* target, int key, const QString& text = {}, Qt::KeyboardModifiers mods = {})
{
    QKeyEvent override(QEvent::ShortcutOverride, key, mods, text);
    QCoreApplication::sendEvent(target, &override);
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QCoreApplication::sendEvent(target, &press);
}

} // namespace

class TestEditAids : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { initTimelineTestCase("test_editaids"); }

    // ---- spec 21.9: the offset label while dragging ----

    void spec21_9_moveShowsOffsetLabel()
    {
        View v;
        v.put(kV1, solidClip(10, 20));
        v.press(v.at(20, kV1), v.at(45, kV1));
        const DragInfoLabel label = v.w.dragInfo();
        QCOMPARE(label.lines.size(), 2);
        QCOMPARE(label.lines[0], QStringLiteral("+00:00:01:00")); // 25 frames at 25 fps
        QVERIFY(label.lines[1].contains(QStringLiteral("00:00:01:10"))); // new start 35
        v.release(v.at(45, kV1));
        QVERIFY(v.w.dragInfo().isEmpty());
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("35:20"));
    }

    void spec21_9_trimShowsOffsetAndDuration()
    {
        View v;
        v.put(kV1, solidClip(10, 20));
        const QPoint tail = v.at(30, kV1) - QPoint(2, 0);
        v.press(tail, tail - QPoint(50, 0)); // -5 frames
        const DragInfoLabel label = v.w.dragInfo();
        QCOMPARE(label.lines.value(0), QStringLiteral("-00:00:00:05"));
        QVERIFY(label.lines.value(1).contains(QStringLiteral("00:00:00:15")));
        v.release(tail - QPoint(50, 0));
        QVERIFY(v.w.dragInfo().isEmpty());
    }

    void spec21_9_formatOffset()
    {
        QCOMPARE(draginfo::formatOffset(0, kFps25), QStringLiteral("+00:00:00:00"));
        QCOMPARE(draginfo::formatOffset(-26, kFps25), QStringLiteral("-00:00:01:01"));
        QCOMPARE(draginfo::formatOffset(25 * 61, kFps25), QStringLiteral("+00:01:01:00"));
    }

    // ---- spec 8.15: trim preview frames ----

    void spec8_15_rollPreviewShowsBothSidesOfTheCut()
    {
        View v;
        const QString media = addMedia(v.f.project);
        const quint64 left = v.put(kV1, mediaClip(media, 10, 20, 100));
        const quint64 right = v.put(kV1, mediaClip(media, 30, 20, 200));
        v.c.setTool(TimelineTool::Trim);
        QSignalSpy spy(&v.w, &TimelineWidget::trimPreviewChanged);
        const QPoint cut = v.at(30, kV1) + QPoint(2, 0);
        v.press(cut, cut + QPoint(50, 0)); // roll +5
        const TrimPreview p = v.w.trimPreview();
        QVERIFY(p.isActive());
        QCOMPARE(p.left.clipId, left);
        QCOMPARE(p.right.clipId, right);
        QCOMPARE(p.left.mediaId, media);
        QCOMPARE(p.left.timelineFrame, int64_t(34)); // new last frame of the left clip
        QCOMPARE(p.right.timelineFrame, int64_t(35)); // new first frame of the right clip
        // Left: source frame 100 + 24; right: 200 + 5 (its head moved 5 frames later).
        QCOMPARE(p.left.sourceUs, int64_t(124 * 40'000));
        QCOMPARE(p.right.sourceUs, int64_t(205 * 40'000));
        QVERIFY(spy.count() >= 1);
        v.release(cut + QPoint(50, 0));
        QVERIFY(!v.w.trimPreview().isActive());
        QVERIFY(!spy.last().at(0).value<TrimPreview>().isActive());
    }

    void spec8_15_tailTrimShowsNextClipOrNothing()
    {
        View v;
        const QString media = addMedia(v.f.project);
        const quint64 only = v.put(kV1, mediaClip(media, 10, 20, 0));
        const QPoint tail = v.at(30, kV1) - QPoint(2, 0);
        v.press(tail, tail - QPoint(30, 0));
        const TrimPreview p = v.w.trimPreview();
        QCOMPARE(p.left.clipId, only);
        QCOMPARE(p.left.timelineFrame, int64_t(26));
        QCOMPARE(p.right.clipId, quint64(0)); // nothing after it: black
        v.release(tail - QPoint(30, 0));
    }

    void spec8_15_moveHasNoTrimPreview()
    {
        View v;
        v.put(kV1, solidClip(10, 20));
        v.press(v.at(20, kV1), v.at(40, kV1));
        QVERIFY(!v.w.trimPreview().isActive());
        v.release(v.at(40, kV1));
    }

    // ---- spec 2.10: typed timecodes ----

    void spec2_10_parseTyped()
    {
        using timecode::parseTyped;
        QCOMPARE(parseTyped(QStringLiteral("12"), kFps25)->frames, int64_t(12));
        QCOMPARE(parseTyped(QStringLiteral("1000"), kFps25)->frames, int64_t(250));
        QCOMPARE(parseTyped(QStringLiteral("1.."), kFps25)->frames, int64_t(25 * 60));
        QCOMPARE(parseTyped(QStringLiteral("1:00:00:00"), kFps25)->frames, int64_t(25 * 3600));
        const auto plus = parseTyped(QStringLiteral("+90"), kFps25);
        QVERIFY(plus->relative);
        QCOMPARE(plus->frames, int64_t(90));
        const auto minus = parseTyped(QStringLiteral("-1."), kFps25);
        QVERIFY(minus->relative);
        QCOMPARE(minus->frames, int64_t(-25));
        QVERIFY(!parseTyped(QStringLiteral("+"), kFps25));
        QVERIFY(!parseTyped(QStringLiteral("12a"), kFps25));
        QCOMPARE(timecode::typedDisplay(QStringLiteral("1215")), QStringLiteral("00:00:12:15"));
        QCOMPARE(timecode::typedDisplay(QStringLiteral("-5")), QStringLiteral("-00:00:00:05"));
    }

    void spec2_10_absoluteMovesPlayhead()
    {
        View v;
        v.c.enterTimecode({false, 250});
        QCOMPARE(v.f.project.timeline()->playhead(), int64_t(250));
        v.c.enterTimecode({true, -300}); // clamped at the start
        QCOMPARE(v.f.project.timeline()->playhead(), int64_t(0));
        v.c.enterTimecode({true, 40});
        QCOMPARE(v.f.project.timeline()->playhead(), int64_t(40));
    }

    void spec2_10_offsetMovesSelectedClips()
    {
        View v;
        const quint64 id = v.put(kV1, solidClip(10, 20));
        v.f.selection.setClips({id});
        const int before = v.undoCount();
        v.c.enterTimecode({true, 25});
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("35:20"));
        QCOMPARE(v.undoCount(), before + 1);
        QCOMPARE(v.f.project.timeline()->playhead(), int64_t(0)); // the playhead stays
        v.c.enterTimecode({false, 100}); // absolute: playhead, clips stay
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("35:20"));
        QCOMPARE(v.f.project.timeline()->playhead(), int64_t(100));
    }

    void spec2_10_typingDigitsAndEnter()
    {
        View v;
        bool accepting = true;
        TimecodeEntry entry([&] { return accepting; }, [] { return kFps25; });
        QSignalSpy shown(&entry, &TimecodeEntry::displayChanged);
        QSignalSpy entered(&entry, &TimecodeEntry::entered);
        typeKey(&v.w, Qt::Key_1, QStringLiteral("1"));
        typeKey(&v.w, Qt::Key_0, QStringLiteral("0"));
        typeKey(&v.w, Qt::Key_Period, QStringLiteral("."));
        QVERIFY(entry.isActive());
        QCOMPARE(shown.last().at(0).toString(), QStringLiteral("00:00:10:00"));
        typeKey(&v.w, Qt::Key_Return);
        QCOMPARE(entered.count(), 1);
        QCOMPARE(entered.last().at(0).value<TypedTimecode>().frames, int64_t(250));
        QVERIFY(!entry.isActive());
        QCOMPARE(shown.last().at(0).toString(), QString());

        // '+' starts an offset; Esc cancels; '.' alone is not taken (the nudge key).
        typeKey(&v.w, Qt::Key_Plus, QStringLiteral("+"), Qt::ShiftModifier);
        typeKey(&v.w, Qt::Key_5, QStringLiteral("5"));
        QCOMPARE(shown.last().at(0).toString(), QStringLiteral("+00:00:00:05"));
        typeKey(&v.w, Qt::Key_Escape);
        QVERIFY(!entry.isActive());
        QCOMPARE(entered.count(), 1);
        typeKey(&v.w, Qt::Key_Period, QStringLiteral("."));
        QVERIFY(!entry.isActive());

        // Another key ends the entry; not accepting: nothing is taken.
        typeKey(&v.w, Qt::Key_3, QStringLiteral("3"));
        typeKey(&v.w, Qt::Key_Space, QStringLiteral(" "));
        QVERIFY(!entry.isActive());
        accepting = false;
        typeKey(&v.w, Qt::Key_3, QStringLiteral("3"));
        QVERIFY(!entry.isActive());
    }

    void spec2_10_digitsDoNotFireNumberShortcuts()
    {
        View v;
        TimecodeEntry entry([] { return true; }, [] { return kFps25; });
        QKeyEvent override(
            QEvent::ShortcutOverride, Qt::Key_1, Qt::NoModifier, QStringLiteral("1"));
        QCoreApplication::sendEvent(&v.w, &override);
        QVERIFY(override.isAccepted()); // accepted override: the "1" shortcut does not fire
    }

    // ---- spec 4.14: edits of the viewer's edit overlay ----

    void spec4_14_actionIdsMapToEdits()
    {
        QCOMPARE(*TimelineController::sourceEditModeFor(QStringLiteral("edit.insert")),
            SourceEditMode::Insert);
        QCOMPARE(*TimelineController::sourceEditModeFor(QStringLiteral("edit.ripple_overwrite")),
            SourceEditMode::RippleOverwrite);
        QCOMPARE(*TimelineController::sourceEditModeFor(QStringLiteral("edit.append_at_end")),
            SourceEditMode::AppendAtEnd);
        QVERIFY(!TimelineController::sourceEditModeFor(QStringLiteral("edit.copy")));
    }

    void spec4_14_severalSourcesAreOneUndoStep()
    {
        View v;
        const QString a = addMedia(v.f.project, QStringLiteral("a.mp4"));
        const QString b = addMedia(v.f.project, QStringLiteral("b.mp4"));
        const int before = v.undoCount();
        const QVector<quint64> ids
            = v.c.editSources({{a, 0, 1'000'000}, {b, 0, 2'000'000}}, SourceEditMode::Overwrite);
        QCOMPARE(ids.size(), 4); // V + A of each
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:25 25:50"));
        QCOMPARE(layout(v.f.view(), kA1), QStringLiteral("0:25 25:50"));
        QCOMPARE(v.undoCount(), before + 1);
        v.f.project.undo();
        QVERIFY(v.f.view().track(kV1)->clips.empty());
    }

    void spec4_14_replaceUsesTheFirstSource()
    {
        View v;
        const QString a = addMedia(v.f.project, QStringLiteral("a.mp4"));
        const QString b = addMedia(v.f.project, QStringLiteral("b.mp4"));
        v.put(kV1, mediaClip(a, 0, 50, 0));
        v.f.project.timeline()->setPlayhead(10);
        const int before = v.undoCount();
        v.c.editSources({{b, 0, 0}, {a, 0, 0}}, SourceEditMode::Replace);
        QCOMPARE(v.undoCount(), before + 1);
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:50")); // place and length kept
        QCOMPARE(v.f.view().track(kV1)->clips.front().mediaId, b);
    }

    void spec4_7_replaceLinesUpTheSourcePlayhead()
    {
        View v;
        const QString a = addMedia(v.f.project, QStringLiteral("a.mp4"));
        const QString b = addMedia(v.f.project, QStringLiteral("b.mp4"));
        v.put(kV1, mediaClip(a, 20, 50, 0));
        v.f.project.timeline()->setPlayhead(30); // 10 frames into the clip
        // Source playhead at 2 s (frame 50): that frame lands under the timeline playhead.
        v.c.insertSource(b, 0, 0, SourceEditMode::Replace, 2'000'000);
        const ClipData& c = v.f.view().track(kV1)->clips.front();
        QCOMPARE(c.mediaId, b);
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("20:50"));
        QCOMPARE(c.sourceInUs, int64_t(2'000'000 - 10 * 40'000));
        // Near the media start the in point is clamped at 0.
        v.f.project.timeline()->setPlayhead(30); // the edit moved it after the clip
        v.c.insertSource(a, 0, 0, SourceEditMode::Replace, 100'000);
        QCOMPARE(v.f.view().track(kV1)->clips.front().sourceInUs, int64_t(0));
    }

    // ---- spec 21.10: Alt+Up / Alt+Down ----

    void spec21_10_moveClipsUpAndDownATrack()
    {
        View v;
        const quint64 vid = v.put(kV1, solidClip(10, 20));
        const quint64 aud = v.put(kA1, solidClip(10, 20));
        v.f.project.beginEdit(QStringLiteral("setup")); // exactly V1, V2 and A1, A2
        for (const TrackType type : {TrackType::Video, TrackType::Audio}) {
            while (v.f.data().tracks(type).size() < 2)
                ops::addTrack(v.f.data(), type);
            while (v.f.data().tracks(type).size() > 2)
                ops::removeTrack(v.f.data(), TrackRef {type, 2});
        }
        v.f.project.endEdit();
        constexpr TrackRef kV2 {TrackType::Video, 1};
        constexpr TrackRef kA2 {TrackType::Audio, 1};
        v.f.selection.setClips({vid, aud});
        const int before = v.undoCount();
        v.f.actions.action(QStringLiteral("trim.move_up"))->trigger();
        QCOMPARE(layout(v.f.view(), kV2), QStringLiteral("10:20")); // the video moved up
        QCOMPARE(layout(v.f.view(), kA1), QStringLiteral("10:20")); // the audio stayed
        QCOMPARE(v.undoCount(), before + 1);
        v.f.actions.action(QStringLiteral("trim.move_up"))->trigger(); // top track: no-op
        QCOMPARE(v.undoCount(), before + 1);
        // Only audio selected: Down moves it to A2 (further down on screen).
        v.f.selection.setClips({aud});
        v.f.actions.action(QStringLiteral("trim.move_down"))->trigger();
        QCOMPARE(layout(v.f.view(), kA2), QStringLiteral("10:20"));
        QCOMPARE(v.undoCount(), before + 2);
    }

    // ---- spec 12.8: Delete Empty Tracks ----

    void spec12_8_deleteEmptyTracks()
    {
        View v;
        v.f.project.beginEdit(QStringLiteral("setup"));
        for (int i = 0; i < 3; ++i) {
            ops::addTrack(v.f.data(), TrackType::Video);
            ops::addTrack(v.f.data(), TrackType::Audio);
        }
        v.f.project.endEdit();
        v.put(TrackRef {TrackType::Video, 2}, solidClip(0, 10));
        v.f.project.beginEdit(QStringLiteral("setup"));
        v.f.data().videoTracks[3].locked = true;
        v.f.project.endEdit();
        const int before = v.undoCount();
        v.f.actions.action(QStringLiteral("timeline.delete_empty_tracks"))->trigger();
        // V: the clip's track and the locked one stay; A: one empty track is kept.
        QCOMPARE(int(v.f.view().videoTracks.size()), 2);
        QCOMPARE(int(v.f.view().audioTracks.size()), 1);
        QCOMPARE(v.f.view().videoTracks[0].clips.size(), size_t(1));
        QVERIFY(v.f.view().videoTracks[1].locked);
        QCOMPARE(v.undoCount(), before + 1);
        v.f.actions.action(QStringLiteral("timeline.delete_empty_tracks"))->trigger();
        QCOMPARE(v.undoCount(), before + 1); // nothing left to delete: no step
    }
};

QTEST_MAIN(TestEditAids)
#include "test_editaids.moc"
