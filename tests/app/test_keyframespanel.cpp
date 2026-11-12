// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the Keyframes panel (docs/behavior-spec.md 15.14 - 15.19): what it shows,
// edits through undo, playhead sync, linked pairs, and docking / floating in the Edit page.
#include "../core/ClassicTimeline.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/EditPage.h"
#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/StandardActions.h"
#include "ui/inspector/KeyframeButton.h"
#include "ui/inspector/ValueField.h"
#include "ui/keyframes/CurveView.h"
#include "ui/keyframes/DopeSheetView.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/keyframes/KeyframeParamTree.h"
#include "ui/keyframes/KeyframeRuler.h"
#include "ui/keyframes/KeyframesPanel.h"
#include "ui/timeline/KeyframeLanes.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QLabel>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QUndoStack>

#include <memory>

using namespace kk;
namespace ko = kk::keyframeops;

namespace {
constexpr TrackRef kV1 {TrackType::Video, 0};
constexpr TrackRef kA1 {TrackType::Audio, 0};
const QString kPosX = QStringLiteral("transform/posX");
const QString kOpacity = QStringLiteral("transform/opacity");
const QString kVolume = QStringLiteral("audio/volumeDb");

/// Project + context; optionally a main window (docking tests).
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    quint64 video = 0;
    quint64 audio = 0;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        kk::test::useClassicTimeline(project); // the tests count frames at 25 fps
        // A 100-frame clip at frame 50: posX keys 0 / 40 (0 -> 100), opacity key 40 (50 %);
        // linked to an audio clip with volume keys 0 / 60.
        ClipData v;
        v.name = QStringLiteral("Shot");
        v.mediaId = QStringLiteral("m1");
        v.start = 50;
        v.duration = 100;
        v.transform.animated(keys::PosX, 0).setKey(0, 0.0);
        v.transform.animated(keys::PosX, 0).setKey(40, 100.0);
        v.transform.animated(keys::Opacity, 100).setKey(40, 50.0);
        ClipData a = v;
        a.transform = {};
        a.audio.animated(keys::VolumeDb, 0).setKey(0, -20.0);
        a.audio.animated(keys::VolumeDb, 0).setKey(60, 0.0);
        project.beginEdit(QStringLiteral("setup"));
        TimelineData& d = project.timeline()->mutableData();
        video = editops::overwriteClip(d, kV1, v);
        audio = editops::overwriteClip(d, kA1, a);
        editops::linkClips(d, {video, audio});
        project.endEdit();
        project.undoStack()->clear();
        keyframecommands::clearClipboard();
    }

    void makeWindow()
    {
        MainWindowOptions options;
        options.autosave = false;
        window = std::make_unique<MainWindow>(ctx, options);
        window->resize(1600, 1000);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    }

    const ClipData& clip(quint64 id) const
    {
        return *editops::clipById(project.timeline()->data(), id);
    }
    const AnimatedValue& value(quint64 id, const QString& path) const
    {
        return *ko::findValue(clip(id), path);
    }
    int undoCount() const { return project.undoStack()->count(); }
};

/// A shown standalone panel.
struct PanelView
{
    KeyframesPanel panel;
    explicit PanelView(AppContext& ctx)
        : panel(ctx)
    {
        panel.resize(1400, 600);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
    }
    int rowOf(const QString& label) const
    {
        for (int i = 0; i < panel.tree()->rowCount(); ++i) {
            if (panel.tree()->entryAt(i).label == label)
                return i;
        }
        return -1;
    }
};

void drag(QWidget* w, QPoint from, QPoint to, Qt::KeyboardModifiers mods = {})
{
    QTest::mousePress(w, Qt::LeftButton, mods, from);
    for (int i = 1; i <= 4; ++i)
        QTest::mouseMove(w, from + (to - from) * i / 4);
    QTest::mouseRelease(w, Qt::LeftButton, mods, to);
}
} // namespace

