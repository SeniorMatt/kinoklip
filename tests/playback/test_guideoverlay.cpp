// SPDX-License-Identifier: GPL-3.0-or-later
// Viewer guides in the real viewer (spec 41, offscreen raster surface): View menu commands,
// dragging guides out of the rulers, moving and removing them, the position dialog, lock,
// ruler mapping under zoom, snapping of on-screen transform drags, presets, the source
// viewer's own guides, that picture grabs never contain guides, safe areas or grid, and the
// viewer's wheel zoom / pan with everything over the picture following it.
#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/viewer/GuideOverlay.h"
#include "ui/viewer/TransformOverlay.h"
#include "ui/viewer/ViewerOverlaySettings.h"
#include "ui/viewer/ViewerPanel.h"
#include "ui/viewer/ViewerRuler.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>
#include <QWheelEvent>

#include <cmath>

using namespace kk;

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 180;

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
        ViewerOverlaySettings::instance().connectActions(&actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        TimelineData tl = makeDefaultTimeline(QStringLiteral("Demo"), {25, 1}, kWidth, kHeight, 2, 2);
        ClipData c;
        c.id = tl.allocateId();
        c.kind = ClipKind::Solid;
        c.duration = 100;
        c.solidColor = QColor(0x30, 0x60, 0x90);
        tl.videoTracks[0].clips.push_back(c);
        project.timeline()->setData(tl);
        clipId = c.id;
        selection.setClips({clipId});
    }
    QAction* action(const char* id) { return actions.action(QLatin1String(id)); }
    [[nodiscard]] const std::vector<ViewerGuide>& guides() const
    {
        return project.timeline()->data().guides;
    }
    [[nodiscard]] const ClipData& clip() const
    {
        return *editops::clipById(project.timeline()->data(), clipId);
    }
};

void send(QWidget* w, QEvent::Type type, QPointF p, Qt::MouseButton button = Qt::LeftButton,
    Qt::KeyboardModifiers mods = {})
{
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton
        : type == QEvent::MouseMove && button == Qt::NoButton           ? Qt::NoButton
                                                                        : Qt::LeftButton;
    QMouseEvent e(type, p, w->mapToGlobal(p), type == QEvent::MouseMove ? Qt::NoButton : button,
        buttons, mods);
    QCoreApplication::sendEvent(w, &e);
}

/// Drags on @p w from @p from to @p to (widget px of @p w).
void drag(QWidget* w, QPointF from, QPointF to, Qt::KeyboardModifiers mods = {})
{
    send(w, QEvent::MouseButtonPress, from, Qt::LeftButton, mods);
    send(w, QEvent::MouseMove, (from + to) / 2, Qt::LeftButton, mods);
    send(w, QEvent::MouseMove, to, Qt::LeftButton, mods);
    send(w, QEvent::MouseButtonRelease, to, Qt::LeftButton, mods);
}

/// A shown viewer of @p f.
struct Viewer
{
    ViewerPanel panel;
    explicit Viewer(Fixture& f, ViewerPanel::Mode mode = ViewerPanel::Mode::Timeline)
        : panel(mode, f.ctx)
    {
        panel.resize(700, 500);
        panel.show();
    }
    GuideOverlay* overlay() const { return panel.guideOverlay(); }
    QWidget* surface() const { return panel.surface()->widget(); }
    /// Widget point of frame point @p frame.
    QPointF at(QPointF frame) const { return overlay()->frameToWidget().map(frame); }
};

void resetOptions(ViewerOverlayOptions o = {})
{
    ViewerOverlaySettings::instance().setOptions(o);
}

/// Accepts the next modal dialog after setting its position box to @p value in unit @p unit.
void answerPositionDialog(double value, int unit)
{
    QTimer::singleShot(0, [value, unit] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        dialog->findChild<QComboBox*>(QStringLiteral("GuideUnit"))->setCurrentIndex(unit);
        dialog->findChild<QDoubleSpinBox*>(QStringLiteral("GuidePosition"))->setValue(value);
        dialog->accept();
    });
}

/// Mouse event with an explicit button state (middle-button pans).
void sendButton(
    QWidget* w, QEvent::Type type, QPointF p, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, p, w->mapToGlobal(p), button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &e);
}

/// One wheel event of @p delta eighths of a degree at @p p on @p w.
void wheel(QWidget* w, QPointF p, int delta)
{
    QWheelEvent e(p, w->mapToGlobal(p), QPoint(), QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
        Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(w, &e);
}

} // namespace

