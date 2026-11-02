// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of spec section 32 on the timeline: audio level line drag (one undo step, Shift
// fine), Alt+click adds a volume key, key drag in time and value, key delete, fade handles;
// stacked timelines (toggle, activation by click and focus routing, copy drops both ways).
#include "../panels/PanelTestUtils.h"
#include "app/Theme.h"
#include "core/LevelOps.h"
#include "core/ParamKeys.h"
#include "ui/common/DragMime.h"
#include "ui/common/Resources.h"
#include "ui/timeline/LevelLines.h"
#include "ui/timeline/StackedTimelinePane.h"
#include "ui/timeline/TimelineController.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QSettings>
#include <QStandardPaths>
#include <QTabBar>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
namespace ll = levellines;

namespace {
constexpr TrackRef kV1 {TrackType::Video, 0};
constexpr TrackRef kA1 {TrackType::Audio, 0};

/// DragEnter, DragMove and Drop at @p pos (Qt sends a drop only to the widget that accepted
/// the enter). Returns whether the drop was accepted.
bool sendDrag(QWidget* target, const QMimeData* mime, QPointF pos)
{
    QDragEnterEvent enter(pos.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, {});
    enter.setAccepted(false);
    QCoreApplication::sendEvent(target, &enter);
    QDragMoveEvent move(pos.toPoint(), Qt::CopyAction, mime, Qt::LeftButton, {});
    move.setAccepted(false);
    QCoreApplication::sendEvent(target, &move);
    QDropEvent drop(pos, Qt::CopyAction, mime, Qt::LeftButton, {});
    drop.setAccepted(false);
    QCoreApplication::sendEvent(target, &drop);
    return drop.isAccepted();
}

double volumeAt(const ClipData& c, double local)
{
    return levelops::volumeDbAt(c, local);
}
} // namespace

class TestLevelLines : public QObject
{
    Q_OBJECT

    struct View
    {
        PanelFixture f;
        TimelinePanel panel {f.ctx};
        TimelineWidget* w = nullptr;
        quint64 clip = 0;
        View()
        {
            panel.controller()->setNamePrompt({});
            panel.controller()->setConfirmPrompt({});
            panel.controller()->setMessageSink({});
            panel.resize(1400, 600);
            panel.show();
            w = panel.timelineWidget();
            clip = f.addClip(kA1, 0, 200);
            w->setPixelsPerFrame(5.0, timelinemetrics::kHeaderWidth);
            w->scrollToFrame(0);
            QApplication::processEvents();
        }
        QRectF clipRect() const
        {
            const ClipData* c = f.clip(clip);
            return w->layout().clipRect(w->layout().rowOf(kA1), *c);
        }
        QRectF area() const { return ll::levelArea(clipRect()); }
        QPoint lineAt(double frame) const
        {
            const double x = w->layout().frameToX(frame);
            return QPointF(x, ll::yForDb(area(), volumeAt(*f.clip(clip), frame))).toPoint();
        }
    };

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_levellines"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    void lineDragIsOneUndoStep()
    {
        View v;
        QVERIFY(!v.area().isEmpty());
        QWidget* vp = v.w->viewport();
        const QPoint start = v.lineAt(100);
        const int before = v.f.project.undoStack()->count();
        QTest::mousePress(vp, Qt::LeftButton, {}, start);
        // Down by 12 px in steps (past the drag threshold).
        for (int dy = 3; dy <= 12; dy += 3)
            QTest::mouseMove(vp, start + QPoint(0, dy));
        QTest::mouseRelease(vp, Qt::LeftButton, {}, start + QPoint(0, 12));
        const double expected = -12 * ll::dbPerPixel(v.area());
        const double db = volumeAt(*v.f.clip(v.clip), 0);
        QVERIFY2(std::abs(db - expected) < 0.01, qPrintable(QString::number(db)));
        QCOMPARE(v.f.project.undoStack()->count(), before + 1);
        // The clip stays where it was (not moved) and keeps its selection state.
        QCOMPARE(v.f.clip(v.clip)->start, 0);
        v.f.project.undo();
        QCOMPARE(volumeAt(*v.f.clip(v.clip), 0), 0.0);
    }

