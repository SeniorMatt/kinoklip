// SPDX-License-Identifier: GPL-3.0-or-later
// Viewer guides in the real viewer (spec 41, offscreen raster surface): View menu commands,
// dragging guides out of the rulers, moving and removing them, the position dialog, lock,
// ruler mapping under zoom, snapping of on-screen transform drags, presets, the source
// viewer's own guides, and that picture grabs never contain guides, safe areas or grid.
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
#include <QDialog>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

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
        TimelineData tl = makeDefaultTimeline(QStringLiteral("Demo"), {25, 1}, kWidth, kHeight);
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