class TestGuideOverlay : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        initResources();
        QSettings().clear();
        ViewerOverlaySettings::instance().load();
    }
    void init() { resetOptions(); }

    // 41.1: View > Guides > Add Horizontal / Vertical Guide at the centre; one undo step each.
    void addFromTheMenu()
    {
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        v.panel.activate();
        const int steps = f.project.undoStack()->count();
        f.action("view.guide_add_horizontal")->trigger();
        f.action("view.guide_add_vertical")->trigger();
        QCOMPARE(f.guides(),
            (std::vector<ViewerGuide> {
                {Qt::Horizontal, kHeight / 2.0}, {Qt::Vertical, kWidth / 2.0}}));
        QCOMPARE(f.project.undoStack()->count(), steps + 2);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Add Guide"));
        QVERIFY(f.project.isDirty());
        // Adding with Show Guides off turns it on (the new guide must be visible).
        f.action("view.guides_show")->setChecked(false);
        QVERIFY(!ViewerOverlaySettings::instance().options().showGuides);
        f.action("view.guide_add_vertical")->trigger();
        QVERIFY(ViewerOverlaySettings::instance().options().showGuides);
        QVERIFY(f.action("view.guides_show")->isChecked());
        f.action("view.guides_clear")->trigger();
        QVERIFY(f.guides().empty());
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Clear Guides"));
    }

    // 41.2 / 41.3: drag a guide out of a ruler, move it, drag it back onto the ruler or off
    // the picture to remove it; the cursor changes over a guide.
    void dragOutMoveAndRemove()
    {
        resetOptions({.showRulers = true});
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        ViewerRuler* top = v.panel.topRuler();
        ViewerRuler* left = v.panel.leftRuler();
        QVERIFY(top->isVisible() && left->isVisible());

        // Top ruler -> a horizontal guide where the mouse is released (frame y = 45).
        const QPointF target = v.at(QPointF(100, 45));
        const QPointF inRuler(top->width() / 2.0, top->height() / 2.0);
        const QPointF targetInRuler = top->mapFromGlobal(v.surface()->mapToGlobal(target));
        drag(top, inRuler, targetInRuler);
        QCOMPARE(f.guides(), (std::vector<ViewerGuide> {{Qt::Horizontal, 45}}));
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Add Guide"));
        // Released over the ruler again: nothing is added.
        drag(top, inRuler, inRuler + QPointF(10, 0));
        QCOMPARE(f.guides().size(), size_t(1));
        // Left ruler -> vertical guide at x = 200.
        const QPointF xTarget = left->mapFromGlobal(v.surface()->mapToGlobal(v.at({200, 90})));
        drag(left, QPointF(left->width() / 2.0, 60), xTarget);
        QCOMPARE(f.guides().back(), (ViewerGuide {Qt::Vertical, 200}));

        // Hover: split cursor over the horizontal guide.
        send(v.surface(), QEvent::MouseMove, v.at({150, 45}), Qt::NoButton);
        QCOMPARE(v.surface()->cursor().shape(), Qt::SplitVCursor);
        send(v.surface(), QEvent::MouseMove, v.at({150, 120}), Qt::NoButton);
        QCOMPARE(v.surface()->cursor().shape(), Qt::ArrowCursor);

        // Move it from 45 to 120.
        drag(v.surface(), v.at({150, 45}), v.at({150, 120}));
        QCOMPARE(f.guides()[0], (ViewerGuide {Qt::Horizontal, 120}));
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Move Guide"));
        // Esc during a drag cancels it.
        send(v.surface(), QEvent::MouseButtonPress, v.at({150, 120}));
        send(v.surface(), QEvent::MouseMove, v.at({150, 20}));
        QVERIFY(v.overlay()->isDragging());
        QTest::keyClick(v.overlay(), Qt::Key_Escape);
        QVERIFY(!v.overlay()->isDragging());
        send(v.surface(), QEvent::MouseButtonRelease, v.at({150, 20}));
        QCOMPARE(f.guides()[0].position, 120.0);
        // Out of the picture (above the top edge): removed.
        drag(v.surface(), v.at({150, 120}), v.at({150, -30}));
        QCOMPARE(f.guides().size(), size_t(1));
        QCOMPARE(f.guides()[0].orientation, Qt::Vertical);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Remove Guide"));
        // Back onto the left ruler: removed.
        send(v.surface(), QEvent::MouseButtonPress, v.at({200, 90}));
        v.overlay()->finishDrag(v.overlay()->mapFromGlobal(left->mapToGlobal(QPoint(5, 60))));
        QVERIFY(f.guides().empty());
    }

    // 41.18: Ctrl while dragging a guide snaps it (owner request on 0.4.0).
    void ctrlDragSnapsTheGuide()
    {
        resetOptions({.showRulers = true});
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        const QSizeF frame(v.overlay()->frameSize());
        const double third = frame.width() / 3;
        // A vertical guide dragged from the left ruler, released 2 frame px off the third.
        ViewerRuler* left = v.panel.leftRuler();
        const QPointF near = left->mapFromGlobal(
            v.surface()->mapToGlobal(v.at({third + 2, frame.height() / 2})));
        drag(left, QPointF(left->width() / 2.0, 60), near);
        QCOMPARE(f.guides().back().position, std::round(third + 2)); // no Ctrl: free
        // Ctrl: the dragged guide snaps onto the centre line.
        const double centreX = frame.width() / 2;
        drag(v.surface(), v.at({std::round(third + 2), 50}), v.at({centreX - 2, 50}),
            Qt::ControlModifier);
        QCOMPARE(f.guides().back().position, centreX);
        // ... and a horizontal one onto the title safe edge and onto another guide.
        v.overlay()->beginNewGuide(Qt::Horizontal, v.at({100, 30}));
        v.overlay()->dragTo(v.at({100, frame.height() * 0.1 + 1.5}), Qt::ControlModifier);
        v.overlay()->finishDrag(v.at({100, frame.height() * 0.1 + 1.5}));
        QCOMPARE(f.guides().back().position, frame.height() * 0.1);
        // Pressing Ctrl in the middle of a drag snaps at once; releasing it lets go.
        send(v.surface(), QEvent::MouseButtonPress, v.at({centreX, 50}));
        send(v.surface(), QEvent::MouseMove, v.at({centreX + 30, 50}));
        QCOMPARE(v.overlay()->draggedGuide()->position, std::round(centreX + 30));
        v.overlay()->dragTo(v.at({frame.width() * 2 / 3 - 1, 50}));
        QTest::keyPress(v.overlay(), Qt::Key_Control);
        QCOMPARE(v.overlay()->draggedGuide()->position, frame.width() * 2 / 3);
        QTest::keyRelease(v.overlay(), Qt::Key_Control);
        QCOMPARE(v.overlay()->draggedGuide()->position, std::round(frame.width() * 2 / 3 - 1));
        v.overlay()->finishDrag({}, false);
    }

    // 41.4: double-click opens the position dialog (px or %).
    void positionDialog()
    {
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        v.overlay()->addGuide(Qt::Vertical, 10);
        answerPositionDialog(25, 1); // 25 % of 320 px
        send(v.surface(), QEvent::MouseButtonDblClick, v.at({10, 90}));
        QCOMPARE(f.guides()[0].position, 80.0);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Set Guide Position"));
        answerPositionDialog(33.5, 0);
        v.overlay()->editGuidePosition(0);
        QCOMPARE(f.guides()[0].position, 33.5);
    }

    // 41.27: a press near a guide takes it (kGrabPx screen px), and the nearest guide wins, so
    // guides close together stay precise.
    void pressNearAGuideTakesTheNearest()
    {
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        v.overlay()->addGuide(Qt::Vertical, 100); // index 0
        v.overlay()->addGuide(Qt::Vertical, 104); // index 1
        v.overlay()->addGuide(Qt::Horizontal, 90); // index 2
        const QPointF first = v.at({100, 20});
        const QPointF second = v.at({104, 20});
        QVERIFY(second.x() - first.x() > 2);

        const double reach = GuideOverlay::kGrabPx;
        QCOMPARE(v.overlay()->guideAt(first - QPointF(reach - 1, 0)), 0);
        QCOMPARE(v.overlay()->guideAt(second + QPointF(reach - 1, 0)), 1);
        QCOMPARE(v.overlay()->guideAt(first - QPointF(reach + 1, 0)), -1);
        // Between two guides the closer one wins, either way.
        const double gap = second.x() - first.x();
        QCOMPARE(v.overlay()->guideAt(first + QPointF(gap * 0.25, 0)), 0);
        QCOMPARE(v.overlay()->guideAt(first + QPointF(gap * 0.75, 0)), 1);
        // Near a crossing, the axis the pointer is closer to wins.
        const QPointF crossing = v.at({100, 90});
        QCOMPARE(v.overlay()->guideAt(crossing + QPointF(-1, 5)), 0);
        QCOMPARE(v.overlay()->guideAt(crossing + QPointF(-5, 1)), 2);
    }

    // 41.6: Lock Guides stops every mouse edit; hidden guides are not hit.
    void lockAndHide()
    {
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        v.overlay()->addGuide(Qt::Horizontal, 90);
        f.action("view.guides_lock")->setChecked(true);
        QVERIFY(ViewerOverlaySettings::instance().options().lockGuides);
        QCOMPARE(v.overlay()->guideAt(v.at({100, 90})), -1);
        QCOMPARE(v.overlay()->guideAt(v.at({100, 90}), false), 0); // context menu still finds it
        drag(v.surface(), v.at({100, 90}), v.at({100, 30}));
        QCOMPARE(f.guides()[0].position, 90.0);
        v.overlay()->beginNewGuide(Qt::Vertical, v.at({50, 50}));
        QVERIFY(!v.overlay()->isDragging());
        f.action("view.guides_lock")->setChecked(false);
        f.action("view.guides_show")->setChecked(false);
        QCOMPARE(v.overlay()->guideAt(v.at({100, 90})), -1);
        QVERIFY(v.overlay()->snapTargets(Qt::Horizontal).empty());
        f.action("view.guides_show")->setChecked(true);
        QCOMPARE(v.overlay()->guideAt(v.at({100, 90})), 0);
    }

    // 41.5: ruler units are frame px; the scale follows Fit and the zoom levels.
    void rulerMappingUnderZoom()
    {
        resetOptions({.showRulers = true});
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        ViewerRuler* top = v.panel.topRuler();
        ViewerRuler* left = v.panel.leftRuler();
        for (const auto zoom : {ViewerPanel::Zoom::Fit, ViewerPanel::Zoom::Percent50,
                 ViewerPanel::Zoom::Percent100, ViewerPanel::Zoom::Percent200}) {
            v.panel.setZoom(zoom);
            QCoreApplication::processEvents();
            const QRect pic = v.panel.surface()->pictureRect();
            // Frame 0 and the far edge sit exactly beside the picture's edges.
            const QPoint picTopLeft = top->mapFromGlobal(v.surface()->mapToGlobal(pic.topLeft()));
            QVERIFY(std::abs(top->toRuler(0) - picTopLeft.x()) < 0.51);
            QVERIFY(std::abs(top->toRuler(kWidth) - (picTopLeft.x() + pic.width())) < 0.51);
            const QPoint picLeft = left->mapFromGlobal(v.surface()->mapToGlobal(pic.topLeft()));
            QVERIFY(std::abs(left->toRuler(0) - picLeft.y()) < 0.51);
            QVERIFY(std::abs(left->toRuler(kHeight) - (picLeft.y() + pic.height())) < 0.51);
            QVERIFY(std::abs(top->toFrame(top->toRuler(123)) - 123) < 1e-6);
            const double scale = top->toRuler(1) - top->toRuler(0);
            if (zoom == ViewerPanel::Zoom::Percent200)
                QVERIFY(std::abs(scale - 2.0 / v.surface()->devicePixelRatioF()) < 1e-6);
            if (zoom == ViewerPanel::Zoom::Percent100)
                QVERIFY(std::abs(scale - 1.0 / v.surface()->devicePixelRatioF()) < 1e-6);
        }
        // Rulers off: hidden.
        f.action("view.show_rulers")->setChecked(false);
        QVERIFY(!top->isVisible() && !left->isVisible());
    }

    // 41.9: an on-screen Move drag snaps the picture's edges / centre onto guides within a
    // few screen px; Snap to Guides off or Ctrl held: no snap. Anchor and crop snap too.
    void transformDragSnapsToGuides()
    {
        Fixture f;
        // Half size picture: frame x 80 .. 240.
        f.project.beginEdit(QStringLiteral("Zoom"));
        ClipData* c = editops::clipById(f.project.timeline()->mutableData(), f.clipId);
        c->transform.values[keys::ZoomX].setValue(0, 0.5);
        c->transform.values[keys::ZoomY].setValue(0, 0.5);
        f.project.endEdit();
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        v.panel.setZoom(ViewerPanel::Zoom::Percent100); // 1 widget px per frame px
        f.action("view.onscreen_transform")->setChecked(true);
        TransformOverlay* t = v.panel.transformOverlay();
        QTRY_VERIFY(t->isEditing());
        v.overlay()->addGuide(Qt::Vertical, 100);
        const QPointF from = v.at({160, 60});

        // Left edge 80 -> 98 without snap would give posX 18; the guide at 100 pulls it to 20.
        drag(t, from, from + QPointF(18, 0));
        QVERIFY(std::abs(f.clip().transform.value(keys::PosX, 0) - 20) < 1e-6);
        f.project.undoStack()->undo();
        // Ctrl: free.
        drag(t, from, from + QPointF(18, 0), Qt::ControlModifier);
        QVERIFY(std::abs(f.clip().transform.value(keys::PosX, 0) - 18) < 1e-6);
        f.project.undoStack()->undo();
        // Far from any guide: free.
        drag(t, from, from + QPointF(40, 0));
        QVERIFY(std::abs(f.clip().transform.value(keys::PosX, 0) - 40) < 1e-6);
        f.project.undoStack()->undo();
        // Snap to Guides off: free.
        f.action("view.guides_snap")->setChecked(false);
        drag(t, from, from + QPointF(18, 0));
        QVERIFY(std::abs(f.clip().transform.value(keys::PosX, 0) - 18) < 1e-6);
        f.project.undoStack()->undo();
        f.action("view.guides_snap")->setChecked(true);
        // The centre snaps too: picture y 45 .. 135 (centre 90), a horizontal guide at 100,
        // dragged down 8 -> centre 98 -> pulled to 100 (Position Y 10).
        v.overlay()->addGuide(Qt::Horizontal, 100);
        drag(t, from, from + QPointF(0, 8));
        QVERIFY(std::abs(f.clip().transform.value(keys::PosY, 0) - 10) < 1e-6);
        f.project.undoStack()->undo();

        // Crop: the left crop edge (80) dragged to 97 lands on 100.
        f.action("view.onscreen_crop")->setChecked(true);
        QTRY_VERIFY(t->isCropMode());
        const QPointF leftEdge = v.at({80, 90});
        drag(t, leftEdge, leftEdge + QPointF(17, 0));
        // Crop Left is in source px (the picture is drawn at half size: 2 source px per px).
        const auto pic = t->picture();
        QVERIFY(pic);
        const QRectF box = pic->contentToFrame.map(QPolygonF(pic->crop)).boundingRect();
        QVERIFY2(std::abs(box.left() - 100) < 1e-6, qPrintable(QString::number(box.left())));
        f.project.undoStack()->undo();

        // Anchor: the cross (picture centre 160, 90) dragged to 104, 97 lands on 100, 100.
        f.action("view.onscreen_transform")->setChecked(true);
        QTRY_VERIFY(t->isEditing() && !t->isCropMode());
        drag(t, v.at({160, 90}), v.at({104, 97}));
        const auto moved = t->picture();
        QVERIFY(moved);
        const QPointF anchor = moved->contentToFrame.map(moved->anchor);
        QVERIFY2(std::abs(anchor.x() - 100) < 1e-6 && std::abs(anchor.y() - 100) < 1e-6,
            qPrintable(QStringLiteral("%1 %2").arg(anchor.x()).arg(anchor.y())));
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Move Anchor Point"));
    }

    // 41.7: presets replace the guides (one undo step); user presets are saved and deleted.
    void presetsApplyAndSave()
    {
        QTemporaryDir dir;
        ViewerOverlaySettings& s = ViewerOverlaySettings::instance();
        s.setPresetPath(dir.filePath(QStringLiteral("guide-presets.json")));
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        v.overlay()->applyPreset(guides::builtInPresets()[1]); // thirds
        QCOMPARE(f.guides().size(), size_t(4));
        QCOMPARE(f.guides()[0].position, 60.0);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Load Guide Preset"));
        QVERIFY(s.saveUserPreset(guides::makeUserPreset(
            QStringLiteral("Mine"), {{Qt::Vertical, 32}}, QSizeF(kWidth, kHeight))));
        QVERIFY(s.saveUserPreset( // same name: replaced, not duplicated
            guides::makeUserPreset(
                QStringLiteral("Mine"), {{Qt::Vertical, 64}}, QSizeF(kWidth, kHeight))));
        const auto user = guides::loadUserPresets(s.presetPath());
        QCOMPARE(user.size(), size_t(1));
        v.overlay()->applyPreset(user[0]);
        QCOMPARE(f.guides(), (std::vector<ViewerGuide> {{Qt::Vertical, 64}}));
        QVERIFY(s.deleteUserPreset(QStringLiteral("Mine")));
        QVERIFY(guides::loadUserPresets(s.presetPath()).empty());
        s.setPresetPath({});
    }

    // 41.13 / 41.14: safe areas, aspect marker and grid follow the actions and settings.
    void safeAreaAndGridActions()
    {
        Fixture f;
        f.action("view.safe_action")->setChecked(true);
        f.action("view.safe_title")->setChecked(true);
        f.action("view.safe_centre")->setChecked(true);
        f.action("view.aspect_9_16")->trigger();
        f.action("view.grid_quarters")->trigger();
        const ViewerOverlayOptions& o = ViewerOverlaySettings::instance().options();
        QVERIFY(o.actionSafe && o.titleSafe && o.centreMark);
        QVERIFY(std::abs(o.aspectMarker - 9.0 / 16.0) < 1e-9);
        QVERIFY(o.showGrid); // choosing a grid type shows the grid
        QCOMPARE(o.gridDivisions, 4);
        QVERIFY(f.action("view.grid_show")->isChecked());
        QVERIFY(f.action("view.grid_quarters")->isChecked());
        QVERIFY(!f.action("view.grid_thirds")->isChecked());
        // Stored per user: a reload gives the same options.
        const ViewerOverlayOptions before = o;
        ViewerOverlaySettings::instance().load();
        QCOMPARE(ViewerOverlaySettings::instance().options(), before);
        // 41.7: the guide colour, default from the scheme, stored per user.
        QVERIFY(ViewerOverlaySettings::instance().guideColor().isValid());
        ViewerOverlayOptions red = ViewerOverlaySettings::instance().options();
        red.guideColor = QColor(Qt::red);
        ViewerOverlaySettings::instance().setOptions(red);
        ViewerOverlaySettings::instance().load();
        QCOMPARE(ViewerOverlaySettings::instance().guideColor(), QColor(Qt::red));
        f.action("view.aspect_none")->trigger();
        QCOMPARE(ViewerOverlaySettings::instance().options().aspectMarker, 0.0);
    }

    // 41.10: the source viewer has its own guides (per user), in px of the clip shown.
    void sourceViewerGuides()
    {
        Fixture f;
        Viewer v(f, ViewerPanel::Mode::Source);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        GuideOverlay* o = v.overlay();
        QVERIFY(o && !o->isTimeline());
        // No clip loaded: nothing to place guides on (and nothing is drawn).
        QVERIFY(o->frameSize().isEmpty());
        o->addGuide(Qt::Horizontal);
        QVERIFY(o->storedGuides().empty());
        o->setFrameSize(QSize(1280, 720));
        o->addGuide(Qt::Horizontal);
        QCOMPARE(o->storedGuides(), (std::vector<ViewerGuide> {{Qt::Horizontal, 360}}));
        QVERIFY(f.guides().empty()); // not the timeline's
        Viewer again(f, ViewerPanel::Mode::Source); // remembered per user
        QCOMPARE(again.overlay()->storedGuides(), o->storedGuides());
        o->clearGuides();
    }

    // 41.19: the wheel zooms in smooth steps about the cursor: the frame point under it stays.
    void wheelZoomsAboutCursor()
    {
        resetOptions({.showRulers = true});
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        auto* box = v.panel.findChild<QComboBox*>(QStringLiteral("ViewerZoom"));
        const QPointF frame(80, 45);
        const QPointF cursor = v.at(frame);
        const double fit = v.panel.effectiveZoom();
        QCOMPARE(v.panel.zoomFactor(), 0.0);
        // Over a child overlay too (the transform layer covers the picture).
        QWidget* target = v.panel.transformOverlay() && v.panel.transformOverlay()->isVisible()
            ? static_cast<QWidget*>(v.panel.transformOverlay())
            : v.surface();
        const QPointF local = target == v.surface() ? cursor : target->mapFrom(v.surface(), cursor);
        wheel(target, local, 120);
        QVERIFY(std::abs(v.panel.zoomFactor() / fit - 1.125) < 1e-6);
        QCOMPARE(v.panel.zoom(), ViewerPanel::Zoom::Custom);
        QCOMPARE(box->currentText(),
            QStringLiteral("%1%").arg(int(std::lround(v.panel.zoomFactor() * 100))));
        for (int i = 0; i < 12; ++i) {
            wheel(v.surface(), cursor, 120);
        }
        // One widget px of rounding at most.
        const QPointF back = v.overlay()->frameToWidget().map(frame);
        QVERIFY2((back - cursor).manhattanLength() < 2.0,
            qPrintable(QStringLiteral("%1,%2 vs %3,%4")
                           .arg(back.x())
                           .arg(back.y())
                           .arg(cursor.x())
                           .arg(cursor.y())));
        // Rulers follow: frame 0 sits beside the picture's left edge.
        const QRect pic = v.panel.surface()->pictureRect();
        const QPoint picTopLeft
            = v.panel.topRuler()->mapFromGlobal(v.surface()->mapToGlobal(pic.topLeft()));
        QVERIFY(std::abs(v.panel.topRuler()->toRuler(0) - picTopLeft.x()) < 0.51);
        // Out again, below Fit, down to the 10 % floor.
        for (int i = 0; i < 60; ++i)
            wheel(v.surface(), cursor, -120);
        QVERIFY(std::abs(v.panel.zoomFactor() - ViewerPicture::kMinZoom) < 1e-9);
        // In to the 1600 % ceiling.
        for (int i = 0; i < 80; ++i)
            wheel(v.surface(), cursor, 120);
        QVERIFY(std::abs(v.panel.zoomFactor() - ViewerPicture::kMaxZoom) < 1e-9);
        // A trackpad's small delta is a proportional step.
        v.panel.setZoom(ViewerPanel::Zoom::Percent100);
        QCOMPARE(box->currentIndex(), int(ViewerPanel::Zoom::Percent100));
        QCOMPARE(box->count(), int(ViewerPanel::Zoom::Custom)); // the "137%" item is gone
        wheel(v.surface(), cursor, 12);
        QVERIFY(std::abs(v.panel.zoomFactor() - std::pow(1.125, 0.1)) < 1e-6);
    }

    // 41.20 / 41.23: middle drag pans; guides, rulers and the transform overlay follow.
    // 41.28: a middle click puts two guides crossing there, one undo step; a drag pans and a
    // double-click fits, neither adds a guide.
    void middleClickAddsCrossingGuides()
    {
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        const int undoBefore = f.project.undoStack()->count();
        const QPoint point = v.at({100, 60}).toPoint();
        const QPointF frame = v.overlay()->frameToWidget().inverted().map(QPointF(point));
        QTest::mouseClick(v.surface(), Qt::MiddleButton, {}, point);
        QTRY_COMPARE(v.overlay()->guides().size(), size_t(2));
        const auto list = v.overlay()->guides();
        QCOMPARE(list[0].orientation, Qt::Horizontal);
        QVERIFY(std::abs(list[0].position - frame.y()) < 1e-6);
        QCOMPARE(list[1].orientation, Qt::Vertical);
        QVERIFY(std::abs(list[1].position - frame.x()) < 1e-6);
        QCOMPARE(f.project.undoStack()->count(), undoBefore + 1);
        f.project.undoStack()->undo();
        QVERIFY(v.overlay()->guides().empty());

        QTest::mouseDClick(v.surface(), Qt::MiddleButton, {}, point);
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        QVERIFY(v.overlay()->guides().empty());

        QTest::mousePress(v.surface(), Qt::MiddleButton, {}, point);
        QTest::mouseMove(v.surface(), point + QPoint(30, 0));
        QTest::mouseRelease(v.surface(), Qt::MiddleButton, {}, point + QPoint(30, 0));
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        QVERIFY(v.overlay()->guides().empty());

        f.action("view.guides_lock")->setChecked(true);
        QTest::mouseClick(v.surface(), Qt::MiddleButton, {}, point);
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        QVERIFY(v.overlay()->guides().empty());
    }

    void middleDragPans()
    {
        resetOptions({.showRulers = true});
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        v.overlay()->addGuide(Qt::Vertical, 77);
        // At Fit there is nothing to pan.
        const QRect fitPic = v.panel.surface()->pictureRect();
        sendButton(
            v.surface(), QEvent::MouseButtonPress, {50, 50}, Qt::MiddleButton, Qt::MiddleButton);
        sendButton(v.surface(), QEvent::MouseMove, {80, 70}, Qt::NoButton, Qt::MiddleButton);
        sendButton(
            v.surface(), QEvent::MouseButtonRelease, {80, 70}, Qt::MiddleButton, Qt::NoButton);
        QCOMPARE(v.panel.surface()->pictureRect(), fitPic);

        v.panel.setZoom(ViewerPanel::Zoom::Percent200);
        const QRect before = v.panel.surface()->pictureRect();
        const QPointF guideBefore = v.at({77, 0});
        const double rulerBefore = v.panel.topRuler()->toRuler(77);
        sendButton(
            v.surface(), QEvent::MouseButtonPress, {50, 50}, Qt::MiddleButton, Qt::MiddleButton);
        sendButton(v.surface(), QEvent::MouseMove, {65, 60}, Qt::NoButton, Qt::MiddleButton);
        sendButton(v.surface(), QEvent::MouseMove, {80, 70}, Qt::NoButton, Qt::MiddleButton);
        sendButton(
            v.surface(), QEvent::MouseButtonRelease, {80, 70}, Qt::MiddleButton, Qt::NoButton);
        QCOMPARE(v.panel.surface()->pictureRect(), before.translated(30, 20));
        QCOMPARE(v.panel.pan(), QPointF(30, 20));
        QCOMPARE(v.panel.zoom(), ViewerPanel::Zoom::Percent200); // panning keeps the zoom
        QCOMPARE(v.at({77, 0}), guideBefore + QPointF(30, 20));
        QVERIFY(std::abs(v.panel.topRuler()->toRuler(77) - (rulerBefore + 30)) < 0.51);
        // The same mapping drives the guide hit test.
        QCOMPARE(v.overlay()->guideAt(v.at({77, 90})), 0);
        // A drag far away keeps part of the picture in view.
        v.panel.setPan({1e6, -1e6});
        const QRect far = v.panel.surface()->pictureRect();
        QVERIFY(far.intersects(v.surface()->rect()));
        QVERIFY(far.intersected(v.surface()->rect()).width() >= 40);
        // No guide was moved by the middle button.
        QCOMPARE(f.guides().size(), size_t(1));
        QCOMPARE(f.guides()[0].position, 77.0);
    }

    // 41.21: Shift+Z (focus in the viewer), the header's Fit and a middle double-click fit.
    void zKeyFitsTheViewerUnderThePointer()
    {
        // Owner request on 0.4.1: Z resets the viewer's zoom and pan (View > Viewer Zoom to Fit).
        resetOptions();
        Fixture f;
        Viewer a(f);
        Viewer b(f, ViewerPanel::Mode::Source);
        a.panel.move(0, 0);
        b.panel.move(720, 0);
        QVERIFY(QTest::qWaitForWindowExposed(&a.panel));
        QVERIFY(QTest::qWaitForWindowExposed(&b.panel));
        wheel(a.surface(), {60, 60}, 360);
        wheel(b.surface(), {60, 60}, 360);
        QVERIFY(a.panel.zoomFactor() > 0.0 && b.panel.zoomFactor() > 0.0);
        // The viewer with the focus fits (the pointer is over neither here).
        QCursor::setPos(QPoint(-50, -50));
        b.panel.activateWindow();
        b.surface()->setFocus();
        QVERIFY(QApplication::widgetAt(QCursor::pos()) == nullptr);
        QTRY_VERIFY(b.surface()->hasFocus());
        ViewerPanel::zoomTargetToFit();
        QCOMPARE(b.panel.zoom(), ViewerPanel::Zoom::Fit);
        QVERIFY(a.panel.zoomFactor() > 0.0);
        // No viewer under the pointer or with focus: every visible viewer fits.
        wheel(b.surface(), {60, 60}, 360);
        b.surface()->clearFocus();
        QWidget other;
        other.show();
        other.activateWindow();
        other.setFocus();
        QTRY_VERIFY(!b.surface()->hasFocus());
        ViewerPanel::zoomTargetToFit();
        QCOMPARE(a.panel.zoom(), ViewerPanel::Zoom::Fit);
        QCOMPARE(b.panel.zoom(), ViewerPanel::Zoom::Fit);
        // The action is bound to Z.
        QVERIFY(f.ctx.actions->shortcuts(QStringLiteral("view.viewer_zoom_fit"))
                    .contains(QKeySequence(Qt::Key_Z)));
    }

    void fitResets()
    {
        resetOptions();
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        auto* box = v.panel.findChild<QComboBox*>(QStringLiteral("ViewerZoom"));
        const QRect fitPic = v.panel.surface()->pictureRect();
        auto zoomAndPan = [&] {
            wheel(v.surface(), {60, 60}, 360);
            v.panel.setPan({25, -15});
            QVERIFY(v.panel.zoomFactor() > 0.0);
            QVERIFY(v.panel.surface()->pictureRect() != fitPic);
        };
        zoomAndPan();
        v.panel.activateWindow();
        v.surface()->setFocus();
        QVERIFY(v.surface()->hasFocus());
        v.panel.addAction(f.action("timeline.zoom_fit")); // Shift+Z is live in this window
        int timelineFits = 0;
        connect(f.action("timeline.zoom_fit"), &QAction::triggered, this,
            [&timelineFits] { ++timelineFits; });
        QTest::keyClick(v.surface(), Qt::Key_Z, Qt::ShiftModifier);
        QCOMPARE(v.panel.zoom(), ViewerPanel::Zoom::Fit);
        QCOMPARE(v.panel.pan(), QPointF());
        QCOMPARE(v.panel.surface()->pictureRect(), fitPic);
        QCOMPARE(timelineFits, 0); // the viewer took the key
        QCOMPARE(box->currentIndex(), int(ViewerPanel::Zoom::Fit));

        zoomAndPan();
        box->setCurrentIndex(int(ViewerPanel::Zoom::Fit));
        QCOMPARE(v.panel.surface()->pictureRect(), fitPic);

        zoomAndPan();
        sendButton(
            v.surface(), QEvent::MouseButtonDblClick, {60, 60}, Qt::MiddleButton, Qt::MiddleButton);
        QCOMPARE(v.panel.surface()->pictureRect(), fitPic);
        QCOMPARE(v.panel.pan(), QPointF());
    }

    // 41.20: Space held over the zoomed picture makes the left button pan; a tap still plays.
    void spaceHandPans()
    {
        resetOptions();
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        QAction* play = f.action("transport.play_pause");
        QVERIFY(play);
        v.panel.addAction(play); // its Space shortcut is live in this window
        int plays = 0;
        connect(play, &QAction::triggered, this, [&plays] { ++plays; });
        v.panel.setZoom(ViewerPanel::Zoom::Percent200);
        const QPoint inside(v.surface()->width() / 2, v.surface()->height() / 2);
        QCursor::setPos(v.surface()->mapToGlobal(inside));
        if (!v.surface()->rect().contains(v.surface()->mapFromGlobal(QCursor::pos())))
            QSKIP("the platform cannot move the cursor");
        v.surface()->setFocus();
        const QRect before = v.panel.surface()->pictureRect();
        QTest::keyPress(v.surface(), Qt::Key_Space);
        QCOMPARE(plays, 0); // held: no play yet
        sendButton(
            v.surface(), QEvent::MouseButtonPress, {100, 100}, Qt::LeftButton, Qt::LeftButton);
        sendButton(v.surface(), QEvent::MouseMove, {90, 110}, Qt::NoButton, Qt::LeftButton);
        sendButton(
            v.surface(), QEvent::MouseButtonRelease, {90, 110}, Qt::LeftButton, Qt::NoButton);
        QTest::keyRelease(v.surface(), Qt::Key_Space);
        QCOMPARE(v.panel.surface()->pictureRect(), before.translated(-10, 10));
        QCOMPARE(plays, 0); // used as a hand: no play
        QCOMPARE(f.clip().transform.value(keys::PosX, 0), 0.0); // the clip was not moved
        // A tap plays (on release).
        QTest::keyClick(v.surface(), Qt::Key_Space);
        QCOMPARE(plays, 1);
        // At Fit, Space plays at once as before.
        v.panel.zoomToFit();
        QTest::keyPress(v.surface(), Qt::Key_Space);
        QCOMPARE(plays, 2);
        QTest::keyRelease(v.surface(), Qt::Key_Space);
        QCOMPARE(plays, 2);
    }

    // 41.12: guides, safe areas and grid are drawn over the picture but never part of a
    // picture grab: GuideOverlay::setSuppressed() (source drag pixmap, --screenshot) hides
    // them, and the frame the viewer shows is the compositor's, untouched.
    void grabsContainNoGuides()
    {
        resetOptions({.actionSafe = true,
            .titleSafe = true,
            .centreMark = true,
            .aspectMarker = 2.39,
            .showGrid = true});
        Fixture f;
        Viewer v(f);
        QVERIFY(QTest::qWaitForWindowExposed(&v.panel));
        QImage frame(kWidth, kHeight, QImage::Format_ARGB32_Premultiplied);
        frame.fill(QColor(0x30, 0x60, 0x90));
        v.panel.showFrame(frame);
        v.panel.setZoom(ViewerPanel::Zoom::Percent100);
        v.overlay()->addGuide(Qt::Horizontal, 33);
        v.overlay()->addGuide(Qt::Vertical, 77);
        QCoreApplication::processEvents();
        const QRect pic = v.panel.surface()->pictureRect();
        const QImage with = v.surface()->grab(pic).toImage();
        GuideOverlay::setSuppressed(true);
        QVERIFY(!v.overlay()->isVisible());
        const QImage without = v.surface()->grab(pic).toImage();
        GuideOverlay::setSuppressed(false);
        QVERIFY(v.overlay()->isVisible());
        QVERIFY(with != without); // the overlay draws when shown...
        // ...and without it the grab is the plain frame, pixel for pixel.
        const QImage plain = frame.convertToFormat(without.format());
        QCOMPARE(without.size(), plain.size());
        QCOMPARE(without, plain);
    }
};

QTEST_MAIN(TestGuideOverlay)
#include "test_guideoverlay.moc"