class TestKeyframesPanel : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_keyframespanel"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // 15.14: the panel lists the selected clip's animated parameters, grouped like the
    // inspector; the dope sheet has the same rows; every parameter is a curve.
    void spec15_14_showsAnimatedParams()
    {
        Fixture f;
        PanelView v(f.ctx);
        QCOMPARE(v.panel.tree()->rowCount(), 0); // nothing selected
        f.selection.selectClip(f.video);
        // Keyframes, Transform, Position X, Composite, Opacity, Audio, Volume (linked audio
        // member).
        QCOMPARE(v.panel.tree()->rowCount(), 7);
        QCOMPARE(v.panel.tree()->entryAt(0).kind, KeyRow::Kind::Master);
        QVERIFY(v.rowOf(QStringLiteral("Position X")) > 0);
        QVERIFY(v.rowOf(QStringLiteral("Opacity")) > 0);
        QCOMPARE(v.panel.dopeSheet()->rows().size(), 7);
        QCOMPARE(v.panel.curveView()->curves().size(), 3);
        QCOMPARE(
            v.panel.model().firstItemName(f.project.timeline()->data()), QStringLiteral("Shot"));
        // Show All lists every animatable parameter (Zoom, Rotation, Cropping, Pan, ...).
        v.panel.setShowAllParameters(true);
        QVERIFY(v.rowOf(QStringLiteral("Rotation")) > 0);
        QVERIFY(v.rowOf(QStringLiteral("Crop Left")) > 0);
        v.panel.setShowAllParameters(false);
        QCOMPARE(v.panel.tree()->rowCount(), 7);
        // Collapsing a group hides its rows in the tree and the dope sheet alike.
        const int transform = v.rowOf(QStringLiteral("Transform"));
        emit v.panel.tree()->collapseToggled(v.panel.tree()->entryAt(transform).key);
        QCOMPARE(v.panel.tree()->rowCount(), 6);
        QCOMPARE(v.panel.dopeSheet()->rows().size(), 6);
    }

    // 15.15 / spec 24.1: a linked pair is one item whichever part is selected; the video
    // member's parameters come first.
    void spec15_15_linkedPairIsOneItem()
    {
        Fixture f;
        PanelView v(f.ctx);
        f.selection.selectClip(f.audio);
        QCOMPARE(v.panel.model().items.size(), 1);
        QCOMPARE(v.panel.model().items.front().front(), f.video);
        QCOMPARE(v.panel.tree()->entryAt(1).label, QStringLiteral("Transform"));
        QVERIFY(v.rowOf(QStringLiteral("Volume")) > 0);
        f.selection.setClips({f.video, f.audio}); // linked selection: still one item
        QCOMPARE(v.panel.model().items.size(), 1);
    }

    // 15.16: value and diamond edits in the tree are one undo step each.
    void spec15_16_treeEditsRoundTripThroughUndo()
    {
        Fixture f;
        PanelView v(f.ctx);
        f.selection.selectClip(f.video);
        f.project.timeline()->setPlayhead(50 + 40); // on the posX key at 40
        const int posX = v.rowOf(QStringLiteral("Position X"));
        ValueField* field = v.panel.tree()->valueField(posX);
        QVERIFY(field);
        QVERIFY(field->minimumWidth() >= field->sizeHint().width()); // spec 23.23: never clipped
        QCOMPARE(field->value(), 100.0);
        QCOMPARE(v.panel.tree()->diamond(posX)->state(), KeyframeButton::State::OnKey);
        emit field->valueCommitted(42.0);
        QCOMPARE(f.value(f.video, kPosX).valueAt(40), 42.0);
        QCOMPARE(f.undoCount(), 1);
        // A drag on the field is one step from press to release.
        emit field->scrubStarted();
        emit field->valueScrubbed(43.0);
        emit field->valueScrubbed(44.0);
        emit field->scrubFinished();
        QCOMPARE(f.value(f.video, kPosX).valueAt(40), 44.0);
        QCOMPARE(f.undoCount(), 2);
        // The diamond off a key adds one, and the inspector-style states follow.
        f.project.timeline()->setPlayhead(50 + 10);
        QCOMPARE(v.panel.tree()->diamond(posX)->state(), KeyframeButton::State::Animated);
        QTest::mouseClick(v.panel.tree()->diamond(posX), Qt::LeftButton);
        QVERIFY(f.value(f.video, kPosX).hasKeyAt(10));
        QCOMPARE(v.panel.tree()->diamond(posX)->state(), KeyframeButton::State::OnKey);
        QCOMPARE(f.undoCount(), 3);
        f.project.undo();
        QVERIFY(!f.value(f.video, kPosX).hasKeyAt(10));
        f.project.undo();
        f.project.undo();
        QCOMPARE(f.value(f.video, kPosX).valueAt(40), 100.0);
        // Outside the clip the diamonds are disabled (spec 15.5).
        f.project.timeline()->setPlayhead(10);
        QVERIFY(!v.panel.tree()->diamond(posX)->isEnabled());
    }

    // 15.16: the group diamond keys every parameter of the group at the playhead.
    void spec15_16_groupDiamondKeysGroup()
    {
        Fixture f;
        PanelView v(f.ctx);
        f.selection.selectClip(f.video);
        f.project.timeline()->setPlayhead(50 + 20);
        const int transform = v.rowOf(QStringLiteral("Transform"));
        QTest::mouseClick(v.panel.tree()->diamond(transform), Qt::LeftButton);
        QVERIFY(f.value(f.video, kPosX).hasKeyAt(20));
        QVERIFY(!f.value(f.video, kOpacity).hasKeyAt(20)); // Composite group (spec 24.19)
        QCOMPARE(f.undoCount(), 1);
        const int composite = v.rowOf(QStringLiteral("Composite"));
        QTest::mouseClick(v.panel.tree()->diamond(composite), Qt::LeftButton);
        QVERIFY(f.value(f.video, kOpacity).hasKeyAt(20));
        QCOMPARE(f.undoCount(), 2);
        QTest::mouseClick(v.panel.tree()->diamond(composite), Qt::LeftButton);
        QTest::mouseClick(v.panel.tree()->diamond(transform), Qt::LeftButton);
        QVERIFY(!f.value(f.video, kPosX).hasKeyAt(20));
        QVERIFY(!f.value(f.video, kOpacity).hasKeyAt(20));
    }

    // 15.17: dope sheet drags move keys in whole frames, one undo step; the inline lanes,
    // inspector and viewers read the same model.
    void spec15_17_dopeSheetDragUndo()
    {
        Fixture f;
        PanelView v(f.ctx);
        f.selection.selectClip(f.video);
        QApplication::processEvents();
        DopeSheetView* dope = v.panel.dopeSheet();
        const int posX = v.rowOf(QStringLiteral("Position X"));
        const QPoint from = dope->diamondPosAt(posX, 50 + 40);
        const double ppf = v.panel.timeMap().pixelsPerFrame();
        const QPoint to = from + QPoint(int(std::lround(10 * ppf)), 0);
        drag(dope, from, to);
        QVERIFY(!f.value(f.video, kPosX).hasKeyAt(40));
        QVERIFY(f.value(f.video, kPosX).hasKeyAt(50));
        QCOMPARE(f.undoCount(), 1);
        f.project.undo();
        QVERIFY(f.value(f.video, kPosX).hasKeyAt(40));
    }

    // 15.17: curve view drags change values (absolute axis), one undo step.
    void spec15_17_curveDragChangesValue()
    {
        Fixture f;
        PanelView v(f.ctx);
        f.selection.selectClip(f.video);
        v.panel.setView(KeyframesPanel::View::Curves);
        CurveView* curves = v.panel.curveView();
        curves->setAxisMode(CurveView::AxisMode::Absolute);
        for (const CurveInfo& c : curves->curves())
            curves->setCurveShown(c.id, c.id.path == kPosX);
        QApplication::processEvents();
        const ParamId id {f.video, kPosX};
        const QPoint from = curves->keyPosAt(id, 40).toPoint();
        drag(curves, from, from + QPoint(0, -30), Qt::ShiftModifier);
        QVERIFY(f.value(f.video, kPosX).hasKeyAt(40)); // Shift: value only
        QVERIFY(f.value(f.video, kPosX).valueAt(40) > 100.0);
        QCOMPARE(f.undoCount(), 1);
        f.project.undo();
        QCOMPARE(f.value(f.video, kPosX).valueAt(40), 100.0);
        // The tree check box follows the curve visibility.
        QCOMPARE(v.panel.tree()->curveCheck(v.rowOf(QStringLiteral("Opacity")))->checkState(),
            Qt::Unchecked);
    }

    // 15.18: the playhead is shared both ways.
    void spec15_18_playheadSync()
    {
        Fixture f;
        PanelView v(f.ctx);
        f.selection.selectClip(f.video);
        QApplication::processEvents();
        KeyframeRuler* ruler = v.panel.ruler();
        const int x = int(std::lround(v.panel.timeMap().frameToX(80.0)));
        QTest::mousePress(ruler, Qt::LeftButton, {}, QPoint(x, 10));
        QTest::mouseRelease(ruler, Qt::LeftButton, {}, QPoint(x, 10));
        QCOMPARE(f.project.timeline()->playhead(), 80);
        // Timeline -> panel: the tree shows the value at the playhead.
        f.project.timeline()->setPlayhead(50 + 20);
        const int posX = v.rowOf(QStringLiteral("Position X"));
        QCOMPARE(v.panel.tree()->valueField(posX)->value(), f.value(f.video, kPosX).valueAt(20));
        // Double-click on a dope sheet key jumps the playhead there.
        const QPoint key = v.panel.dopeSheet()->diamondPosAt(posX, 50 + 40);
        QTest::mouseDClick(v.panel.dopeSheet(), Qt::LeftButton, {}, key);
        QCOMPARE(f.project.timeline()->playhead(), 90);
        // Fit Clip: the clip fills the width.
        QVERIFY(v.panel.timeMap().frameToX(50) >= v.panel.curveView()->labelWidth());
        QVERIFY(v.panel.timeMap().frameToX(150) <= v.panel.dopeSheet()->width());
        // Follow Timeline mirrors the timeline range.
        v.panel.setViewMode(KeyframesPanel::ViewMode::FollowTimeline);
        v.panel.setTimelineView(0, 300);
        QCOMPARE(v.panel.timeMap().xToFrame(v.panel.curveView()->labelWidth()), 0.0);
    }

    // 15.19: toggled from the interface bar; floats into its own window and docks back; the
    // dock state persists with the page layout; closing the window hides the panel.
    void spec15_19_detachAndRedockPersist()
    {
        QSettings().clear();
        {
            Fixture f;
            f.makeWindow();
            EditPage* edit = f.window->editPage();
            QAction* toggle = f.actions.action(QStringLiteral("view.toggle_keyframes"));
            QVERIFY(!edit->isPanelVisible(PanelId::Keyframes));
            toggle->trigger();
            QVERIFY(toggle->isChecked());
            QVERIFY(edit->isPanelVisible(PanelId::Keyframes));
            KeyframesPanel* panel = edit->keyframes();
            QVERIFY(panel->isVisible());
            QVERIFY(qobject_cast<QSplitter*>(panel->parentWidget()));
            QVERIFY(panel->height() >= 150); // usable height when docked
            QTest::mouseClick(panel->floatButton(), Qt::LeftButton);
            QVERIFY(edit->isKeyframesFloating());
            QVERIFY(edit->keyframesWindow()->isVisible());
            QCOMPARE(panel->window(), edit->keyframesWindow());
            f.window->close(); // saves the layout
        }
        {
            Fixture f;
            f.makeWindow();
            EditPage* edit = f.window->editPage();
            QVERIFY(edit->isKeyframesFloating());
            QVERIFY(edit->isPanelVisible(PanelId::Keyframes));
            QVERIFY(edit->keyframesWindow()->isVisible());
            // Closing the window hides the panel, the toggle follows.
            edit->keyframesWindow()->close();
            QVERIFY(!edit->isPanelVisible(PanelId::Keyframes));
            QVERIFY(!f.actions.action(QStringLiteral("view.toggle_keyframes"))->isChecked());
            f.actions.action(QStringLiteral("view.toggle_keyframes"))->trigger();
            QVERIFY(edit->keyframesWindow()->isVisible());
            // Dock again.
            QTest::mouseClick(edit->keyframes()->floatButton(), Qt::LeftButton);
            QVERIFY(!edit->isKeyframesFloating());
            QVERIFY(qobject_cast<QSplitter*>(edit->keyframes()->parentWidget()));
            QVERIFY(edit->keyframes()->isVisible());
            f.window->close();
        }
        {
            Fixture f;
            f.makeWindow();
            QVERIFY(!f.window->editPage()->isKeyframesFloating());
            QVERIFY(f.window->editPage()->isPanelVisible(PanelId::Keyframes));
        }
        QSettings().clear();
    }

    // 15.22: one view at a time, Keyframes by default; the switch's two buttons; the buttons
    // of the value axis only with the curves; no splitter between the views any more.
    void spec15_22_showsOneViewAtATime()
    {
        Fixture f;
        PanelView v(f.ctx);
        f.selection.selectClip(f.video);
        QApplication::processEvents();
        KeyframesPanel& p = v.panel;
        QCOMPARE(p.view(), KeyframesPanel::View::Keyframes);
        QVERIFY(p.dopeSheet()->isVisible());
        QVERIFY(!p.curveView()->isVisible());
        QVERIFY(p.keyframesViewButton()->isChecked());
        QVERIFY(!p.normalisedButton()->isVisible());
        QVERIFY(!p.fitSelectionButton()->isVisible());
        const int minimumWidth = p.minimumSizeHint().width();
        const auto fillOf = [](QToolButton* b) {
            return b->grab().toImage().pixelColor(3, b->height() / 2).rgb();
        };
        QVERIFY(fillOf(p.keyframesViewButton()) != fillOf(p.curvesViewButton()));
        QTest::mouseClick(p.curvesViewButton(), Qt::LeftButton);
        QApplication::processEvents(); // the tool row lays out its buttons
        QCOMPARE(p.minimumSizeHint().width(), minimumWidth); // a docked column stays put
        QVERIFY(fillOf(p.keyframesViewButton()) != fillOf(p.curvesViewButton()));
        QCOMPARE(p.view(), KeyframesPanel::View::Curves);
        QVERIFY(!p.dopeSheet()->isVisible());
        QVERIFY(p.curveView()->isVisible());
        QVERIFY(p.curvesViewButton()->isChecked());
        QVERIFY(!p.keyframesViewButton()->isChecked());
        QVERIFY(p.normalisedButton()->isVisible());
        QVERIFY(p.fitSelectionButton()->isVisible());
        QVERIFY(p.dopeSheet()->width() > 0);
        QCOMPARE(p.findChildren<QSplitter*>().size(), 1); // tree | views
        QTest::mouseClick(p.keyframesViewButton(), Qt::LeftButton);
        QVERIFY(p.dopeSheet()->isVisible());
        QVERIFY(!p.curveView()->isVisible());
    }

    // 15.27: the panel shrinks for a small laptop screen: tool row buttons that do not fit
    // move into the "»" extension menu, the tree and the views have small minimums.
    void spec15_27_shrinksOnSmallScreens()
    {
        Fixture f;
        PanelView v(f.ctx);
        f.selection.selectClip(f.video);
        QApplication::processEvents();
        KeyframesPanel& p = v.panel;
        const QSize hint = p.minimumSizeHint();
        qInfo() << "keyframes panel minimum size" << hint << "tree" << p.tree()->minimumSizeHint()
                << "dope" << p.dopeSheet()->minimumSizeHint() << "ruler"
                << p.ruler()->minimumSizeHint() << "row"
                << p.findChild<QToolBar*>(QStringLiteral("KeyframesToolRow"))->minimumSizeHint();
        QVERIFY(hint.width() <= 320);
        QVERIFY(hint.height() <= 160);
        p.resize(hint.expandedTo(QSize(300, 200)));
        QApplication::processEvents();
        QVERIFY(p.width() <= 320);
        auto* bar = p.findChild<QToolBar*>(QStringLiteral("KeyframesToolRow"));
        QVERIFY(bar);
        QVERIFY(bar->width() <= p.width());
        QVERIFY(p.keyframesViewButton()->isVisible()); // first in order, kept longest
        QVERIFY(!p.findChild<QComboBox*>(QStringLiteral("KeyframesViewMode"))->isVisible());
        auto* ext = bar->findChild<QToolButton*>(QStringLiteral("qt_toolbar_ext_button"));
        QVERIFY(ext && ext->isVisible()); // the overflow button
        QVERIFY(p.tree()->isVisible() && p.tree()->width() > 0);
        QVERIFY(p.dopeSheet()->isVisible() && p.dopeSheet()->width() > 0);
        if (const QByteArray png = qgetenv("KK_KEYFRAMES_NARROW_PNG"); !png.isEmpty())
            p.grab().save(QString::fromLocal8Bit(png)); // for looking at the layout by hand
        p.resize(1400, 600);
        QApplication::processEvents();
        QVERIFY(p.findChild<QComboBox*>(QStringLiteral("KeyframesViewMode"))->isVisible());
        QVERIFY(!ext->isVisible());
    }

    // The view switch is two standard checkable tool buttons in one exclusive group, raised
    // like the other buttons of the row, with no style sheet of its own.
    void viewSwitchUsesStandardButtons()
    {
        Fixture f;
        PanelView v(f.ctx);
        KeyframesPanel& p = v.panel;
        QToolButton* a = p.keyframesViewButton();
        QToolButton* b = p.curvesViewButton();
        QVERIFY(a->isCheckable() && b->isCheckable());
        QVERIFY(a->autoRaise() && b->autoRaise());
        QVERIFY(p.viewGroup() && p.viewGroup()->exclusive());
        QCOMPARE(p.viewGroup()->buttons().size(), 2);
        QVERIFY(p.viewGroup()->buttons().contains(a) && p.viewGroup()->buttons().contains(b));
        QVERIFY(a->styleSheet().isEmpty() && b->styleSheet().isEmpty());
        QVERIFY(a->parentWidget()->styleSheet().isEmpty());
        QCOMPARE(a->parentWidget(), p.normalisedButton()->parentWidget());
    }

    // 15.22: switching keeps the selection, the time zoom and scroll, the shown curves and the
    // tree; both views use the same time map.
    // 15.25: a middle drag on the Keyframes view pans in time, without touching keys or selection.
    void spec15_25_middleDragPansTheDopeSheet()
    {
        Fixture f;
        PanelView v(f.ctx);
        f.selection.selectClip(f.video);
        KeyframesPanel& p = v.panel;
        p.setViewMode(KeyframesPanel::ViewMode::FollowTimeline);
        p.setTimelineView(20, 400);
        p.setViewMode(KeyframesPanel::ViewMode::Free);
        DopeSheetView* dope = p.dopeSheet();
        const auto before = f.selection.keyframes();
        const int undoBefore = f.project.undoStack()->count();
        const double xBefore = p.timeMap().frameToX(50);

        const QPoint from = dope->rect().center();
        QTest::mousePress(dope, Qt::MiddleButton, {}, from);
        const QPointF to(from + QPoint(-60, 0));
        QMouseEvent move(QEvent::MouseMove, to, dope->mapToGlobal(to), Qt::NoButton,
            Qt::MiddleButton, Qt::NoModifier);
        QApplication::sendEvent(dope, &move);
        QTest::mouseRelease(dope, Qt::MiddleButton, {}, from + QPoint(-60, 0));

        QVERIFY2(std::abs(p.timeMap().frameToX(50) - (xBefore - 60)) < 2,
            qPrintable(QString::number(p.timeMap().frameToX(50) - xBefore)));
        QCOMPARE(f.selection.keyframes(), before);
        QCOMPARE(f.project.undoStack()->count(), undoBefore);
    }

    void spec15_22_switchKeepsSharedState()
    {
        Fixture f;
        PanelView v(f.ctx);
        f.selection.selectClip(f.video);
        const QVector<KeyframeRef> keys = ko::keysAt(f.clip(f.video), 40);
        f.selection.setKeyframes(keys);
        KeyframesPanel& p = v.panel;
        p.setViewMode(KeyframesPanel::ViewMode::FollowTimeline);
        p.setTimelineView(20, 400);
        p.setViewMode(KeyframesPanel::ViewMode::Free);
        const ParamId opacity {f.video, kOpacity};
        p.curveView()->setCurveShown(opacity, false);
        QApplication::processEvents();
        const double ppf = p.timeMap().pixelsPerFrame();
        const double x50 = p.timeMap().frameToX(50);
        const int rows = p.tree()->rowCount();
        for (const auto view : {KeyframesPanel::View::Curves, KeyframesPanel::View::Keyframes}) {
            p.setView(view);
            QApplication::processEvents();
            QCOMPARE(f.selection.keyframes(), keys);
            QCOMPARE(p.timeMap().pixelsPerFrame(), ppf);
            QCOMPARE(p.timeMap().frameToX(50), x50);
            QCOMPARE(p.viewMode(), KeyframesPanel::ViewMode::Free);
            QVERIFY(!p.curveView()->isCurveShown(opacity));
            QCOMPARE(p.tree()->rowCount(), rows);
        }
        QCOMPARE(f.undoCount(), 0);
    }

    // 15.22: the choice survives a restart.
    void spec15_22_viewSurvivesRestart()
    {
        QSettings().clear();
        {
            Fixture f;
            f.makeWindow();
            f.window->editPage()->keyframes()->setView(KeyframesPanel::View::Curves);
            f.window->close();
        }
        {
            Fixture f;
            f.makeWindow();
            QCOMPARE(f.window->editPage()->keyframes()->view(), KeyframesPanel::View::Curves);
        }
        QSettings().clear();
    }

    // 15.22: a layout stored by the stacked panel: its splitter value is ignored and dropped,
    // the tree | views splitter keeps its own value, and curves alone stay curves.
    void spec15_22_storedStackedLayoutIsIgnored()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings s(dir.filePath(QStringLiteral("layout.ini")), QSettings::IniFormat);
        Fixture f;
        PanelView v(f.ctx);
        const QByteArray split = v.panel.findChild<QSplitter*>()->saveState();
        s.setValue(QStringLiteral("keyframes/split"), split);
        // A state of a vertical splitter, as the old "viewSplit" held.
        QSplitter old(Qt::Vertical);
        old.addWidget(new QWidget);
        old.addWidget(new QWidget);
        s.setValue(QStringLiteral("keyframes/viewSplit"), old.saveState());
        s.setValue(QStringLiteral("keyframes/dopeSheet"), true);
        s.setValue(QStringLiteral("keyframes/curves"), true);
        v.panel.restoreState(s, QStringLiteral("keyframes/"));
        QCOMPARE(v.panel.view(), KeyframesPanel::View::Keyframes); // both on: the default
        QCOMPARE(v.panel.findChild<QSplitter*>()->orientation(), Qt::Horizontal);
        s.setValue(QStringLiteral("keyframes/dopeSheet"), false);
        v.panel.restoreState(s, QStringLiteral("keyframes/"));
        QCOMPARE(v.panel.view(), KeyframesPanel::View::Curves);
        v.panel.saveState(s, QStringLiteral("keyframes/"));
        QVERIFY(!s.contains(QStringLiteral("keyframes/viewSplit")));
        QVERIFY(!s.contains(QStringLiteral("keyframes/dopeSheet")));
        QVERIFY(!s.contains(QStringLiteral("keyframes/curves")));
        QCOMPARE(s.value(QStringLiteral("keyframes/view")).toString(), QStringLiteral("curves"));
    }

    // 15.22: what asks for a view shows it: the toggle command (no default key), the clip's
    // editors, "Open in Keyframes Panel", a curve ticked in the tree.
    void spec15_22_entryPointsSwitchView()
    {
        QSettings().clear();
        Fixture f;
        f.makeWindow();
        EditPage* edit = f.window->editPage();
        KeyframesPanel* p = edit->keyframes();
        const auto kCurves = KeyframesPanel::View::Curves;
        const auto kKeys = KeyframesPanel::View::Keyframes;
        const QString toggleId = QStringLiteral("view.keyframes_panel_toggle_view");
        QVERIFY(f.actions.contains(toggleId));
        QAction* toggle = f.actions.action(toggleId);
        QVERIFY(toggle->shortcut().isEmpty());
        QVERIFY(!edit->isPanelVisible(PanelId::Keyframes));
        toggle->trigger(); // hidden: opens in the view it had
        QCOMPARE(p->view(), kKeys);
        QVERIFY(edit->isPanelVisible(PanelId::Keyframes));
        QVERIFY(p->isVisible());
        toggle->trigger();
        QCOMPARE(p->view(), kCurves);
        toggle->trigger();
        QCOMPARE(p->view(), kKeys);

        f.selection.selectClip(f.video);
        // The clip's editors: opening switches, closing leaves the view alone.
        f.actions.action(QStringLiteral("clip.toggle_curve_editor"))->trigger();
        QCOMPARE(p->view(), kCurves);
        f.actions.action(QStringLiteral("clip.toggle_keyframe_editor"))->trigger();
        QCOMPARE(p->view(), kKeys);
        f.actions.action(QStringLiteral("clip.toggle_curve_editor"))->trigger(); // closes
        QCOMPARE(p->view(), kKeys);

        auto* timeline = f.window->findChild<TimelinePanel*>();
        QVERIFY(timeline);
        KeyframeLanes* lanes = timeline->timelineWidget()->keyframeLanes();
        edit->setPanelVisible(PanelId::Keyframes, false);
        f.selection.clear();
        emit lanes->openInPanelRequested(f.audio, true);
        QCOMPARE(p->view(), kCurves);
        QVERIFY(edit->isPanelVisible(PanelId::Keyframes));
        QVERIFY(f.selection.isSelected(f.audio));
        emit lanes->openInPanelRequested(f.audio, false);
        QCOMPARE(p->view(), kKeys);

        // A curve ticked on in the tree shows the curves; ticked off it stays where it is.
        const QVector<ParamId> ids {ParamId {f.video, kPosX}};
        emit p->tree()->curvesToggled(ids, false);
        QCOMPARE(p->view(), kKeys);
        emit p->tree()->curvesToggled(ids, true);
        QCOMPARE(p->view(), kCurves);
        f.window->close();
        QSettings().clear();
    }

    // 15.23: the panel's regions differ without a selection, in a dark and a light scheme: the
    // rows from the window background, every other row from its neighbour, the ruler band from
    // the rows and from the window. Colours sampled from the painted panel.
    void spec15_23_regionsAreDistinct_data()
    {
        QTest::addColumn<QString>("scheme");
        QTest::newRow("Kinoklip") << QStringLiteral("Kinoklip");
        QTest::newRow("BreezeLight") << QStringLiteral("BreezeLight");
    }
    void spec15_23_regionsAreDistinct()
    {
        QFETCH(QString, scheme);
        theme::setColorScheme(scheme);
        QApplication::processEvents();
        Fixture f;
        PanelView v(f.ctx);
        // The clip's keys are selected as a clip, no key selected.
        f.selection.selectClip(f.video);
        v.panel.setViewMode(KeyframesPanel::ViewMode::FollowTimeline);
        v.panel.setTimelineView(0, 400);
        QApplication::processEvents();
        const SchemeColors& sc = schemeColors();
        const QColor window = v.panel.palette().color(QPalette::Window);
        const auto apart = [](const QColor& a, const QColor& b, int minimum) {
            const int d = std::abs(qGray(a.rgb()) - qGray(b.rgb()));
            if (d < minimum)
                qWarning() << a.name() << b.name() << "differ by" << d << "of" << minimum;
            return d >= minimum;
        };

        const QImage dope = v.panel.dopeSheet()->grab().toImage();
        const int inClip = int(v.panel.timeMap().frameToX(140)); // after the last key
        QColor row, stripe;
        const int rowCount = v.panel.dopeSheet()->rows().size();
        // Rows 3 and 4 (Composite, Opacity: no item fill, no span line this far right).
        const int rowH = KeyframeParamTree::kRowHeight;
        stripe = dope.pixelColor(inClip, 3 * rowH + rowH / 2);
        row = dope.pixelColor(inClip, 4 * rowH + rowH / 2);
        QVERIFY(rowCount > 4);
        QCOMPARE(row.rgb(), sc.keyRows.rgb());
        QVERIFY(apart(row, stripe, 6));
        QVERIFY(apart(row, window, 8));
        QVERIFY(apart(stripe, window, 5));
        // Outside the clip's span is its own shade.
        const QColor outside
            = dope.pixelColor(int(v.panel.timeMap().frameToX(300)), dope.height() - 2);
        QCOMPARE(outside.rgb(), sc.keyRowsOutside.rgb());
        QVERIFY(apart(outside, row, 4));
        QVERIFY(apart(outside, window, 8));

        // The curves share the rows' background.
        v.panel.setView(KeyframesPanel::View::Curves);
        QApplication::processEvents();
        const QImage curvesImage = v.panel.curveView()->grab().toImage();
        bool curvesOutside = false; // grid lines cross it: any pixel of a short stretch will do
        for (int dx = 0; dx < 40 && !curvesOutside; ++dx) {
            curvesOutside
                = curvesImage.pixelColor(int(v.panel.timeMap().frameToX(300)) + dx, 3).rgb()
                == sc.keyRowsOutside.rgb();
        }
        QVERIFY(curvesOutside);

        // The ruler band (and the tree's header beside it).
        const QColor ruler
            = v.panel.ruler()->grab().toImage().pixelColor(v.panel.ruler()->width() - 2, 1);
        QCOMPARE(ruler.rgb(), sc.keyRuler.rgb());
        QVERIFY(apart(ruler, row, 8));
        QVERIFY(apart(ruler, window, 4));
        auto* header = v.panel.findChild<QLabel*>(QStringLiteral("KeyframesTreeHeader"));
        QVERIFY(header);
        QCOMPARE(header->grab().toImage().pixelColor(header->width() - 2, 1).rgb(), ruler.rgb());
        theme::setColorScheme(QStringLiteral("Kinoklip"));
    }

    // 11.9: keys are snap targets of the playhead like markers, for the keys of the shown clips,
    // not the keys trimmed away or being dragged, not the retime curve.
    void spec11_9_keySnapTargets()
    {
        Fixture f;
        const TimelineData& d = f.project.timeline()->data();
        const auto targets = ko::withKeySnapTargets({0}, d, {f.video});
        QCOMPARE(targets, (std::vector<int64_t> {0, 50, 90})); // clip at 50, keys 0 and 40
        QCOMPARE(ko::withKeySnapTargets({0}, d, {}), (std::vector<int64_t> {0}));
        // opacity also has a key at 40: 90 stays until both are excluded.
        const QVector<KeyframeRef> posX40 {{f.video, kPosX, 40}};
        QCOMPARE(
            ko::withKeySnapTargets({0}, d, {f.video}, posX40), (std::vector<int64_t> {0, 50, 90}));
        const QVector<KeyframeRef> both {{f.video, kPosX, 40}, {f.video, kOpacity, 40}};
        QCOMPARE(ko::withKeySnapTargets({0}, d, {f.video}, both), (std::vector<int64_t> {0, 50}));
        // Trimmed out of the clip: not a target.
        f.project.beginEdit(QStringLiteral("trim"));
        editops::clipById(f.project.timeline()->mutableData(), f.video)->duration = 30;
        f.project.endEdit();
        QCOMPARE(ko::withKeySnapTargets({}, f.project.timeline()->data(), {f.video}),
            (std::vector<int64_t> {50}));
    }

    // 11.9: scrubbing the panel's ruler snaps to keys, in both views; the Snapping toggle turns
    // it off, read live during the drag (11.5); timeline markers count too.
    void spec11_9_panelRulerSnapsToKeys_data()
    {
        QTest::addColumn<bool>("curves");
        QTest::newRow("keyframes") << false;
        QTest::newRow("curves") << true;
    }
    void spec11_9_panelRulerSnapsToKeys()
    {
        QFETCH(bool, curves);
        Fixture f;
        PanelView v(f.ctx);
        f.selection.selectClip(f.video);
        KeyframesPanel& p = v.panel;
        p.setView(curves ? KeyframesPanel::View::Curves : KeyframesPanel::View::Keyframes);
        // About 2.4 px per frame: the snap distance (8 px) is 3 frames.
        p.setViewMode(KeyframesPanel::ViewMode::FollowTimeline);
        p.setTimelineView(0, 400);
        QApplication::processEvents();
        QVERIFY(p.timeMap().pixelsPerFrame() > 2.0);
        QVERIFY(p.timeMap().pixelsPerFrame() < 4.0);
        QAction* snapping = f.actions.action(QStringLiteral("timeline.snapping"));
        snapping->setChecked(true);
        Timeline* t = f.project.timeline();
        KeyframeRuler* ruler = p.ruler();
        const auto xOf
            = [&](int frame) { return QPoint(int(std::lround(p.timeMap().frameToX(frame))), 5); };

        QTest::mousePress(ruler, Qt::LeftButton, {}, xOf(92)); // key at 50 + 40
        QCOMPARE(t->playhead(), int64_t(90));
        QTest::mouseMove(ruler, xOf(110)); // volume key of the linked audio: 50 + 60
        QCOMPARE(t->playhead(), int64_t(110));
        QTest::mouseMove(ruler, xOf(130));
        QCOMPARE(t->playhead(), int64_t(130)); // nothing near
        snapping->setChecked(false);
        QTest::mouseMove(ruler, xOf(92));
        QCOMPARE(t->playhead(), int64_t(92));
        snapping->setChecked(true);
        QTest::mouseMove(ruler, xOf(92));
        QCOMPARE(t->playhead(), int64_t(90));
        QTest::mouseRelease(ruler, Qt::LeftButton, {}, xOf(92));
        QVERIFY(!ruler->isScrubbing());

        // Markers, which the panel's ruler ignored before.
        f.project.beginEdit(QStringLiteral("marker"));
        Marker marker;
        marker.frame = 200;
        t->mutableData().markers.append(marker);
        f.project.endEdit();
        QTest::mousePress(ruler, Qt::LeftButton, {}, xOf(201));
        QCOMPARE(t->playhead(), int64_t(200));
        QTest::mouseRelease(ruler, Qt::LeftButton, {}, xOf(201));
        snapping->setChecked(false);
    }

    // 15.24: a dragged key snaps to the other keys of the shown clips and to markers, in the
    // dope sheet and the curves (shared code), not to itself.
    void spec15_24_keyDragSnapsToKeysAndMarkers()
    {
        Fixture f;
        PanelView v(f.ctx);
        f.selection.selectClip(f.video);
        f.project.beginEdit(QStringLiteral("marker"));
        Marker marker;
        marker.frame = 70;
        f.project.timeline()->mutableData().markers.append(marker);
        f.project.endEdit();
        f.project.undoStack()->clear();
        KeyframesPanel& p = v.panel;
        p.setViewMode(KeyframesPanel::ViewMode::FollowTimeline);
        p.setTimelineView(0, 400);
        QApplication::processEvents();
        f.actions.action(QStringLiteral("timeline.snapping"))->setChecked(true);
        DopeSheetView* dope = p.dopeSheet();
        const int y = dope->diamondPosAt(v.rowOf(QStringLiteral("Position X")), 50).y();
        const auto xOf = [&](int frame) { return int(std::lround(p.timeMap().frameToX(frame))); };

        // Position X's first key (frame 50), dropped next to the marker at 70.
        drag(dope, QPoint(xOf(50), y), QPoint(xOf(68), y));
        QVERIFY(f.value(f.video, kPosX).hasKeyAt(20));
        f.project.undo();
        // ... and next to the volume key of the linked audio at 110 (local 60).
        drag(dope, QPoint(xOf(50), y), QPoint(xOf(112), y));
        QVERIFY(f.value(f.video, kPosX).hasKeyAt(60));
        f.project.undo();
        // Nothing near: where it was dropped. Its own old frame is no target either.
        drag(dope, QPoint(xOf(50), y), QPoint(xOf(130), y));
        QVERIFY(f.value(f.video, kPosX).hasKeyAt(80));
        f.project.undo();
        QVERIFY(f.value(f.video, kPosX).hasKeyAt(0));
        f.actions.action(QStringLiteral("timeline.snapping"))->setChecked(false);
    }
};

QTEST_MAIN(TestKeyframesPanel)
#include "test_keyframespanel.moc"
