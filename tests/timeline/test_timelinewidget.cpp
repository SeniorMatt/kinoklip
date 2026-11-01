// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen UI tests for TimelineWidget: simulated mouse gestures (move, duplicate, trims,
// roll, slip, blade, rubber band, scrub, header buttons, Esc), drag and drop, zoom and undo.
// Every test asserts the resulting TimelineData and the undo behaviour.
#include "TimelineTestUtils.h"
#include "core/MediaPool.h"
#include "ui/common/DragMime.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelineWidget.h"

#include <QMimeData>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
using namespace kk::timelinemetrics;
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

/// Fixture + controller + a shown 1200x500 widget at a known zoom (no media providers).
struct View
{
    TimelineFixture f;
    TimelineController c {f.ctx};
    TimelineWidget w {f.ctx, &c};

    explicit View(double ppf = 10.0)
    {
        c.connectActions();
        w.setThumbnailProvider({});
        w.setWaveformProvider({});
        w.resize(1200, 500);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.setPixelsPerFrame(ppf, kHeaderWidth);
        w.scrollToFrame(0);
    }
    QWidget* vp() { return w.viewport(); }
    /// View point at frame @p frame (fractional allowed) in the middle of track @p ref.
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
    /// Press at @p from, move in a few steps to @p to, release.
    void drag(QPoint from, QPoint to, Qt::KeyboardModifiers mods = {})
    {
        QTest::mousePress(vp(), Qt::LeftButton, mods, from);
        for (int i = 1; i <= 4; ++i)
            QTest::mouseMove(vp(), from + (to - from) * i / 4);
        QTest::mouseRelease(vp(), Qt::LeftButton, mods, to);
    }
    int undoCount() const { return f.project.undoStack()->count(); }
};

/// Sends a drag-enter + drop of @p mime at @p pos to the viewport.
bool drop(QWidget* viewport, QMimeData* mime, QPoint pos, Qt::KeyboardModifiers mods = {})
{
    // Qt 6.12 deprecates the QPoint constructors of the drag events; Qt 6.4 has no QPointF ones.
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    const QPointF dragPos(pos);
#else
    const QPoint dragPos(pos);
#endif
    QDragEnterEvent enter(dragPos, Qt::CopyAction, mime, Qt::LeftButton, mods);
    QCoreApplication::sendEvent(viewport, &enter);
    QDragMoveEvent move(dragPos, Qt::CopyAction, mime, Qt::LeftButton, mods);
    QCoreApplication::sendEvent(viewport, &move);
    QDropEvent dropEvent(pos, Qt::CopyAction, mime, Qt::LeftButton, mods);
    QCoreApplication::sendEvent(viewport, &dropEvent);
    return dropEvent.isAccepted();
}
} // namespace