    void altClickAddsKeyAndKeyDrag()
    {
        View v;
        QWidget* vp = v.w->viewport();
        QTest::mouseClick(vp, Qt::LeftButton, Qt::AltModifier, v.lineAt(50.2));
        AnimatedValue val = v.f.clip(v.clip)->audio.values.value(keys::VolumeDb);
        QCOMPARE(val.keys.size(), 1);
        QCOMPARE(val.keys[0].frame, 50);
        QCOMPARE(v.f.selection.keyframes().size(), 1);
        QTest::mouseClick(vp, Qt::LeftButton, Qt::AltModifier, v.lineAt(150.2));
        QCOMPARE(v.f.clip(v.clip)->audio.values[keys::VolumeDb].keys.size(), 2);

        // Drag the key at 50 right by 10 frames (50 px) and down 20 px.
        const QPoint key = v.lineAt(50);
        const int before = v.f.project.undoStack()->count();
        QTest::mousePress(vp, Qt::LeftButton, {}, key);
        for (int i = 1; i <= 10; ++i)
            QTest::mouseMove(vp, key + QPoint(5 * i, 2 * i));
        QTest::mouseRelease(vp, Qt::LeftButton, {}, key + QPoint(50, 20));
        QCOMPARE(v.f.project.undoStack()->count(), before + 1);
        val = v.f.clip(v.clip)->audio.values.value(keys::VolumeDb);
        QCOMPARE(val.keys.size(), 2);
        QCOMPARE(val.keys[0].frame, 60);
        QVERIFY(std::abs(val.keys[0].value - (-20 * ll::dbPerPixel(v.area()))) < 0.01);
        // The line now ramps from the key's value to 0 dB at frame 150.
        QVERIFY(volumeAt(*v.f.clip(v.clip), 105) < 0.0);
        QVERIFY(volumeAt(*v.f.clip(v.clip), 105) > val.keys[0].value);

        // Selected key + Delete (edit.ripple_delete) removes only the key, not the clip.
        QCOMPARE(v.f.selection.keyframes().size(), 1);
        v.f.actions.action(QStringLiteral("edit.ripple_delete"))->trigger();
        QVERIFY(v.f.clip(v.clip));
        QCOMPARE(v.f.clip(v.clip)->audio.values[keys::VolumeDb].keys.size(), 1);
        v.f.project.undo();
        QCOMPARE(v.f.clip(v.clip)->audio.values[keys::VolumeDb].keys.size(), 2);
    }

    void fadeHandles()
    {
        View v;
        QWidget* vp = v.w->viewport();
        const ClipData* c = v.f.clip(v.clip);
        const QRectF head = ll::fadeHandleRect(*c, v.clipRect(), v.w->layout(), true);
        QVERIFY(!head.isEmpty());
        const QPoint start = head.center().toPoint();
        QTest::mouseMove(vp, start); // hover shows the handles
        QTest::mousePress(vp, Qt::LeftButton, {}, start);
        for (int dx = 5; dx <= 100; dx += 5)
            QTest::mouseMove(vp, start + QPoint(dx, 0));
        QTest::mouseRelease(vp, Qt::LeftButton, {}, start + QPoint(100, 0));
        // 100 px at 5 px per frame from the press (which sits a few px inside the corner).
        const int64_t expected = std::llround((start.x() + 100 - v.w->layout().frameToX(0)) / 5.0);
        QCOMPARE(v.f.clip(v.clip)->fadeInFrames, expected);
        QCOMPARE(v.f.clip(v.clip)->start, 0); // not trimmed, not moved
        QCOMPARE(v.f.clip(v.clip)->duration, 200);

        // Tail handle on a video clip.
        const quint64 vid = v.f.addClip(kV1, 0, 200);
        const QRectF vr = v.w->layout().clipRect(v.w->layout().rowOf(kV1), *v.f.clip(vid));
        const QPoint tail
            = ll::fadeHandleRect(*v.f.clip(vid), vr, v.w->layout(), false).center().toPoint();
        QTest::mousePress(vp, Qt::LeftButton, {}, tail);
        for (int dx = 5; dx <= 50; dx += 5)
            QTest::mouseMove(vp, tail - QPoint(dx, 0));
        QTest::mouseRelease(vp, Qt::LeftButton, {}, tail - QPoint(50, 0));
        QVERIFY(v.f.clip(vid)->fadeOutFrames >= 10 && v.f.clip(vid)->fadeOutFrames <= 12);
        v.f.project.undo();
        QCOMPARE(v.f.clip(vid)->fadeOutFrames, 0);
        saveScreenshot(v.panel, QStringLiteral("levellines"));
    }

