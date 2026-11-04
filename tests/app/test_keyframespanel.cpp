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
#include "ui/common/StandardActions.h"
#include "ui/inspector/KeyframeButton.h"
#include "ui/inspector/ValueField.h"
#include "ui/keyframes/CurveView.h"
#include "ui/keyframes/DopeSheetView.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/keyframes/KeyframeParamTree.h"
#include "ui/keyframes/KeyframeRuler.h"
#include "ui/keyframes/KeyframesPanel.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTest>
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
        v.panel.setViews(false, true);
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
};

QTEST_MAIN(TestKeyframesPanel)
#include "test_keyframespanel.moc"