class TestTimelineWidget : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { initTimelineTestCase("test_timelinewidget"); }

    void dragMoveIsOneUndoStep()
    {
        View v;
        v.put(kV1, solidClip(10, 20));
        const int before = v.undoCount();
        v.drag(v.at(20, kV1), v.at(50, kV1));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("40:20"));
        QCOMPARE(v.undoCount(), before + 1);
        v.f.project.undo();
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:20"));
    }

    void dragMoveAcrossTracksAndOverwrite()
    {
        View v;
        v.put(kV1, solidClip(10, 20));
        v.put(kV2, solidClip(40, 20));
        // Up to V2, overlapping the clip there by 5 frames: it gets its head cut.
        v.drag(v.at(20, kV1), v.at(45, kV2));
        QCOMPARE(layout(v.f.view(), kV1), QString());
        QCOMPARE(layout(v.f.view(), kV2), QStringLiteral("35:20 55:5"));
        KK_VERIFY_TIMELINE(v.f.view());
    }

    void dragBackAndForthDoesNotChewNeighbours()
    {
        View v;
        v.put(kV1, solidClip(0, 10));
        v.put(kV1, solidClip(30, 10));
        // Move the first clip over the second and back within one gesture: the second clip
        // must be intact because every update starts again from the press-time state.
        const QPoint from = v.at(5, kV1);
        QTest::mousePress(v.vp(), Qt::LeftButton, {}, from);
        QTest::mouseMove(v.vp(), from + QPoint(50, 0));
        QTest::mouseMove(v.vp(), v.at(35, kV1));
        QTest::mouseMove(v.vp(), v.at(15, kV1));
        QTest::mouseRelease(v.vp(), Qt::LeftButton, {}, v.at(15, kV1));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:10 30:10"));
    }

    void linkedClipsMoveTogether()
    {
        View v;
        const quint64 a = v.put(kV1, solidClip(10, 20));
        const quint64 b = v.put(kA1, solidClip(10, 20));
        v.f.project.beginEdit(QStringLiteral("link"));
        ops::linkClips(v.f.data(), {a, b});
        v.f.project.endEdit();
        v.drag(v.at(20, kV1), v.at(30, kV1));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("20:20"));
        QCOMPARE(layout(v.f.view(), kA1), QStringLiteral("20:20"));
        // Linked selection off: only the grabbed clip moves.
        v.c.setLinkedSelection(false);
        v.f.selection.clear();
        v.drag(v.at(30, kV1), v.at(40, kV1));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("30:20"));
        QCOMPARE(layout(v.f.view(), kA1), QStringLiteral("20:20"));
    }

    void altDragDuplicates()
    {
        View v;
        const quint64 a = v.put(kV1, solidClip(10, 20));
        v.drag(v.at(20, kV1), v.at(60, kV1), Qt::AltModifier);
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:20 50:20"));
        QVERIFY(!v.f.selection.isSelected(a)); // the copy is selected
        QCOMPARE(v.f.selection.clips().size(), 1);
        KK_VERIFY_TIMELINE(v.f.view());
    }

    void moveSnapsToEdges()
    {
        View v(2.0); // 8 px snap = 4 frames
        v.put(kV1, solidClip(0, 50));
        v.put(kV2, solidClip(100, 20));
        // Drop the V2 clip's start 3 frames after V1's end: it snaps to 50.
        v.drag(v.at(110, kV2), v.at(63, kV2));
        QCOMPARE(layout(v.f.view(), kV2), QStringLiteral("50:20"));
        v.c.setSnapping(false);
        v.drag(v.at(60, kV2), v.at(73, kV2)); // ends 3 frames past V1's end: no snap
        QCOMPARE(layout(v.f.view(), kV2), QStringLiteral("63:20"));
    }

    void trimWithGapInSelectTool()
    {
        View v;
        v.put(kV1, solidClip(10, 20));
        v.put(kV1, solidClip(50, 20));
        // Tail: grab 2 px inside the edge, drag +5 frames.
        const QPoint tail = v.at(30, kV1) - QPoint(2, 0);
        v.drag(tail, tail + QPoint(50, 0));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:25 50:20"));
        // Head of the second clip: -10 frames is limited by the first clip's new end.
        const QPoint head = v.at(50, kV1) + QPoint(2, 0);
        v.drag(head, head - QPoint(200, 0));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:25 35:35"));
    }

    void rippleTrimInTrimTool()
    {
        View v;
        v.put(kV1, solidClip(10, 20));
        v.put(kV1, solidClip(40, 20));
        v.c.setTool(TimelineTool::Trim);
        const QPoint tail = v.at(30, kV1) - QPoint(2, 0);
        v.drag(tail, tail - QPoint(50, 0));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:15 35:20"));
    }

    void rollAtEditPointInTrimTool()
    {
        View v;
        v.put(kV1, solidClip(10, 20));
        v.put(kV1, solidClip(30, 20));
        v.c.setTool(TimelineTool::Trim);
        const QPoint cut = v.at(30, kV1) + QPoint(2, 0);
        v.drag(cut, cut + QPoint(70, 0));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:27 37:13"));
    }

    void slipInTrimTool()
    {
        View v;
        ClipData c = solidClip(10, 20);
        c.kind = ClipKind::Media;
        c.sourceInUs = 40 * 40000; // 40 frames into a 200 frame source
        c.sourceDurationUs = 200 * 40000;
        const quint64 id = v.put(kV1, c);
        v.c.setTool(TimelineTool::Trim);
        // Dragging right by 10 frames shows earlier material.
        v.drag(v.at(20, kV1), v.at(30, kV1));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:20"));
        QCOMPARE(ops::clipById(v.f.view(), id)->sourceInUs, int64_t(30 * 40000));
    }

    void bladeSplitsLinkedClips()
    {
        View v;
        const quint64 a = v.put(kV1, solidClip(10, 20));
        const quint64 b = v.put(kA1, solidClip(10, 20));
        v.f.project.beginEdit(QStringLiteral("link"));
        ops::linkClips(v.f.data(), {a, b});
        v.f.project.endEdit();
        v.c.setSnapping(false);
        v.f.actions.action(QStringLiteral("tool.blade"))->trigger();
        const int before = v.undoCount();
        QTest::mouseClick(v.vp(), Qt::LeftButton, {}, v.at(17, kV1));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:7 17:13"));
        QCOMPARE(layout(v.f.view(), kA1), QStringLiteral("10:7 17:13"));
        QCOMPARE(v.undoCount(), before + 1);
        v.f.project.undo();
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:20"));
        QCOMPARE(layout(v.f.view(), kA1), QStringLiteral("10:20"));
    }

    void escapeCancelsDrag()
    {
        View v;
        v.put(kV1, solidClip(10, 20));
        const int before = v.undoCount();
        const QPoint from = v.at(20, kV1);
        QTest::mousePress(v.vp(), Qt::LeftButton, {}, from);
        QTest::mouseMove(v.vp(), from + QPoint(100, 0));
        QTest::mouseMove(v.vp(), from + QPoint(200, 0));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("30:20")); // live update
        QTest::keyClick(&v.w, Qt::Key_Escape);
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:20"));
        QTest::mouseRelease(v.vp(), Qt::LeftButton, {}, from + QPoint(200, 0));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("10:20"));
        QCOMPARE(v.undoCount(), before);
    }

    void clickAndRubberBandSelection()
    {
        View v;
        const quint64 a = v.put(kV1, solidClip(10, 10));
        const quint64 b = v.put(kV2, solidClip(30, 10));
        const quint64 c = v.put(kV1, solidClip(60, 10));
        QTest::mouseClick(v.vp(), Qt::LeftButton, {}, v.at(15, kV1));
        QCOMPARE(v.f.selection.clips(), QVector<quint64> {a});
        QTest::mouseClick(v.vp(), Qt::LeftButton, Qt::ControlModifier, v.at(65, kV1));
        QCOMPARE(v.f.selection.clips().size(), 2);
        QTest::mouseClick(v.vp(), Qt::LeftButton, Qt::ShiftModifier, v.at(15, kV1));
        QCOMPARE(v.f.selection.clips(), QVector<quint64> {c});
        // Click on empty space clears; a band from empty space selects what it touches.
        QTest::mouseClick(v.vp(), Qt::LeftButton, {}, v.at(50, kV1));
        QVERIFY(v.f.selection.clips().isEmpty());
        v.drag(v.at(5, kV2) - QPoint(0, 10), v.at(35, kV1));
        QCOMPARE(v.f.selection.clips().size(), 2);
        QVERIFY(v.f.selection.isSelected(a));
        QVERIFY(v.f.selection.isSelected(b));
    }

    void scrubRulerMovesPlayhead()
    {
        View v;
        v.c.setSnapping(false);
        const int x = int(v.w.layout().frameToX(42.3));
        QTest::mousePress(v.vp(), Qt::LeftButton, {}, QPoint(x, 10));
        QCOMPARE(v.f.project.timeline()->playhead(), int64_t(42));
        QTest::mouseMove(v.vp(), QPoint(int(v.w.layout().frameToX(77)), 10));
        QTest::mouseRelease(v.vp(), Qt::LeftButton, {}, QPoint(int(v.w.layout().frameToX(77)), 10));
        QCOMPARE(v.f.project.timeline()->playhead(), int64_t(77));
    }

    void headerButtonsToggleTrackState()
    {
        View v;
        const TimelineLayout& L = v.w.layout();
        const int row = L.rowOf(kA1);
        QTest::mouseClick(
            v.vp(), Qt::LeftButton, {}, L.headerButtonRect(row, HeaderButton::Mute).center());
        QVERIFY(v.f.view().audioTracks[0].muted);
        QTest::mouseClick(
            v.vp(), Qt::LeftButton, {}, L.headerButtonRect(row, HeaderButton::Lock).center());
        QVERIFY(v.f.view().audioTracks[0].locked);
        QTest::mouseClick(
            v.vp(), Qt::LeftButton, {}, L.headerButtonRect(row, HeaderButton::Target).center());
        QCOMPARE(v.c.targetAudioTrack(), -1);
        // Locked: a drag does not move clips.
        v.put(kA1, solidClip(10, 20));
        v.drag(v.at(20, kA1), v.at(50, kA1));
        QCOMPARE(layout(v.f.view(), kA1), QString());
    }

    void resizeTrackByDraggingHeaderEdge()
    {
        View v;
        const TimelineLayout& L = v.w.layout();
        const QRect r = L.rowRect(L.rowOf(kV2));
        const QPoint edge(100, r.bottom());
        v.drag(edge, edge + QPoint(0, 30));
        QCOMPARE(v.f.view().videoTracks[1].height, kDefaultVideoHeight + 30);
    }

    void dropMediaOverwritesAndCtrlInserts()
    {
        View v;
        MediaItem item;
        item.path = QStringLiteral("/nonexistent/a.mp4");
        item.name = QStringLiteral("a.mp4");
        item.info.valid = item.info.hasVideo = item.info.hasAudio = true;
        item.info.durationUs = 2'000'000; // 50 frames
        const QString id = v.f.project.mediaPool()->addItem(item);
        v.put(kV1, solidClip(0, 100));
        v.c.setSnapping(false);

        auto* mime = new QMimeData;
        mime->setData(QLatin1String(mime::MediaItems), id.toUtf8());
        QVERIFY(drop(v.vp(), mime, v.at(20, kV1)));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:20 20:50 70:30"));
        QCOMPARE(layout(v.f.view(), kA1), QStringLiteral("20:50"));
        const ClipData& clip = v.f.view().videoTracks[0].clips[1];
        QVERIFY(clip.linkGroup != 0);
        QCOMPARE(v.f.view().audioTracks[0].clips[0].linkGroup, clip.linkGroup);

        QVERIFY(drop(v.vp(), mime, v.at(0, kV1), Qt::ControlModifier));
        QCOMPARE(layout(v.f.view(), kV1), QStringLiteral("0:50 50:20 70:50 120:30"));
        delete mime;
        KK_VERIFY_TIMELINE(v.f.view());
    }

    void dropEffectsTransitionsAndTitles()
    {
        View v;
        const quint64 a = v.put(kV1, solidClip(0, 50));
        const quint64 b = v.put(kV1, solidClip(50, 50));
        v.c.setSnapping(false);
        QMimeData effect;
        effect.setData(QLatin1String(mime::Effect), "gaussian_blur");
        QVERIFY(drop(v.vp(), &effect, v.at(20, kV1)));
        QCOMPARE(ops::clipById(v.f.view(), a)->effects.size(), 1);
        QCOMPARE(ops::clipById(v.f.view(), a)->effects[0].type, QStringLiteral("gaussian_blur"));
        // A video effect is refused on an audio track.
        v.put(kA1, solidClip(0, 50));
        QVERIFY(!drop(v.vp(), &effect, v.at(20, kA1)));

        QMimeData transition;
        transition.setData(QLatin1String(mime::Effect), "cross_dissolve");
        QVERIFY(drop(v.vp(), &transition, v.at(45, kV1))); // tail of a touching b
        QVERIFY(ops::clipById(v.f.view(), b)->transitionIn);
        QCOMPARE(ops::clipById(v.f.view(), b)->transitionIn->duration, int64_t(25));

        QMimeData title;
        title.setData(QLatin1String(mime::Effect), "text_title");
        QVERIFY(drop(v.vp(), &title, v.at(10, kV2)));
        QCOMPARE(layout(v.f.view(), kV2), QStringLiteral("10:125"));
        QCOMPARE(v.f.view().videoTracks[1].clips[0].kind, ClipKind::Title);
    }

    void zoomControls()
    {
        View v(1.0);
        v.put(kV1, solidClip(0, 5000));
        v.w.zoomIn();
        QVERIFY(v.w.pixelsPerFrame() > 1.0);
        v.w.zoomToFit();
        QVERIFY(v.w.pixelsPerFrame() * 5000 <= v.w.layout().trackAreaWidth());
        const double fit = v.w.pixelsPerFrame();
        // Ctrl+wheel zooms around the mouse position.
        const QPoint pos(700, 200);
        const double frameBefore = v.w.layout().xToFrame(pos.x());
        QWheelEvent wheel(pos, v.vp()->mapToGlobal(pos), {}, QPoint(0, 240), Qt::NoButton,
            Qt::ControlModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(v.vp(), &wheel);
        QVERIFY(v.w.pixelsPerFrame() > fit);
        QVERIFY(qAbs(v.w.layout().xToFrame(pos.x()) - frameBefore) < 1.0);
    }

    void followPlayheadPageScrolls()
    {
        View v;
        v.put(kV1, solidClip(0, 5000));
        v.f.project.timeline()->setPlayhead(3000);
        const double x = v.w.layout().frameToX(3000);
        QVERIFY(x >= kHeaderWidth && x <= v.vp()->width());
    }
};

QTEST_MAIN(TestTimelineWidget)
#include "test_timelinewidget.moc"
