// SPDX-License-Identifier: GPL-3.0-or-later
// On-screen Transform and Crop controls of the timeline viewer (spec 24.11, 24.12): the
// gesture maths (scale around the anchor, Shift toggling the zoom link, rotation unwrapping
// and snapping, anchor moves that keep the picture still, crop limits) and the real overlay:
// handles, one named undo step per drag, keyframes at the playhead when animated.
#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "render/TimelineEval.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/viewer/TransformGesture.h"
#include "ui/viewer/TransformOverlay.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

#include <cmath>

using namespace kk;
namespace tg = kk::transformgesture;

namespace {

constexpr QSizeF kFrame(320, 180);

ClipData solid()
{
    ClipData c;
    c.id = 1;
    c.kind = ClipKind::Solid;
    c.duration = 100;
    return c;
}

/// Drags handle @p h of @p clip from @p from to @p to (frame px) and returns the parameters.
ParamSet dragHandle(
    const ClipData& clip, TransformHandle h, QPointF from, QPointF to, bool shift = false)
{
    const TransformPicture pic = tg::picture(clip, 0, kFrame, nullptr);
    tg::TransformDrag drag;
    drag.begin(clip, 0, pic, h, from);
    ParamSet t = clip.transform;
    drag.update(t, 0, (from + to) / 2, shift);
    drag.update(t, 0, to, shift);
    return t;
}

bool near(double a, double b, double tolerance = 1e-6)
{
    return std::abs(a - b) <= tolerance;
}

/// A viewer with the solid timeline and its clip selected (no playback engine needed).
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    quint64 clipId = 0;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        TimelineData tl = makeDefaultTimeline(QStringLiteral("Demo"), {25, 1}, 320, 180, 2, 2);
        ClipData c = solid();
        c.id = tl.allocateId();
        c.solidColor = QColor(0x30, 0x60, 0x90);
        tl.videoTracks[0].clips.push_back(c);
        project.timeline()->setData(tl);
        clipId = c.id;
        selection.setClips({clipId});
    }
    [[nodiscard]] const ClipData& clip() const
    {
        return *editops::clipById(project.timeline()->data(), clipId);
    }
    QAction* action(const char* id) { return actions.action(QLatin1String(id)); }
};

void press(QWidget* w, QPointF p, Qt::KeyboardModifiers mods = {})
{
    QMouseEvent e(
        QEvent::MouseButtonPress, p, w->mapToGlobal(p), Qt::LeftButton, Qt::LeftButton, mods);
    QCoreApplication::sendEvent(w, &e);
}
void move(QWidget* w, QPointF p, Qt::KeyboardModifiers mods = {})
{
    QMouseEvent e(QEvent::MouseMove, p, w->mapToGlobal(p), Qt::NoButton, Qt::LeftButton, mods);
    QCoreApplication::sendEvent(w, &e);
}
void release(QWidget* w, QPointF p)
{
    QMouseEvent e(
        QEvent::MouseButtonRelease, p, w->mapToGlobal(p), Qt::LeftButton, Qt::NoButton, {});
    QCoreApplication::sendEvent(w, &e);
}

/// Widget position of the handle @p sx / @p sy of the overlay's picture (or crop).
QPointF handleAt(TransformOverlay* overlay, int sx, int sy, bool crop)
{
    const auto pic = overlay->picture();
    const auto map = overlay->frameToWidget();
    for (const auto& [p, h] : tg::handles(*pic, *map, crop)) {
        if (h.sx == sx && h.sy == sy)
            return p;
    }
    return {};
}

void saveShot(QWidget& w, const QString& name)
{
    const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR");
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    w.grab().save(QDir(dir).filePath(name + QStringLiteral(".png")));
}

} // namespace