    void stackedTimelines()
    {
        PanelFixture f;
        TimelinePanel panel(f.ctx);
        panel.controller()->setNamePrompt({});
        panel.controller()->setConfirmPrompt({});
        panel.controller()->setMessageSink({});
        panel.resize(1400, 800);
        panel.show();
        const quint64 first = f.project.activeTimelineUid();
        const quint64 a = f.addClip(kV1, 10, 50, ClipKind::Media, QStringLiteral("A"));
        f.actions.action(QStringLiteral("timeline.new"))->trigger();
        const quint64 second = f.project.activeTimelineUid();
        QVERIFY(second != first);

        // View > Stacked Timelines.
        QAction* toggle = f.actions.action(QStringLiteral("view.stacked_timelines"));
        QVERIFY(toggle->isCheckable());
        toggle->setChecked(true);
        QVERIFY(panel.stackedTimelines());
        StackedTimelinePane* pane = panel.stackedPane();
        QVERIFY(pane && pane->isVisible());
        QCOMPARE(pane->currentTimeline(), first);
        QCOMPARE(panel.activePaneIndex(), 1); // editor at the bottom
        QApplication::processEvents();

        // Own playhead: scrubbing the pane's ruler moves only that timeline's playhead.
        StackedTimelineView* view = pane->view();
        const int64_t mainPlayhead = f.project.timeline()->playhead();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {},
            QPoint(int(std::lround(view->layout().frameToX(30.5))),
                timelinemetrics::kRulerHeight / 2));
        QCOMPARE(f.project.timelinePlayhead(first), 30);
        QCOMPARE(f.project.timeline()->playhead(), mainPlayhead);
        QCOMPARE(f.project.activeTimelineUid(), second);

        // Drop of the pane's clip on the editor: a copy into the active timeline.
        auto payload = [](quint64 uid, quint64 clip) {
            auto* mime = new QMimeData;
            mime->setData(QLatin1String(mime::TimelineClips),
                QJsonDocument(QJsonObject {{QLatin1String("timeline"), qint64(uid)},
                                  {QLatin1String("clips"), QJsonArray {qint64(clip)}},
                                  {QLatin1String("grabFrames"), 0}})
                    .toJson());
            return mime;
        };
        TimelineWidget* w = panel.timelineWidget();
        const int v1 = w->layout().rowOf(kV1);
        const QPointF dropPos(w->layout().frameToX(100), w->layout().rowRect(v1).center().y());
        std::unique_ptr<QMimeData> mime(payload(first, a));
        QVERIFY(sendDrag(w->viewport(), mime.get(), dropPos));
        const TimelineData& copy = f.project.timeline()->data();
        QCOMPARE(copy.videoTracks[0].clips.size(), size_t(1));
        QCOMPARE(copy.videoTracks[0].clips[0].start, 100);
        QCOMPARE(copy.videoTracks[0].clips[0].name, QStringLiteral("A"));
        // The source is untouched (copy, not move).
        QCOMPARE(f.project.timelineData(first)->videoTracks[0].clips.size(), size_t(1));
        const quint64 copied = copy.videoTracks[0].clips[0].id;

        // A click in the pane's tracks activates it: it drives the viewer (the project's
        // active timeline), the editor moves up, the pane shows the other one; keys go there.
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {},
            QPoint(int(view->layout().frameToX(5)), view->layout().rowRect(0).center().y()));
        QCOMPARE(f.project.activeTimelineUid(), first);
        QCOMPARE(panel.activePaneIndex(), 0);
        QCOMPARE(pane->currentTimeline(), second);
        QCOMPARE(f.project.timeline()->playhead(), 30); // its own playhead came along
        QVERIFY(f.project.tabs().contains(first));
        QVERIFY(!f.project.tabs().contains(second)); // each pane keeps its own tabs
        QVERIFY(pane->tabs().contains(second));
        f.selection.setClips({a});
        f.actions.action(QStringLiteral("edit.delete"))->trigger(); // acts on the active one
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips.size(), size_t(0));
        f.project.undo();
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips.size(), size_t(1));

        // A drop of the editor's clip on the pane: the pane's timeline becomes active and gets
        // the copy (one undo step).
        view = pane->view();
        const int pv1 = view->layout().rowOf(kV1);
        const QPointF paneDrop(
            view->layout().frameToX(200), view->layout().rowRect(pv1).center().y());
        std::unique_ptr<QMimeData> back(payload(first, a));
        QVERIFY(sendDrag(view->viewport(), back.get(), paneDrop));
        QCOMPARE(f.project.activeTimelineUid(), second);
        const TimelineData& now = f.project.timeline()->data();
        QCOMPARE(now.videoTracks[0].clips.size(), size_t(2));
        QCOMPARE(now.videoTracks[0].clips[0].id, copied);
        QCOMPARE(now.videoTracks[0].clips[1].start, 200);
        f.project.undo();
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips.size(), size_t(1));
        saveScreenshot(panel, QStringLiteral("stacked"));

        toggle->setChecked(false);
        QVERIFY(!pane->isVisible());
        QVERIFY(w->isVisible());
    }
};

QTEST_MAIN(TestLevelLines)
#include "test_levellines.moc"