class TestTransformControls : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
    }

    // Corners scale around the anchor keeping the aspect while the zoom is linked; Shift
    // frees it (and unlinks); edges stretch one axis only when unlinked.
    void scaleHandles()
    {
        const ClipData c = solid(); // 320x180 content centred on the frame centre (160, 90)
        // Bottom-right corner from (320, 180) to (400, 225): 1.5x.
        ParamSet t = dragHandle(c, {TransformHandle::Kind::Scale, 1, 1}, {320, 180}, {400, 225});
        QVERIFY(near(t.value(keys::ZoomX, 0), 1.5));
        QVERIFY(near(t.value(keys::ZoomY, 0), 1.5));
        // Off the diagonal the linked corner still keeps the aspect (best fit).
        t = dragHandle(c, {TransformHandle::Kind::Scale, 1, 1}, {320, 180}, {400, 180});
        QVERIFY(near(t.value(keys::ZoomX, 0), t.value(keys::ZoomY, 0)));
        // Shift: free corner, unlinks the zoom.
        t = dragHandle(c, {TransformHandle::Kind::Scale, 1, 1}, {320, 180}, {400, 180}, true);
        QVERIFY(near(t.value(keys::ZoomX, 0), 1.5));
        QVERIFY(near(t.value(keys::ZoomY, 0), 1.0));
        QCOMPARE(t.staticValue(keys::ZoomLinked, true).toBool(), false);
        // Linked right edge: both axes follow.
        t = dragHandle(c, {TransformHandle::Kind::Scale, 1, 0}, {320, 90}, {240, 90});
        QVERIFY(near(t.value(keys::ZoomX, 0), 0.5));
        QVERIFY(near(t.value(keys::ZoomY, 0), 0.5));
        // Unlinked: the top edge only changes Zoom Y; Shift links it for the drag.
        ClipData unlinked = c;
        unlinked.transform.setStatic(keys::ZoomLinked, false);
        t = dragHandle(unlinked, {TransformHandle::Kind::Scale, 0, -1}, {160, 0}, {160, -90});
        QVERIFY(near(t.value(keys::ZoomX, 0), 1.0));
        QVERIFY(near(t.value(keys::ZoomY, 0), 2.0));
        t = dragHandle(unlinked, {TransformHandle::Kind::Scale, 0, -1}, {160, 0}, {160, -90}, true);
        QVERIFY(near(t.value(keys::ZoomX, 0), 2.0));
        // Never below the Inspector's minimum (dragging past the anchor).
        t = dragHandle(c, {TransformHandle::Kind::Scale, 1, 1}, {320, 180}, {0, 0});
        QVERIFY(near(t.value(keys::ZoomX, 0), 0.01));
        // A rotated clip scales along its own axes.
        ClipData turned = c;
        turned.transform.setBase(keys::Rotation, 90);
        // Its right edge midpoint is now below the centre: (160, 90 + 160).
        t = dragHandle(turned, {TransformHandle::Kind::Scale, 1, 0}, {160, 250}, {160, 410});
        QVERIFY(near(t.value(keys::ZoomX, 0), 2.0));
    }

    // The knob turns around the anchor; turns accumulate past 180 degrees; Shift snaps.
    void rotateHandle()
    {
        const ClipData c = solid();
        const TransformPicture pic = tg::picture(c, 0, kFrame, nullptr);
        tg::TransformDrag drag;
        drag.begin(c, 0, pic, {TransformHandle::Kind::Rotate, 0, 0}, {160, 0}); // straight up
        ParamSet t = c.transform;
        drag.update(t, 0, {260, 90}, false); // right: +90
        QVERIFY(near(t.value(keys::Rotation, 0), 90));
        drag.update(t, 0, {160, 190}, false); // down: +180
        drag.update(t, 0, {60, 91}, false); // left, a hair below: about +270
        QVERIFY(std::abs(t.value(keys::Rotation, 0) - 270) < 1.0);
        drag.update(t, 0, {160, 0}, false); // up again: a full turn
        QVERIFY(near(t.value(keys::Rotation, 0), 360));
        drag.update(t, 0, {260, 70}, true); // ~ +78 -> snapped to 435 (360 + 75)
        QVERIFY(near(t.value(keys::Rotation, 0), 435));
    }

    // Dragging the anchor moves the pivot, not the picture, also when zoomed and rotated.
    void anchorKeepsThePictureStill()
    {
        ClipData c = solid();
        c.transform.setBase(keys::ZoomX, 2.0);
        c.transform.setBase(keys::ZoomY, 2.0);
        c.transform.setBase(keys::Rotation, 30);
        const QTransform before = render::clipTransform(c, 0, kFrame);
        const TransformPicture pic = tg::picture(c, 0, kFrame, nullptr);
        tg::TransformDrag drag;
        const QPointF pivot = before.map(QPointF(0, 0));
        drag.begin(c, 0, pic, {TransformHandle::Kind::Anchor, 0, 0}, pivot);
        ParamSet t = c.transform;
        const QPointF target = before.map(QPointF(40, -20)); // a content point
        drag.update(t, 0, target, false);
        QVERIFY(near(t.value(keys::AnchorX, 0), 40));
        QVERIFY(near(t.value(keys::AnchorY, 0), -20));
        ClipData after = c;
        after.transform = t;
        const QTransform moved = render::clipTransform(after, 0, kFrame);
        for (const QPointF q : {QPointF(-160, -90), QPointF(160, 90), QPointF(0, 0)})
            QVERIFY(QLineF(before.map(q), moved.map(q)).length() < 1.5); // whole-pixel rounding
        QVERIFY(QLineF(moved.map(QPointF(40, -20)), target).length() < 1.5);
    }

    // Crop edges in source pixels; a corner sets two edges; never past the opposite edge.
    void cropHandles()
    {
        const ClipData c = solid();
        ParamSet t = dragHandle(c, {TransformHandle::Kind::Crop, -1, 0}, {0, 90}, {32, 90});
        QVERIFY(near(t.value(keys::CropL, 0), 32));
        t = dragHandle(c, {TransformHandle::Kind::Crop, 1, 1}, {320, 180}, {300, 150});
        QVERIFY(near(t.value(keys::CropR, 0), 20));
        QVERIFY(near(t.value(keys::CropB, 0), 30));
        ClipData cropped = c;
        cropped.transform.setBase(keys::CropR, 100);
        t = dragHandle(cropped, {TransformHandle::Kind::Crop, -1, 0}, {0, 90}, {400, 90});
        QVERIFY(near(t.value(keys::CropL, 0), 219)); // 320 - 100 - 1
        QVERIFY(near(t.value(keys::CropR, 0), 100));
    }

    // Hit-testing order: anchor, knob, corners, edges, else move (Transform) / none (Crop).
    void hitTesting()
    {
        const ClipData c = solid();
        const TransformPicture pic = tg::picture(c, 0, kFrame, nullptr);
        const QTransform id;
        QCOMPARE(tg::hitTest(pic, id, false, {160, 90}).kind, TransformHandle::Kind::Anchor);
        QCOMPARE(tg::hitTest(pic, id, false, tg::rotateKnob(pic, id)).kind,
            TransformHandle::Kind::Rotate);
        QCOMPARE(tg::hitTest(pic, id, false, {2, 2}),
            (TransformHandle {TransformHandle::Kind::Scale, -1, -1}));
        QCOMPARE(tg::hitTest(pic, id, false, {318, 90}),
            (TransformHandle {TransformHandle::Kind::Scale, 1, 0}));
        QCOMPARE(tg::hitTest(pic, id, false, {100, 60}).kind, TransformHandle::Kind::Move);
        QCOMPARE(tg::hitTest(pic, id, true, {100, 60}).kind, TransformHandle::Kind::None);
        QCOMPARE(tg::hitTest(pic, id, true, {160, 1}),
            (TransformHandle {TransformHandle::Kind::Crop, 0, -1}));
    }

    // Round 3 audit A2: with the audio part of a linked pair clicked, the controls show and
    // act on its video part.
    void linkedAudioPartShowsTheVideoControls()
    {
        Fixture f;
        TimelineData d = f.project.timeline()->data();
        ClipData& video = d.videoTracks[0].clips[0];
        video.linkGroup = 7;
        ClipData audio = video;
        audio.id = d.allocateId();
        audio.kind = ClipKind::Solid;
        d.audioTracks[0].clips.push_back(audio);
        f.project.timeline()->setData(d);
        f.selection.setClips({f.clipId, audio.id}); // the audio part clicked last
        ViewerPanel viewer(ViewerPanel::Mode::Timeline, f.ctx);
        viewer.resize(700, 500);
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        TransformOverlay* overlay = viewer.transformOverlay();
        f.action("view.onscreen_transform")->setChecked(true);
        QTRY_VERIFY(overlay->isEditing());
        const QPointF from = handleAt(overlay, 1, 1, false);
        const QPointF to = overlay->frameToWidget()->map(QPointF(400, 225));
        press(overlay, from);
        move(overlay, to);
        release(overlay, to);
        QVERIFY(std::abs(f.clip().transform.value(keys::ZoomX, 0) - 1.5) < 0.02);
        f.action("view.onscreen_transform")->setChecked(false);
    }

    // The real overlay: scale, rotate and crop drags are one named undo step each and
    // write keys at the playhead when the parameter is animated.
    void overlayDragsAreUndoStepsAndKeyframe()
    {
        Fixture f;
        ViewerPanel viewer(ViewerPanel::Mode::Timeline, f.ctx);
        viewer.resize(700, 500);
        viewer.show();
        QVERIFY(QTest::qWaitForWindowExposed(&viewer));
        TransformOverlay* overlay = viewer.transformOverlay();
        f.action("view.onscreen_transform")->setChecked(true);
        QTRY_VERIFY(overlay->isEditing());
        QVERIFY(!overlay->isCropMode());
        QUndoStack* undo = f.project.undoStack();

        // Bottom-right corner outwards: 1.5x.
        const auto map = *overlay->frameToWidget();
        QPointF from = handleAt(overlay, 1, 1, false);
        QPointF to = map.map(QPointF(400, 225));
        int steps = undo->count();
        press(overlay, from);
        move(overlay, (from + to) / 2);
        move(overlay, to);
        release(overlay, to);
        QCOMPARE(undo->count(), steps + 1);
        QCOMPARE(undo->undoText(), QStringLiteral("Scale Clip"));
        QVERIFY(std::abs(f.clip().transform.value(keys::ZoomX, 0) - 1.5) < 0.02);
        saveShot(viewer, QStringLiteral("transform-scale-rotate-handles"));
        undo->undo();

        // Rotation animated: the drag sets a key at the playhead (frame 40).
        f.project.beginEdit(QStringLiteral("Animate"));
        editops::clipById(f.project.timeline()->mutableData(), f.clipId)
            ->transform.animated(keys::Rotation, 0)
            .setKey(0, 0);
        f.project.endEdit();
        f.project.timeline()->setPlayhead(40);
        QCoreApplication::processEvents();
        from = tg::rotateKnob(*overlay->picture(), *overlay->frameToWidget());
        to = overlay->frameToWidget()->map(QPointF(400, 90));
        steps = undo->count();
        press(overlay, from);
        move(overlay, to);
        release(overlay, to);
        QCOMPARE(undo->count(), steps + 1);
        QCOMPARE(undo->undoText(), QStringLiteral("Rotate Clip"));
        const AnimatedValue& rot = f.clip().transform.values.value(keys::Rotation);
        QCOMPARE(rot.keys.size(), 2);
        QVERIFY(rot.hasKeyAt(40));
        QVERIFY(std::abs(rot.valueAt(40) - 90) < 1.0);
        QCOMPARE(rot.valueAt(0), 0.0);

        // Esc cancels a drag: nothing changes, no step.
        steps = undo->count();
        const QPointF corner = handleAt(overlay, 1, 1, false);
        press(overlay, corner);
        move(overlay, corner + QPointF(40, 40));
        QTest::keyClick(overlay, Qt::Key_Escape);
        QCOMPARE(undo->count(), steps);
        QVERIFY(std::abs(f.clip().transform.value(keys::ZoomX, 40) - 1.0) < 1e-9);

        // Crop mode from the on-screen controls menu; Transform turns off.
        auto* modes = viewer.findChild<QToolButton*>(QStringLiteral("ViewerOnScreenControls"));
        QVERIFY(modes);
        f.action("view.onscreen_crop")->setChecked(true);
        QVERIFY(!f.action("view.onscreen_transform")->isChecked());
        QCOMPARE(modes->text(), QStringLiteral("Crop"));
        QVERIFY(overlay->isCropMode());
        QVERIFY(overlay->isEditing());
        // Back to no rotation so the left edge is easy to aim at.
        undo->undo();
        QCoreApplication::processEvents();
        from = handleAt(overlay, -1, 0, true);
        to = from + QPointF(overlay->frameToWidget()->m11() * 40, 0); // 40 source px
        steps = undo->index(); // count() still holds the undone step
        press(overlay, from);
        move(overlay, to);
        release(overlay, to);
        QCOMPARE(undo->index(), steps + 1);
        QCOMPARE(undo->undoText(), QStringLiteral("Crop Clip"));
        QVERIFY(std::abs(f.clip().transform.value(keys::CropL, 40) - 40) <= 1);
        saveShot(viewer, QStringLiteral("transform-crop-handles"));
        // In Crop mode a press away from the handles is not a gesture.
        steps = undo->count();
        const QPointF inside = overlay->frameToWidget()->map(QPointF(200, 100));
        press(overlay, inside);
        move(overlay, inside + QPointF(20, 0));
        release(overlay, inside + QPointF(20, 0));
        QCOMPARE(undo->count(), steps);
        f.action("view.onscreen_crop")->setChecked(false);
        QTRY_VERIFY(!overlay->isEditing());
    }
};

QTEST_MAIN(TestTransformControls)
#include "test_transformcontrols.moc"
