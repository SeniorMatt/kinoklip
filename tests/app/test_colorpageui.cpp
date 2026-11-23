// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the full Color page (docs/behavior-spec.md 44b, rows from 44b.12): the
// node editor (add / connect / move / bypass, each one undo step), wheels and bars as one
// step per drag, the Curves palette (linked R G B, HSL modes), the qualifier eyedropper
// (pick / add / remove ranges, highlight), power window handles in the viewer, gallery stills
// (apply, wipe), the clips strip (selection changes the graded clip, middle-click and
// Ctrl+C / Ctrl+V copy grades) and the interface bar toggles.
#include "../media/MediaTestUtil.h"
#include "app/MainWindow.h"
#include "app/pages/ColorPage.h"
#include "core/EditOps.h"
#include "core/GalleryStillFiles.h"
#include "core/GradeGallery.h"
#include "core/GradeKeyframes.h"
#include "core/GradeMath.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "render/GradeEval.h"
#include "ui/color/ColorBrowsers.h"
#include "ui/color/ColorFrameSource.h"
#include "core/ColorMatch.h"
#include "ui/color/ColorKeyframes.h"
#include "ui/color/ColorMatchPanel.h"
#include "ui/color/ColorNodeEditor.h"
#include "ui/color/ColorSizing.h"
#include "ui/color/ColorTracker.h"
#include "ui/color/ColorPalettes.h"
#include "ui/color/ColorSession.h"
#include "ui/color/ColorViewerOverlay.h"
#include "ui/color/ColorWidgets.h"
#include "ui/color/QualifierPick.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/inspector/ValueField.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

#include <array>
#include <future>
#include <memory>
#include <random>

using namespace kk;
namespace go = kk::gradeops;
namespace gk = kk::gradekeys;
namespace wk = kk::gradewindowkeys;

namespace {

struct Fixture
{
    Project project;
    QTemporaryDir powerDir; ///< the PowerGrade file of the test, never the user's
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;
    QVector<quint64> clips;

    explicit Fixture(int clipCount = 3)
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        project.setPowerGradeFile(powerDir.filePath(QStringLiteral("powergrades.json")));
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        project.beginEdit(QStringLiteral("Add Clips"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 320;
        tl.height = 180;
        const QColor colours[]
            = {QColor(200, 60, 40), QColor(40, 90, 200), QColor(60, 180, 70), QColor(180, 180, 60)};
        for (int i = 0; i < clipCount; ++i) {
            ClipData clip;
            clip.kind = ClipKind::Solid;
            clip.solidColor = colours[i % 4];
            clip.name = QStringLiteral("Clip %1").arg(i + 1);
            clip.start = i * 50;
            clip.duration = 50;
            clips.append(editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, clip));
        }
        project.endEdit();
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    ColorPage* page() const { return window->colorPage(); }
    ColorSession* session() const { return page()->session(); }
    /// Undo steps done (the index: undone steps stay on the stack until the next push).
    int steps() const { return project.undoStack()->index(); }
    ClipGrade grade(quint64 clip) const
    {
        return project.clipGrade(project.activeTimelineUid(), clip);
    }
    void showPage(QSize size = QSize(1600, 900))
    {
        window->resize(size);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        action("view.page_color")->trigger();
        QCoreApplication::processEvents();
    }
    void key(Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        page()->nodeEditor()->setFocus();
        QTest::keyClick(page()->nodeEditor(), key, mods);
        QCoreApplication::processEvents();
    }
    /// Gives the frame source a picture: red left half, blue right half.
    void splitInput()
    {
        QImage img(320, 180, QImage::Format_ARGB32_Premultiplied);
        QPainter p(&img);
        p.fillRect(0, 0, 160, 180, QColor(220, 40, 40));
        p.fillRect(160, 0, 160, 180, QColor(40, 60, 220));
        p.end();
        page()->frames()->setInput(img);
        page()->refreshPictures();
    }
};

void drag(QWidget* w, QPointF from, QPointF to, Qt::MouseButton button = Qt::LeftButton,
    Qt::KeyboardModifiers mods = {})
{
    QTest::mousePress(w, button, mods, from.toPoint());
    for (int i = 1; i <= 4; ++i)
        QTest::mouseMove(w, (from + (to - from) * (i / 4.0)).toPoint());
    QTest::mouseRelease(w, button, mods, to.toPoint());
    QCoreApplication::processEvents();
}

} // namespace

class TestColorPageUi : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-test"));
        QCoreApplication::setApplicationName(QStringLiteral("test_colorpageui"));
        QSettings().clear();
        initResources();
    }

    /// Node types (roadmap 1.13): Outside (Alt+O), Key Mixer, Splitter / Combiner, Compound, Alpha
    /// Output: add through the commands and the context menu, connect keys by dragging, open a
    /// compound, all undoable, and saved with the project.
    void nodeTypesInTheEditor()
    {
        Fixture f;
        f.showPage();
        ColorNodeEditor* ne = f.page()->nodeEditor();
        const quint64 clip = f.clips[0];
        const int base = f.steps();
        // Alt+O on the ungraded clip: node 01 and an Outside node keyed by it, inverted.
        f.key(Qt::Key_O, Qt::AltModifier);
        ClipGrade g = f.grade(clip);
        QCOMPARE(g.nodes.size(), 2);
        QCOMPARE(f.steps(), base + 1);
        const quint64 n1 = g.nodes[0].id, outside = g.nodes[1].id;
        QCOMPARE(g.node(outside)->keyInput, n1);
        QVERIFY(g.node(outside)->keyInputInvert);
        QCOMPARE(g.node(outside)->inputs, QVector<quint64> {n1});
        QCOMPARE(g.output, outside);
        QCOMPARE(f.session()->selectedNode(), outside);
        // The key ports are blue circles below the picture ports; hit tests find them.
        QCOMPARE(ne->hitTest(ne->keyOutputPos(*g.node(n1))).kind,
            ColorNodeEditor::Hit::Kind::KeyOutput);
        QCOMPARE(ne->hitTest(ne->keyInputPos(*g.node(outside))).kind,
            ColorNodeEditor::Hit::Kind::KeyInput);
        QVERIFY(ne->keyInputPos(*g.node(outside)).y() > ne->inputPos(*g.node(outside), 0).y());
        f.action("edit.undo")->trigger();
        QCOMPARE(f.grade(clip).nodes.size(), 0); // back to the ungraded clip
        f.action("edit.redo")->trigger();
        QCOMPARE(f.grade(clip), g);

        // The context menu offers every node type.
        f.session()->selectNode(n1);
        std::unique_ptr<QMenu> menu(ne->buildContextMenu(ne->tileRect(*g.node(n1)).center(), ne));
        for (const char* id : {"color.add_outside", "color.add_key_mixer",
                 "color.add_splitter_combiner", "color.add_alpha_output", "color.group_nodes",
                 "color.ungroup_nodes", "color.open_compound", "color.close_compound"})
            QVERIFY2(menu->findChild<QAction*>(QLatin1String(id)), id);
        QVERIFY(menu->findChild<QAction*>(QStringLiteral("color.group_nodes"))->isEnabled());
        QVERIFY(!menu->findChild<QAction*>(QStringLiteral("color.ungroup_nodes"))->isEnabled());

        // Key Mixer: added from node 01 (its key feeds the first input); drag the Outside
        // node's key output onto the mixer to add a second input.
        f.session()->selectNode(n1);
        f.action("color.add_key_mixer")->trigger();
        g = f.grade(clip);
        quint64 mixer = 0;
        for (const GradeNode& n : g.nodes)
            mixer = n.type == GradeNodeType::KeyMixer ? n.id : mixer;
        QVERIFY(mixer);
        QCOMPARE(g.node(mixer)->inputs, QVector<quint64> {n1});
        const int beforeKey = f.steps();
        drag(ne, ne->keyOutputPos(*g.node(outside)), ne->tileRect(*g.node(mixer)).center());
        g = f.grade(clip);
        QCOMPARE(g.node(mixer)->inputs, (QVector<quint64> {n1, outside}));
        QCOMPARE(f.steps(), beforeKey + 1);
        f.action("edit.undo")->trigger();
        QCOMPARE(f.grade(clip).node(mixer)->inputs, QVector<quint64> {n1});
        f.action("edit.redo")->trigger();
        // The mixer's output into the Outside node's key input (replacing the link to 01).
        g = f.grade(clip);
        drag(ne, ne->keyOutputPos(*g.node(mixer)), ne->keyInputPos(*g.node(n1)));
        QCOMPARE(f.grade(clip).node(n1)->keyInput, quint64(0)); // 01 would close a cycle
        drag(ne, ne->keyOutputPos(*g.node(n1)), ne->inputPos(*g.node(outside), 0));
        QCOMPARE(f.grade(clip), g); // a key output does not connect to a picture input
        // Picking up the key link into the Outside node and dropping it into empty space.
        g = f.grade(clip);
        drag(ne, ne->keyInputPos(*g.node(outside)), QPointF(ne->width() - 5, 5));
        QCOMPARE(f.grade(clip).node(outside)->keyInput, quint64(0));
        f.action("edit.undo")->trigger();
        QCOMPARE(f.grade(clip).node(outside)->keyInput, n1);
        // Invert and gain of a mixer input through the menu of its key port.
        g = f.grade(clip);
        std::unique_ptr<QMenu> keyMenu(ne->buildContextMenu(ne->keyInputPos(*g.node(mixer), 1), ne));
        QAction* invert = keyMenu->findChild<QAction*>(QStringLiteral("color.key_invert"));
        QVERIFY(invert);
        invert->trigger();
        QVERIFY(f.grade(clip).node(mixer)->keyInverted(1));
        f.action("edit.undo")->trigger();
        QVERIFY(!f.grade(clip).node(mixer)->keyInverted(1));
        QVERIFY(f.session()->setKeyGain(mixer, 1, 0.5));
        QCOMPARE(f.grade(clip).node(mixer)->keyGain(1), 0.5);

        // Alpha Output: one only; its key input is the selected node's key.
        f.session()->selectNode(mixer);
        f.action("color.add_alpha_output")->trigger();
        g = f.grade(clip);
        quint64 alpha = 0;
        for (const GradeNode& n : g.nodes)
            alpha = n.type == GradeNodeType::AlphaOutput ? n.id : alpha;
        QVERIFY(alpha);
        QCOMPARE(g.node(alpha)->keyInput, mixer);
        f.session()->selectNode(outside);
        f.action("color.add_alpha_output")->trigger();
        QCOMPARE(f.grade(clip).node(alpha)->keyInput, outside);
        int alphas = 0;
        for (const GradeNode& n : f.grade(clip).nodes)
            alphas += n.type == GradeNodeType::AlphaOutput ? 1 : 0;
        QCOMPARE(alphas, 1);
        QVERIFY(f.session()->grade().node(alpha) != nullptr);

        // Splitter / Combiner after the Outside node.
        f.session()->selectNode(outside);
        f.action("color.add_splitter_combiner")->trigger();
        g = f.grade(clip);
        QCOMPARE(g.node(g.output)->type, GradeNodeType::Combiner);
        quint64 splitter = 0;
        for (const GradeNode& n : g.nodes)
            splitter = n.type == GradeNodeType::Splitter ? n.id : splitter;
        QVERIFY(splitter);
        QCOMPARE(ne->hitTest(ne->outputPos(*g.node(splitter), 2)).index, 2);
        QCOMPARE(ne->hitTest(ne->inputPos(*g.node(g.output), 1)).index, 1);
        // Drag the splitter's green output to the combiner's blue input: one step, port kept.
        const int beforePort = f.steps();
        drag(ne, ne->outputPos(*g.node(splitter), 1), ne->inputPos(*g.node(g.output), 2));
        QCOMPARE(f.grade(clip).node(g.output)->inputs[2], splitter);
        QCOMPARE(f.grade(clip).node(g.output)->portOf(2), 1);
        QCOMPARE(f.steps(), beforePort + 1);
        f.action("edit.undo")->trigger();
        QCOMPARE(f.grade(clip).node(g.output)->portOf(2), 2);

        // Everything is saved with the project and loads again.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("types.kkproj"));
        QString error;
        QVERIFY2(f.project.save(path, &error), qPrintable(error));
        Project loaded;
        QVERIFY2(loaded.load(path, &error), qPrintable(error));
        QCOMPARE(loaded.clipGrade(loaded.activeTimelineUid(), clip), f.grade(clip));
    }

    void compoundNodeInTheEditor()
    {
        Fixture f;
        f.showPage();
        ColorNodeEditor* ne = f.page()->nodeEditor();
        const quint64 clip = f.clips[0];
        f.key(Qt::Key_S, Qt::AltModifier);
        f.key(Qt::Key_S, Qt::AltModifier);
        ClipGrade g = f.grade(clip);
        QCOMPARE(g.nodes.size(), 3);
        const quint64 n1 = g.nodes[0].id, n2 = g.nodes[1].id, n3 = g.nodes[2].id;
        // Ctrl+click adds to the selection; the context menu groups the two nodes.
        QTest::mouseClick(ne, Qt::LeftButton, {}, ne->tileRect(*g.node(n1)).center().toPoint());
        QTest::mouseClick(ne, Qt::LeftButton, Qt::ControlModifier,
            ne->tileRect(*g.node(n2)).center().toPoint());
        QCOMPARE(f.session()->selectedNodes().size(), 2);
        const int base = f.steps();
        f.action("color.group_nodes")->trigger();
        g = f.grade(clip);
        QCOMPARE(f.steps(), base + 1);
        quint64 compound = 0;
        for (const GradeNode& n : g.nodes)
            compound = n.type == GradeNodeType::Compound ? n.id : compound;
        QVERIFY(compound);
        QCOMPARE(g.node(n3)->inputs, QVector<quint64> {compound});
        QCOMPARE(f.session()->selectedNode(), compound);
        // The editor shows the compound as one tile; the members are hidden.
        QCOMPARE(ne->hitTest(ne->tileRect(*g.node(compound)).center()).node, compound);
        // Double-click opens it: now its two nodes are shown, the compound is not.
        QTest::mouseDClick(ne, Qt::LeftButton, {}, ne->tileRect(*g.node(compound)).center().toPoint());
        QCOMPARE(f.session()->openCompoundId(), compound);
        g = f.grade(clip);
        QCOMPARE(ne->hitTest(ne->tileRect(*g.node(n1)).center()).node, n1);
        QVERIFY(ne->hitTest(ne->tileRect(*g.node(n3)).center()).node != n3); // not inside
        // A node added inside belongs to the compound and leads it when it follows its output.
        f.session()->selectNode(n2);
        f.action("color.add_serial")->trigger();
        g = f.grade(clip);
        const quint64 inner = f.session()->selectedNode();
        QCOMPARE(g.node(inner)->compound, compound);
        QCOMPARE(g.node(compound)->innerOutput, inner);
        QCOMPARE(g.node(n3)->inputs, QVector<quint64> {compound}); // still reads the compound
        // Esc closes it.
        f.key(Qt::Key_Escape);
        QCOMPARE(f.session()->openCompoundId(), quint64(0));
        // Decompose gives the nodes back (one step), undo groups them again.
        f.session()->selectNode(compound);
        const int beforeUngroup = f.steps();
        f.action("color.ungroup_nodes")->trigger();
        QCOMPARE(f.steps(), beforeUngroup + 1);
        g = f.grade(clip);
        QVERIFY(g.node(compound) == nullptr);
        QCOMPARE(g.node(n3)->inputs, QVector<quint64> {inner});
        f.action("edit.undo")->trigger();
        QVERIFY(f.grade(clip).node(compound) != nullptr);
        // Open / close through the commands; closing at the top level does nothing.
        f.session()->selectNode(compound);
        f.action("color.open_compound")->trigger();
        QCOMPARE(f.session()->openCompoundId(), compound);
        f.action("color.close_compound")->trigger();
        QCOMPARE(f.session()->openCompoundId(), quint64(0));
        QVERIFY(!f.session()->closeCompound());
    }

    void nodeEditorAddConnectBypassUndo()
    {
        Fixture f;
        f.showPage();
        ColorNodeEditor* ne = f.page()->nodeEditor();
        const quint64 clip = f.clips[0];
        const int base = f.steps();
        // Alt+S on the ungraded clip: node 01 and a serial node 02 (spec 44b.22); the editor
        // shows both with a numbered tile.
        f.key(Qt::Key_S, Qt::AltModifier);
        ClipGrade g = f.grade(clip);
        QCOMPARE(g.nodes.size(), 2);
        QCOMPARE(f.steps(), base + 1);
        const quint64 n1 = g.nodes[0].id, n2 = g.nodes[1].id;
        QCOMPARE(g.output, n2);
        QVERIFY(ne->tileRect(g.nodes[0]).isValid());
        QVERIFY(!ne->tileRect(g.nodes[0]).intersects(ne->tileRect(g.nodes[1])));
        // Clicking a tile selects its node.
        QTest::mouseClick(ne, Qt::LeftButton, {}, ne->tileRect(g.nodes[0]).center().toPoint());
        QCOMPARE(f.session()->selectedNode(), n1);
        // Drag node 1's output onto the grade output: node 1 becomes the output (one step).
        drag(ne, ne->outputPos(g.nodes[0]), ne->gradeOutputPos());
        g = f.grade(clip);
        QCOMPARE(g.output, n1);
        QCOMPARE(f.steps(), base + 2);
        f.action("edit.undo")->trigger();
        QCOMPARE(f.grade(clip).output, n2);
        // Drag the source onto node 2's input: it now reads the clip directly.
        g = f.grade(clip);
        drag(ne, ne->sourcePos(), ne->inputPos(*g.node(n2), 0));
        QCOMPARE(f.grade(clip).node(n2)->inputs.value(0), quint64(0));
        f.action("edit.undo")->trigger();
        QCOMPARE(f.grade(clip).node(n2)->inputs.value(0), n1);
        // Picking up node 2's input and dropping it into empty space disconnects it (source).
        g = f.grade(clip);
        drag(ne, ne->inputPos(*g.node(n2), 0), QPointF(ne->width() - 5, 5));
        QCOMPARE(f.grade(clip).node(n2)->inputs.value(0), quint64(0));
        f.action("edit.undo")->trigger();
        // Moving a tile stores GradeNode::pos as one step.
        g = f.grade(clip);
        const QPointF before = g.node(n2)->pos;
        const int beforeMove = f.steps();
        const QPointF at = ne->tileRect(*g.node(n2)).center();
        const double zoom = ne->zoom();
        drag(ne, at, at + QPointF(30, 40));
        QCOMPARE(f.steps(), beforeMove + 1);
        const QPointF after = f.grade(clip).node(n2)->pos;
        QVERIFY(std::abs(after.x() - before.x() - 30 / zoom) < 1.5);
        QVERIFY(std::abs(after.y() - before.y() - 40 / zoom) < 1.5);
        QCOMPARE(ne->zoom(), zoom); // moving a node keeps the view
        // Ctrl+D bypasses the selected node, Ctrl+Z restores it.
        f.session()->selectNode(n2);
        f.key(Qt::Key_D, Qt::ControlModifier);
        QVERIFY(!f.grade(clip).node(n2)->enabled);
        f.action("edit.undo")->trigger();
        QVERIFY(f.grade(clip).node(n2)->enabled);
        // Shift+D: bypass all (viewer state, no undo step).
        const int beforeBypass = f.steps();
        f.key(Qt::Key_D, Qt::ShiftModifier);
        QVERIFY(f.action("color.bypass_all")->isChecked());
        QCOMPARE(f.steps(), beforeBypass);
        f.key(Qt::Key_D, Qt::ShiftModifier);
        QVERIFY(!f.action("color.bypass_all")->isChecked());
        // Alt+P / Alt+L add mixers shown as small tiles with their input ports.
        f.key(Qt::Key_P, Qt::AltModifier);
        g = f.grade(clip);
        const GradeNode* mixer = nullptr;
        for (const GradeNode& n : g.nodes)
            mixer = n.type == GradeNodeType::Parallel ? &n : mixer;
        QVERIFY(mixer);
        QCOMPARE(mixer->inputs.size(), 2);
        QCOMPARE(ne->hitTest(ne->inputPos(*mixer, 2)).kind, ColorNodeEditor::Hit::Kind::Input);
        // The context menu: add / reset / delete through the color.* commands.
        f.session()->selectNode(n1);
        std::unique_ptr<QMenu> menu(ne->buildContextMenu(ne->tileRect(*g.node(n1)).center(), ne));
        QAction* del = menu->findChild<QAction*>(QStringLiteral("color.delete_node"));
        QAction* reset = menu->findChild<QAction*>(QStringLiteral("color.reset_node"));
        QAction* add = menu->findChild<QAction*>(QStringLiteral("color.add_serial"));
        QVERIFY(del && reset && add);
        QVERIFY(del->isEnabled());
        const int nodes = int(g.nodes.size());
        add->trigger();
        QCOMPARE(int(f.grade(clip).nodes.size()), nodes + 1);
        del->trigger();
        QCOMPARE(int(f.grade(clip).nodes.size()), nodes);
        // The stored positions survive undo / redo exactly.
        const ClipGrade stored = f.grade(clip);
        f.action("edit.undo")->trigger();
        f.action("edit.redo")->trigger();
        QCOMPARE(f.grade(clip), stored);
    }

    void wheelDragUpdatesTheTimelineBeforeRelease()
    {
        // The viewer re-renders on Timeline::changed: it must fire during the drag (spec 44b.x),
        // with the new grade in the timeline, while the undo step is still one at the release.
        Fixture f;
        f.showPage();
        ColorWheel* gain = f.page()->primaries()->wheel(gk::Wheel::Gain);
        const int base = f.steps();
        QSignalSpy changed(f.project.timeline(), &Timeline::changed);
        const QPointF c = gain->discRect().center();
        QTest::mousePress(gain, Qt::LeftButton, {}, c.toPoint());
        QTest::mouseMove(gain, (c + QPointF(-4, 3)).toPoint());
        QTest::mouseMove(gain, (c + QPointF(-9, 6)).toPoint());
        QTest::mouseMove(gain, (c + QPointF(-14, 9)).toPoint());
        QVERIFY(changed.count() >= 2);
        const ClipGrade live = f.grade(f.clips[0]);
        QVERIFY(!live.nodes.isEmpty());
        QVERIFY(live.nodes[0].value(gk::GainR, 1) != 1.0 || live.nodes[0].value(gk::GainB, 1) != 1.0);
        QCOMPARE(f.steps(), base); // not pushed yet
        QTest::mouseRelease(gain, Qt::LeftButton, {}, (c + QPointF(-14, 9)).toPoint());
        QCOMPARE(f.steps(), base + 1);
    }

    void wheelAndBarsDragsAreOneStep()
    {
        Fixture f;
        f.showPage();
        PrimariesPanel* prim = f.page()->primaries();
        ColorWheel* lift = prim->wheel(gk::Wheel::Lift);
        const int base = f.steps();
        const QPointF c = lift->discRect().center();
        drag(lift, c, c + QPointF(-15, 10));
        QCOMPARE(f.steps(), base + 1);
        ClipGrade g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes.size(), 1); // the first node came with the drag
        QVERIFY(g.nodes[0].value(gk::LiftB, 0) != 0.0);
        // The master strip.
        const QPointF s = lift->stripRect().center();
        drag(lift, s, s + QPointF(20, 0));
        QCOMPARE(f.steps(), base + 2);
        QVERIFY(f.grade(f.clips[0]).nodes[0].value(gk::LiftY, 0) > 0.0);
        // Double-click resets the wheel (one step).
        QTest::mouseDClick(lift, Qt::LeftButton, {}, c.toPoint());
        g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes[0].value(gk::LiftR, 0), 0.0);
        QCOMPARE(g.nodes[0].value(gk::LiftY, 0), 0.0);
        // The bars view: a drag on the Gain R bar is one step and raises Gain R only.
        prim->setBarsMode(true);
        QVERIFY(prim->barsMode());
        PrimaryBars* bars = prim->bars(gk::Wheel::Gain);
        bars->resize(140, 160);
        const int beforeBars = f.steps();
        const QPointF r = bars->barRect(0).center();
        drag(bars, r, r - QPointF(0, 30));
        QCOMPARE(f.steps(), beforeBars + 1);
        g = f.grade(f.clips[0]);
        QVERIFY(g.nodes[0].value(gk::GainR, 0) > 1.0);
        QCOMPARE(g.nodes[0].value(gk::GainG, 0), 1.0);
        // The numeric fields follow.
        QCOMPARE(prim->field(gk::GainR)->value(), g.nodes[0].value(gk::GainR, 0));
        // A scrubbed field is one step too.
        ValueField* hue = prim->field(gk::Hue);
        const int beforeField = f.steps();
        Q_EMIT hue->scrubStarted();
        Q_EMIT hue->valueScrubbed(5);
        Q_EMIT hue->valueScrubbed(10);
        Q_EMIT hue->scrubFinished();
        QCOMPARE(f.steps(), beforeField + 1);
        QCOMPARE(f.grade(f.clips[0]).nodes[0].value(gk::Hue, 0), 10.0);
    }

    void curvesPalette()
    {
        Fixture f;
        f.showPage();
        f.page()->showPalette(ColorPage::Palette::Curves);
        ColorCurvesPanel* panel = f.page()->curves();
        GradeCurveEditor* ed = panel->editor();
        ed->resize(300, 240);
        // Linked R editing writes R, G and B.
        panel->showCurve(GradeCurve::Red);
        QVERIFY(ed->isLinked());
        const QRectF plot = ed->plotRect();
        QTest::mouseClick(ed, Qt::LeftButton, {},
            QPoint(
                int(plot.left() + plot.width() * 0.4), int(plot.bottom() - plot.height() * 0.6)));
        ClipGrade g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes[0].curves[GradeCurve::Red].size(), 1);
        QCOMPARE(g.nodes[0].curves[GradeCurve::Green], g.nodes[0].curves[GradeCurve::Red]);
        QCOMPARE(g.nodes[0].curves[GradeCurve::Blue], g.nodes[0].curves[GradeCurve::Red]);
        QVERIFY(g.nodes[0].curves[GradeCurve::Luma].isEmpty());
        // Hue vs Sat: a point, then removed by right-click; reset clears the curve.
        panel->showCurve(GradeCurve::HueSat);
        const QRectF hp = ed->plotRect();
        const QPoint at(int(hp.left() + hp.width() * 0.3), int(hp.bottom() - hp.height() * 0.8));
        QTest::mouseClick(ed, Qt::LeftButton, {}, at);
        // A first point on an HSL curve brings two neutral neighbours.
        QCOMPARE(f.grade(f.clips[0]).nodes[0].curves[GradeCurve::HueSat].size(), 3);
        QTest::mouseClick(ed, Qt::RightButton, {}, at);
        QCOMPARE(f.grade(f.clips[0]).nodes[0].curves[GradeCurve::HueSat].size(), 2);
        panel->resetCurve();
        QVERIFY(f.grade(f.clips[0]).nodes[0].curves[GradeCurve::HueSat].isEmpty());
        panel->showCurve(GradeCurve::Red);
        panel->resetCurve();
        g = f.grade(f.clips[0]);
        QVERIFY(g.nodes[0].curves[GradeCurve::Red].isEmpty());
        QVERIFY(g.nodes[0].curves[GradeCurve::Blue].isEmpty());
    }

    void qualifierPickRanges()
    {
        // The pure maths: pick, add, remove.
        const float red[3] = {0.86f, 0.16f, 0.16f};
        const float blue[3] = {0.16f, 0.24f, 0.86f};
        const qualifierpick::Hsl r = qualifierpick::toHsl(red);
        const qualifierpick::Hsl b = qualifierpick::toHsl(blue);
        GradeQualifier q = qualifierpick::apply({}, {r}, qualifierpick::Mode::Pick);
        QVERIFY(q.enabled);
        QVERIFY(grademath::qualifierKey(q, red) > 0.99f);
        QVERIFY(grademath::qualifierKey(q, blue) < 0.01f);
        q = qualifierpick::apply(q, {b}, qualifierpick::Mode::Add);
        QVERIFY(grademath::qualifierKey(q, red) > 0.99f);
        QVERIFY(grademath::qualifierKey(q, blue) > 0.99f);
        q = qualifierpick::apply(q, {b}, qualifierpick::Mode::Remove);
        QVERIFY(grademath::qualifierKey(q, blue) < 0.5f);
        QVERIFY(grademath::qualifierKey(q, red) > 0.99f);
        // Hue arcs wrap around red.
        const auto [centre, width] = qualifierpick::hueArc({0.97, 0.02});
        QVERIFY(width < 0.1);
        QVERIFY(centre > 0.98 || centre < 0.01);

        // On the page: the eyedropper over the viewer picks from the node's input picture.
        Fixture f;
        f.showPage();
        f.splitInput();
        QualifierPanel* qp = f.page()->qualifier();
        f.page()->showPalette(ColorPage::Palette::Qualifier);
        qp->setPickMode(qualifierpick::Mode::Pick);
        ColorViewerOverlay* ov = f.page()->overlay();
        QVERIFY(ov->pickMode());
        const QRectF pic = ov->pictureRect();
        QVERIFY(pic.width() > 50);
        const int base = f.steps();
        QTest::mouseClick(ov, Qt::LeftButton, {},
            QPoint(int(pic.left() + pic.width() * 0.25), int(pic.center().y())));
        QCoreApplication::processEvents();
        ClipGrade g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes.size(), 1); // the pick gave the ungraded clip its first node
        QVERIFY(g.nodes[0].qualifier.enabled);
        QVERIFY(grademath::qualifierKey(g.nodes[0].qualifier, red) > 0.99f);
        QVERIFY(grademath::qualifierKey(g.nodes[0].qualifier, blue) < 0.01f);
        QVERIFY(f.steps() > base);
        // Pick + over the blue half: both selected (one more step).
        qp->setPickMode(qualifierpick::Mode::Add);
        const int beforeAdd = f.steps();
        drag(ov, QPointF(pic.left() + pic.width() * 0.7, pic.top() + 20),
            QPointF(pic.left() + pic.width() * 0.9, pic.bottom() - 20));
        QCOMPARE(f.steps(), beforeAdd + 1);
        g = f.grade(f.clips[0]);
        QVERIFY(grademath::qualifierKey(g.nodes[0].qualifier, blue) > 0.99f);
        // Highlight in black and white: the matte is white on the selection.
        qp->setPickMode(std::nullopt);
        f.page()->setHighlight(true);
        QVERIFY(f.action("color.highlight")->isChecked());
        QVERIFY(!ov->highlightImage().isNull());
        f.session()->setQualifier(
            0, qualifierpick::apply({}, {r}, qualifierpick::Mode::Pick), QStringLiteral("Q"));
        f.page()->refreshPictures();
        const QImage grey = ov->highlightImage();
        // Grey mode: the red half graded, the blue half flat grey.
        const QColor blueSide = grey.pixelColor(grey.width() * 3 / 4, grey.height() / 2);
        QVERIFY2(std::abs(blueSide.red() - blueSide.blue()) < 4, qPrintable(blueSide.name()));
        // Black / white.
        auto* mode = qp->findChild<QComboBox*>(QStringLiteral("QualifierHighlightMode"));
        QVERIFY(mode);
        mode->setCurrentIndex(1);
        const QImage bw = ov->highlightImage();
        QVERIFY(qGray(bw.pixel(bw.width() / 4, bw.height() / 2)) > 250);
        QVERIFY(qGray(bw.pixel(bw.width() * 3 / 4, bw.height() / 2)) < 5);
        f.key(Qt::Key_H, Qt::ShiftModifier); // Shift+H toggles it off
        QVERIFY(!f.page()->isHighlighting());
        QVERIFY(ov->highlightImage().isNull());
    }

    void windowHandles()
    {
        Fixture f;
        f.showPage();
        f.page()->showPalette(ColorPage::Palette::Windows);
        WindowsPanel* wp = f.page()->windows();
        ColorViewerOverlay* ov = f.page()->overlay();
        QVERIFY(ov->windowControls());
        // The Circle button adds a circle window (and the first node) as one step.
        const int base = f.steps();
        wp->shapeButton(GradeWindowShape::Circle)->click();
        QCOMPARE(f.steps(), base + 1);
        const GradeWindow* w = f.session()->window();
        QVERIFY(w);
        QCOMPARE(w->shape, GradeWindowShape::Circle);
        QCOMPARE(wp->list()->count(), 1);
        // Spec 23.21: the capped list elides instead of scrolling sideways.
        QCOMPARE(wp->list()->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QVERIFY(wp->list()->visualItemRect(wp->list()->item(0)).right()
            < wp->list()->viewport()->width());
        // Spec 23.23: its value fields are as wide as their widest value.
        for (const ValueField* field : wp->findChildren<ValueField*>())
            QVERIFY(field->width() >= field->sizeHint().width());
        const quint64 id = w->id;
        // Width handle: drag right = wider; one step.
        GradeWindow win = *w;
        QPointF hw = ov->handlePos(win, ColorViewerOverlay::Handle::Width);
        QCOMPARE(ov->hitTest(hw).handle, ColorViewerOverlay::Handle::Width);
        const double width0 = win.value(wk::Width, 0);
        int before = f.steps();
        drag(ov, hw, hw + QPointF(20, 0));
        QCOMPARE(f.steps(), before + 1);
        win = *f.session()->window(id);
        QVERIFY(win.value(wk::Width, 0) > width0);
        QCOMPARE(win.value(wk::Height, 0), 0.5);
        // Move: drag inside.
        const QPointF centre = ov->handlePos(win, ColorViewerOverlay::Handle::Move);
        drag(ov, centre + QPointF(3, 3), centre + QPointF(33, 3));
        win = *f.session()->window(id);
        QVERIFY(win.value(wk::CenterX, 0) > 0.55);
        QVERIFY(std::abs(win.value(wk::CenterY, 0) - 0.5) < 1e-6);
        // Rotate: the knob dragged to the right of the centre = 90 degrees.
        const QPointF knob = ov->handlePos(win, ColorViewerOverlay::Handle::Rotate);
        QCOMPARE(ov->hitTest(knob).handle, ColorViewerOverlay::Handle::Rotate);
        const QPointF c2 = ov->handlePos(win, ColorViewerOverlay::Handle::Move);
        drag(ov, knob, c2 + QPointF(60, 0));
        win = *f.session()->window(id);
        QVERIFY2(std::abs(win.value(wk::Rotation, 0) - 90.0) < 3.0,
            qPrintable(QString::number(win.value(wk::Rotation, 0))));
        // Softness handle.
        const QPointF soft = ov->handlePos(win, ColorViewerOverlay::Handle::Soft);
        QCOMPARE(ov->hitTest(soft).handle, ColorViewerOverlay::Handle::Soft);
        // The fields show the values; Invert toggles; a second window joins the node.
        QCOMPARE(wp->field(wk::Rotation)->value(), win.value(wk::Rotation, 0));
        auto* invert = wp->findChild<QToolButton*>(QStringLiteral("WindowInvert"));
        invert->click();
        QVERIFY(f.session()->window(id)->invert);
        wp->shapeButton(GradeWindowShape::Polygon)->click();
        QCOMPARE(f.session()->currentNode()->windows.size(), 2);
        QCOMPARE(wp->list()->count(), 2);
        // Polygon: drag a point, insert one with a double-click on an edge, delete with a
        // right-click.
        GradeWindow poly = *f.session()->window();
        QCOMPARE(poly.shape, GradeWindowShape::Polygon);
        const QPointF p0 = ov->handlePos(poly, ColorViewerOverlay::Handle::Point, 0);
        QCOMPARE(ov->hitTest(p0).handle, ColorViewerOverlay::Handle::Point);
        before = f.steps();
        drag(ov, p0, p0 + QPointF(0, -15));
        QCOMPARE(f.steps(), before + 1);
        poly = *f.session()->window();
        QVERIFY(poly.points[0].pos.y() < -0.5);
        const QPointF a = ov->handlePos(poly, ColorViewerOverlay::Handle::Point, 1);
        const QPointF b = ov->handlePos(poly, ColorViewerOverlay::Handle::Point, 2);
        QTest::mouseDClick(ov, Qt::LeftButton, {}, ((a + b) / 2).toPoint());
        QCOMPARE(f.session()->window()->points.size(), 5);
        poly = *f.session()->window();
        QTest::mouseClick(ov, Qt::RightButton, {},
            ov->handlePos(poly, ColorViewerOverlay::Handle::Point, 2).toPoint());
        QCOMPARE(f.session()->window()->points.size(), 4);
        // Delete removes the selected window.
        wp->findChild<QToolButton*>(QStringLiteral("WindowDelete"))->click();
        QCOMPARE(f.session()->currentNode()->windows.size(), 1);
        // Off the Windows palette the controls hide.
        f.page()->showPalette(ColorPage::Palette::Primaries);
        QVERIFY(!ov->windowControls());
    }

    void curveWindow()
    {
        // Spec 111: the Curve button, tangent handles, smooth / corner, point insertion and
        // deletion, and shape keyframes.
        Fixture f;
        f.showPage();
        f.page()->showPalette(ColorPage::Palette::Windows);
        WindowsPanel* wp = f.page()->windows();
        ColorViewerOverlay* ov = f.page()->overlay();
        QVERIFY(wp->shapeButton(GradeWindowShape::Curve));
        // Every button of the palette explains itself.
        for (const QToolButton* b : wp->findChildren<QToolButton*>())
            QVERIFY2(!b->toolTip().isEmpty(), qPrintable(b->objectName() + b->text()));
        const int base = f.steps();
        wp->shapeButton(GradeWindowShape::Curve)->click();
        QCOMPARE(f.steps(), base + 1);
        QCOMPARE(f.session()->window()->shape, GradeWindowShape::Curve);
        QCOMPARE(f.session()->window()->points.size(), 4);
        // Select point 1 and drag its outgoing tangent: one step, the other side follows.
        GradeWindow win = *f.session()->window();
        const QPointF p1 = ov->handlePos(win, ColorViewerOverlay::Handle::Point, 1);
        QTest::mouseClick(ov, Qt::LeftButton, {}, p1.toPoint());
        QCOMPARE(ov->selectedPoint(), 1);
        const QPointF out = ov->handlePos(win, ColorViewerOverlay::Handle::TangentOut, 1);
        QCOMPARE(ov->hitTest(out).handle, ColorViewerOverlay::Handle::TangentOut);
        int steps = f.steps();
        drag(ov, out, out + QPointF(0, 20));
        QCOMPARE(f.steps(), steps + 1);
        win = *f.session()->window();
        const GradeWindowPoint pt = win.points[1];
        QVERIFY(pt.outHandle.y() > 0.3 * 0.5522847498 + 0.001);
        QVERIFY(std::abs(pt.inHandle.x() * pt.outHandle.y() - pt.inHandle.y() * pt.outHandle.x())
            < 1e-9); // still collinear
        QVERIFY(pt.inHandle.y() < 0.0); // pointing the opposite way
        QVERIFY(pt.smooth);
        // Double-click on the point makes it a corner (no handles), again smooth again.
        const QPointF at = ov->handlePos(win, ColorViewerOverlay::Handle::Point, 1);
        QTest::mouseDClick(ov, Qt::LeftButton, {}, at.toPoint());
        win = *f.session()->window();
        QVERIFY(!win.points[1].smooth && !win.points[1].hasHandles());
        QTest::mouseDClick(ov, Qt::LeftButton, {}, at.toPoint());
        QVERIFY(f.session()->window()->points[1].smooth);
        QVERIFY(f.session()->window()->points[1].hasHandles());
        // Double-click on the outline between points 2 and 3 inserts a point and the curve
        // keeps its shape.
        win = *f.session()->window();
        // A point of the outline itself, from the span's own curve.
        QSizeF size;
        ov->pictureToWidget(&size);
        const double fw = win.value(wk::Width, 0) * size.width();
        const double fh = win.value(wk::Height, 0) * size.height();
        const QPointF local = windowSpanPoint(win.points, 2, 0.5);
        const QPointF outline = ov->windowToWidget(win)->map(QPointF(local.x() * fw, local.y() * fh));
        QTest::mouseDClick(ov, Qt::LeftButton, {}, outline.toPoint());
        QCOMPARE(f.session()->window()->points.size(), 5);
        QVERIFY(f.session()->window()->points[3].smooth);
        // Right-click deletes it again.
        win = *f.session()->window();
        QTest::mouseClick(ov, Qt::RightButton, {},
            ov->handlePos(win, ColorViewerOverlay::Handle::Point, 3).toPoint());
        QCOMPARE(f.session()->window()->points.size(), 4);
        // Shape keyframes: Key Shape creates a key; moving a point at another frame writes
        // a second key and the shape differs between the frames.
        auto* keyButton = wp->findChild<QToolButton*>(QStringLiteral("WindowKeyShape"));
        QVERIFY(keyButton->isEnabled());
        f.project.timeline()->setPlayhead(0);
        keyButton->click();
        QCOMPARE(f.session()->window()->shapeKeys.size(), 1);
        f.project.timeline()->setPlayhead(10);
        win = *f.session()->window();
        const QPointF q0 = ov->handlePos(ov->shown(win), ColorViewerOverlay::Handle::Point, 0);
        drag(ov, q0, q0 + QPointF(0, -20));
        QCOMPARE(f.session()->window()->shapeKeys.size(), 2);
        const GradeWindow keyed = *f.session()->window();
        QVERIFY(keyed.shapeAt(10)[0].pos.y() < keyed.shapeAt(0)[0].pos.y());
        // Deleting a point removes it from every key; one undo step.
        steps = f.steps();
        QTest::mouseClick(ov, Qt::RightButton, {},
            ov->handlePos(ov->shown(keyed), ColorViewerOverlay::Handle::Point, 2).toPoint());
        QCOMPARE(f.steps(), steps + 1);
        for (const auto& key : f.session()->window()->shapeKeys)
            QCOMPARE(key.size(), 3);
        QCOMPARE(f.session()->window()->points.size(), 3);
        // Clear Keys flattens to the shape shown now.
        wp->findChild<QToolButton*>(QStringLiteral("WindowClearShapeKeys"))->click();
        QVERIFY(f.session()->window()->shapeKeys.isEmpty());
    }

    void galleryStillApplyAndWipe()
    {
        Fixture f;
        f.showPage();
        // A still with a grade (desaturate).
        ClipGrade still;
        const quint64 node = gradeops::initialize(still);
        still.node(node)->params.animated(gk::Saturation, 1.0).setValue(0, 0.0);
        QImage img(64, 36, QImage::Format_RGB32);
        img.fill(QColor(90, 90, 90));
        const quint64 id = f.project.gallery()->addStill(img, still);
        ColorGallery* gal = f.page()->gallery();
        QCOMPARE(gal->list()->count(), 1);
        // Middle-click applies it to the current clip (one step); a double-click wipes with
        // it instead (bughunt5, spec 44b.27), a second one ends the wipe.
        const int base = f.steps();
        QListWidgetItem* item = gal->list()->item(0);
        const QPoint at = gal->list()->visualItemRect(item).center();
        QTest::mouseClick(gal->list()->viewport(), Qt::MiddleButton, {}, at);
        QCOMPARE(f.grade(f.clips[0]), still);
        QCOMPARE(f.steps(), base + 1);
        QTest::mouseClick(gal->list()->viewport(), Qt::LeftButton, {}, at);
        QTest::mouseDClick(gal->list()->viewport(), Qt::LeftButton, {}, at);
        QVERIFY(f.page()->overlay()->isWiping());
        QCOMPARE(f.steps(), base + 1);
        QTest::qWait(QApplication::doubleClickInterval() + 50); // a new double-click
        QTest::mouseClick(gal->list()->viewport(), Qt::LeftButton, {}, at);
        QTest::mouseDClick(gal->list()->viewport(), Qt::LeftButton, {}, at);
        QVERIFY(!f.page()->overlay()->isWiping());
        // Middle-click applies it to the clip being graded.
        f.session()->gradeClip(f.clips[1]);
        QTest::mouseClick(gal->list()->viewport(), Qt::MiddleButton, {}, at);
        QCOMPARE(f.grade(f.clips[1]), still);
        // The menu applies it to the clips selected in the strip.
        f.session()->setSelectedClips({f.clips[2]});
        std::unique_ptr<QMenu> menu(gal->buildMenu(id, gal));
        menu->findChild<QAction*>(QStringLiteral("GalleryApplySelected"))->trigger();
        QCOMPARE(f.grade(f.clips[2]), still);
        // Wipe: the overlay shows the still left of the line; Ctrl+W toggles.
        menu->findChild<QAction*>(QStringLiteral("GalleryWipe"))->trigger();
        QVERIFY(f.page()->overlay()->isWiping());
        QVERIFY(f.action("color.wipe")->isChecked());
        f.key(Qt::Key_W, Qt::ControlModifier);
        QVERIFY(!f.page()->overlay()->isWiping());
        f.key(Qt::Key_W, Qt::ControlModifier);
        QVERIFY(f.page()->overlay()->isWiping());
        // Deleting the still ends the wipe.
        f.project.gallery()->removeStill(id);
        QCoreApplication::processEvents();
        QVERIFY(!f.page()->overlay()->isWiping());
    }

    // Spec 151: albums of the gallery and the PowerGrade albums shared by every project.
    void galleryAlbumsAndPowerGrade()
    {
        Fixture f;
        f.showPage();
        ColorGallery* gal = f.page()->gallery();
        QTreeWidget* tree = gal->albums();
        // Stills (with its albums), PowerGrade and Memories, like Resolve's album list.
        QCOMPARE(tree->topLevelItemCount(), 3);
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("Stills"));
        QCOMPARE(tree->topLevelItem(1)->text(0), QStringLiteral("PowerGrade"));
        QCOMPARE(tree->topLevelItem(2)->text(0), QStringLiteral("Memories"));
        QVERIFY(gal->source() == ColorGallery::Source::Project);

        ClipGrade look;
        const quint64 node = gradeops::initialize(look);
        look.node(node)->params.animated(gk::Saturation, 1.0).setValue(0, 0.2);
        QImage img(64, 36, QImage::Format_RGB32);
        img.fill(QColor(90, 90, 90));
        const quint64 id = f.project.gallery()->addStill(img, look);

        // A new album is shown at once and empty; the still stays in its own album.
        const quint64 album = gal->newAlbum(ColorGallery::Source::Project, QStringLiteral("Night"));
        QVERIFY(album > 1);
        QCOMPARE(gal->album(), album);
        QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->child(0)->text(0), QStringLiteral("Night"));
        QCOMPARE(gal->list()->count(), 0);
        // Moving between albums (what dropping a still on an album does).
        QCOMPARE(gal->dropStill(id, ColorGallery::Source::Project, album), id);
        QCOMPARE(gal->list()->count(), 1); // the list shows the album being viewed
        gal->selectAlbum(ColorGallery::Source::Project, album);
        QCOMPARE(gal->list()->count(), 1);
        gal->selectAlbum(ColorGallery::Source::Project, 1);
        QCOMPARE(gal->list()->count(), 0);

        // The menu moves and copies, also into the PowerGrade album.
        gal->selectAlbum(ColorGallery::Source::Project, album);
        std::unique_ptr<QMenu> menu(gal->buildMenu(id, gal));
        auto* move = menu->findChild<QMenu*>(QStringLiteral("GalleryMoveMenu"));
        QVERIFY(move && move->findChild<QAction*>(QStringLiteral("GalleryMove_1")));
        QVERIFY(!move->findChild<QAction*>(QStringLiteral("GalleryMove_%1").arg(album))); // it is there
        auto* copy = menu->findChild<QMenu*>(QStringLiteral("GalleryCopyMenu"));
        QVERIFY(copy);
        copy->findChild<QAction*>(QStringLiteral("GalleryCopy_G1"))->trigger();
        QCOMPARE(f.project.gallery()->stills().size(), 2);
        copy->findChild<QAction*>(QStringLiteral("GalleryCopy_P1"))->trigger();
        const auto* pg = f.project.powerGrades()->gallery();
        QCOMPARE(pg->stills().size(), 1);
        QCOMPARE(pg->stills().first().grade, look);
        QVERIFY(QFile::exists(f.project.powerGrades()->filePath()));
        QCOMPARE(f.project.gallery()->stills().size(), 2); // the project keeps its own
        move->findChild<QAction*>(QStringLiteral("GalleryMove_1"))->trigger();
        QCOMPARE(gal->list()->count(), 0);

        // PowerGrade stills are listed, applied (one undo step) and wiped like the others.
        gal->selectAlbum(ColorGallery::Source::PowerGrade, 1);
        QCOMPARE(gal->list()->count(), 1);
        const quint64 pgId = gal->list()->item(0)->data(Qt::UserRole).toULongLong();
        QVERIFY(pgId & ColorGallery::kPowerGradeBit);
        QVERIFY(gal->stillFor(pgId));
        const int base = f.steps();
        const QPoint at = gal->list()->visualItemRect(gal->list()->item(0)).center();
        QTest::mouseClick(gal->list()->viewport(), Qt::MiddleButton, {}, at);
        QCOMPARE(f.grade(f.clips[0]), look);
        QCOMPARE(f.steps(), base + 1);
        gal->setWipeStill(pgId);
        QVERIFY(f.page()->overlay()->isWiping());
        // Dropping a PowerGrade still on a project album copies it (the original stays).
        const quint64 back = gal->dropStill(pgId, ColorGallery::Source::Project, 1);
        QVERIFY(back && !(back & ColorGallery::kPowerGradeBit));
        QCOMPARE(pg->stills().size(), 1);

        // Albums are renamed and deleted from their menu; the first one cannot go.
        std::unique_ptr<QMenu> first(gal->buildAlbumMenu(ColorGallery::Source::Project, 1, gal));
        QVERIFY(!first->findChild<QAction*>(QStringLiteral("GalleryDeleteAlbum"))->isEnabled());
        QVERIFY(first->findChild<QAction*>(QStringLiteral("GalleryNewAlbum")));
        QVERIFY(first->findChild<QAction*>(QStringLiteral("GalleryImport")));
        const quint64 empty = gal->newAlbum(ColorGallery::Source::PowerGrade, QStringLiteral("Empty"));
        std::unique_ptr<QMenu> custom(
            gal->buildAlbumMenu(ColorGallery::Source::PowerGrade, empty, gal));
        QVERIFY(custom->findChild<QAction*>(QStringLiteral("GalleryDeleteAlbum"))->isEnabled());
        custom->findChild<QAction*>(QStringLiteral("GalleryDeleteAlbum"))->trigger();
        QVERIFY(!pg->album(empty));
        QVERIFY(gal->source() == ColorGallery::Source::Project); // fell back to the first album
        QCOMPARE(tree->topLevelItem(1)->childCount(), 0);

        // The PowerGrade store is the user's, not the project's: another project sees it.
        Project other;
        other.setPowerGradeFile(f.project.powerGrades()->filePath());
        QCOMPARE(other.powerGrades()->gallery()->stills().size(), 1);
        QVERIFY(other.gallery()->stills().isEmpty());
    }

    // Spec 151: memories A to Z, Alt+1..8 save and Ctrl+1..8 apply.
    void galleryMemories()
    {
        Fixture f;
        f.showPage();
        ColorGallery* gal = f.page()->gallery();
        QCOMPARE(f.action("color.memory_save_a")->shortcut(), QKeySequence(QStringLiteral("Alt+1")));
        QCOMPARE(f.action("color.memory_save_h")->shortcut(), QKeySequence(QStringLiteral("Alt+8")));
        QCOMPARE(f.action("color.memory_apply_a")->shortcut(), QKeySequence(QStringLiteral("Ctrl+1")));
        QCOMPARE(f.action("color.memory_apply_h")->shortcut(), QKeySequence(QStringLiteral("Ctrl+8")));

        // Grade clip 1, save it into memory A with Alt+1 and clip 2's (empty) grade into B.
        ClipGrade look;
        const quint64 node = gradeops::initialize(look);
        look.node(node)->params.setBase(gk::Saturation, 0.4);
        f.session()->gradeClip(f.clips[0]);
        QVERIFY(f.session()->commit(look, QStringLiteral("Look")));
        f.key(Qt::Key_1, Qt::AltModifier);
        QVERIFY(f.project.gallery()->memory(0).used);
        QCOMPARE(f.project.gallery()->memory(0).grade, look);
        QVERIFY(f.project.isDirty()); // memories are saved with the project
        f.session()->gradeClip(f.clips[1]);
        f.action("color.memory_save_b")->trigger();
        QVERIFY(f.project.gallery()->memory(1).used);

        // Ctrl+1 gives the clip being graded the grade of A: one undo step.
        QVERIFY(f.grade(f.clips[1]).isEmpty());
        const int base = f.steps();
        f.key(Qt::Key_1, Qt::ControlModifier);
        QCOMPARE(f.grade(f.clips[1]), look);
        QCOMPARE(f.steps(), base + 1);
        f.project.undoStack()->undo();
        QVERIFY(f.grade(f.clips[1]).isEmpty());
        // An empty memory does nothing; selected clips all get it in one step.
        f.key(Qt::Key_3, Qt::ControlModifier);
        QCOMPARE(f.steps(), base); // (the undone step stays on the stack)
        f.session()->setSelectedClips({f.clips[1], f.clips[2]});
        f.key(Qt::Key_1, Qt::ControlModifier);
        QCOMPARE(f.grade(f.clips[1]), look);
        QCOMPARE(f.grade(f.clips[2]), look);
        f.project.undoStack()->undo();
        QVERIFY(f.grade(f.clips[1]).isEmpty() && f.grade(f.clips[2]).isEmpty());

        // The Memories album: tiles A to Z, used ones filled; middle-click applies, the menu clears.
        gal->selectAlbum(ColorGallery::Source::Memories);
        QCOMPARE(gal->list()->count(), kGalleryMemoryCount);
        QCOMPARE(gal->list()->item(0)->text(), QStringLiteral("A"));
        QCOMPARE(gal->list()->item(25)->text(), QStringLiteral("Z"));
        QCOMPARE(gal->currentStill(), quint64(0));
        f.session()->setSelectedClips({});
        f.session()->gradeClip(f.clips[2]);
        const QPoint at = gal->list()->visualItemRect(gal->list()->item(0)).center();
        QTest::mouseClick(gal->list()->viewport(), Qt::MiddleButton, {}, at);
        QCOMPARE(f.grade(f.clips[2]), look);
        std::unique_ptr<QMenu> tile(gal->buildMemoryMenu(0, gal));
        QVERIFY(tile->findChild<QAction*>(QStringLiteral("MemoryApply"))->isEnabled());
        tile->findChild<QAction*>(QStringLiteral("MemoryClear"))->trigger();
        QVERIFY(!f.project.gallery()->memory(0).used);
        std::unique_ptr<QMenu> emptyTile(gal->buildMemoryMenu(0, gal));
        QVERIFY(!emptyTile->findChild<QAction*>(QStringLiteral("MemoryApply"))->isEnabled());
        // Memories beyond H have no key but a tile menu.
        std::unique_ptr<QMenu> zTile(gal->buildMemoryMenu(25, gal));
        zTile->findChild<QAction*>(QStringLiteral("MemorySave"))->trigger();
        QVERIFY(f.project.gallery()->memory(25).used);
    }

    // Spec 151: Append Node Graph and Display Node Graph.
    void galleryAppendNodeGraph()
    {
        Fixture f;
        f.showPage();
        ColorGallery* gal = f.page()->gallery();
        ClipGrade look;
        const quint64 node = gradeops::initialize(look);
        look.node(node)->label = QStringLiteral("Warm");
        gradeops::addSerialNode(look, node);
        const quint64 id = f.project.gallery()->addStill(QImage(32, 18, QImage::Format_RGB32), look);

        ClipGrade own;
        gradeops::initialize(own);
        own.nodes[0].label = QStringLiteral("Mine");
        QVERIFY(f.session()->commit(own, QStringLiteral("Mine")));
        const int base = f.steps();
        std::unique_ptr<QMenu> menu(gal->buildMenu(id, gal));
        menu->findChild<QAction*>(QStringLiteral("GalleryAppend"))->trigger();
        QCOMPARE(f.grade(f.clips[0]).nodes.size(), qsizetype(3));
        QCOMPARE(f.grade(f.clips[0]).nodes[0].label, QStringLiteral("Mine"));
        QCOMPARE(f.steps(), base + 1);
        f.project.undoStack()->undo();
        QCOMPARE(f.grade(f.clips[0]).nodes.size(), qsizetype(1));

        // Selected clips: each keeps its own nodes, one undo step for all.
        f.session()->setSelectedClips({f.clips[0], f.clips[1]});
        const int before = f.steps();
        QCOMPARE(gal->applyStill(id, f.session()->selectedClips(), true), 2);
        QCOMPARE(f.grade(f.clips[0]).nodes.size(), qsizetype(3));
        QCOMPARE(f.grade(f.clips[1]).nodes.size(), qsizetype(2)); // had none: a copy
        QCOMPARE(f.steps(), before + 1);
        f.project.undoStack()->undo();
        QVERIFY(f.grade(f.clips[1]).isEmpty());
        QCOMPARE(f.grade(f.clips[0]).nodes.size(), qsizetype(1));

        // The timeline grade can be appended to as well (Timeline target).
        f.session()->setSelectedClips({});
        f.session()->setTarget(ColorSession::Target::Timeline);
        QCOMPARE(gal->applyStill(id, {}, true), 1);
        QCOMPARE(f.project.timelineGrade(f.project.activeTimelineUid()).nodes.size(), qsizetype(2));
        f.session()->setTarget(ColorSession::Target::Clip);

        // Display Node Graph opens a window that paints the nodes.
        menu->findChild<QAction*>(QStringLiteral("GalleryDisplayGraph"))->trigger();
        auto* graph = gal->findChild<QDialog*>(QStringLiteral("GalleryNodeGraphDialog"));
        QVERIFY(graph);
        QVERIFY(graph->findChild<QWidget*>(QStringLiteral("GalleryNodeGraph")));
        QVERIFY(!graph->grab().isNull());
        graph->close();
    }

    // Spec 151: Import Still and Export Still (picture plus grade file).
    void galleryImportExport()
    {
        Fixture f;
        f.showPage();
        ColorGallery* gal = f.page()->gallery();
        QTemporaryDir dir;
        ClipGrade look;
        const quint64 node = gradeops::initialize(look);
        look.node(node)->params.setBase(gk::Saturation, 1.7);
        QImage img(160, 90, QImage::Format_RGB32);
        img.fill(QColor(30, 120, 60));
        const quint64 id = f.project.gallery()->addStill(img, look);
        f.project.gallery()->renameStill(id, QStringLiteral("Forest"));

        const QString png = dir.filePath(QStringLiteral("forest.png"));
        QString error;
        QVERIFY2(gal->exportStill(id, png, &error), qPrintable(error));
        QVERIFY(QFile::exists(png));
        QVERIFY(QFile::exists(gallerystills::gradeFileFor(png)));
        QVERIFY(!gal->exportStill(0, png, &error));

        // Imported into the album being shown (here a new one), also a PowerGrade album.
        const quint64 album = gal->newAlbum(ColorGallery::Source::Project, QStringLiteral("In"));
        const quint64 in = gal->importStill(png, &error);
        QVERIFY2(in, qPrintable(error));
        QCOMPARE(gal->list()->count(), 1);
        QCOMPARE(f.project.gallery()->still(in)->album, album);
        QCOMPARE(f.project.gallery()->still(in)->grade, look);
        QCOMPARE(f.project.gallery()->still(in)->name, QStringLiteral("Forest"));
        gal->selectAlbum(ColorGallery::Source::PowerGrade, 1);
        const quint64 pgIn = gal->importStill(png, &error);
        QVERIFY(pgIn & ColorGallery::kPowerGradeBit);
        QCOMPARE(f.project.powerGrades()->gallery()->stills().size(), 1);
        QCOMPARE(gal->stillFor(pgIn)->grade, look);
        QVERIFY(!gal->importStill(dir.filePath(QStringLiteral("none.png")), &error));
        QVERIFY(!error.isEmpty());
        // The still's context menu has both commands.
        std::unique_ptr<QMenu> menu(gal->buildMenu(pgIn, gal));
        QVERIFY(menu->findChild<QAction*>(QStringLiteral("GalleryImport")));
        QVERIFY(menu->findChild<QAction*>(QStringLiteral("GalleryExport")));
        QVERIFY(gal->deleteStill(pgIn));
        QVERIFY(f.project.powerGrades()->gallery()->stills().isEmpty());
    }

    void clipStripSelectsTheGradedClip()
    {
        Fixture f;
        f.showPage();
        ColorClipStrip* strip = f.page()->clipStrip();
        QCOMPARE(strip->count(), 3);
        QCOMPARE(f.session()->clipId(), f.clips[0]);
        // Clicking the second thumbnail grades that clip and moves the playhead into it.
        QTest::mouseClick(
            strip->viewport(), Qt::LeftButton, {}, strip->visualItemRect(strip->item(1)).center());
        QCOMPARE(f.session()->clipId(), f.clips[1]);
        QVERIFY(f.project.timeline()->playhead() >= 50 && f.project.timeline()->playhead() < 100);
        // The mini timeline moves the playhead too.
        ColorMiniTimeline* mini = f.page()->miniTimeline();
        QTest::mouseClick(
            mini, Qt::LeftButton, {}, QPoint(int(mini->xOf(125)), mini->height() / 2));
        QCOMPARE(f.session()->clipId(), f.clips[2]);
        // Grade clip 3, then middle-click its thumbnail while clip 1 is selected: clip 1 gets it.
        QVERIFY(f.session()->setParam(0, gk::Saturation, 0.3, QStringLiteral("Sat")));
        QTest::mouseClick(
            strip->viewport(), Qt::LeftButton, {}, strip->visualItemRect(strip->item(0)).center());
        QCOMPARE(f.session()->clipId(), f.clips[0]);
        QCOMPARE(f.session()->selectedClips(), QVector<quint64> {f.clips[0]});
        const int base = f.steps();
        QTest::mouseClick(strip->viewport(), Qt::MiddleButton, {},
            strip->visualItemRect(strip->item(2)).center());
        QCOMPARE(f.grade(f.clips[0]), f.grade(f.clips[2]));
        QCOMPARE(f.steps(), base + 1);
        // Ctrl+C on clip 1, Ctrl-click clip 2 into the selection, Ctrl+V pastes to both.
        QVERIFY(f.project.setClipGrade(
            f.project.activeTimelineUid(), f.clips[0], ClipGrade {}, QStringLiteral("Reset")));
        QVERIFY(f.session()->setParam(0, gk::Hue, 20.0, QStringLiteral("Hue")));
        f.key(Qt::Key_C, Qt::ControlModifier);
        QVERIFY(f.session()->hasCopiedGrade());
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, Qt::ControlModifier,
            strip->visualItemRect(strip->item(1)).center());
        QCOMPARE(f.session()->selectedClips().size(), 2);
        QCOMPARE(f.session()->clipId(), f.clips[0]); // Ctrl-click only selects
        const int beforePaste = f.steps();
        f.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(f.steps(), beforePaste + 1);
        QCOMPARE(f.grade(f.clips[1]), f.grade(f.clips[0]));
        // The current clip is outlined and graded clips carry a mark (item data).
        QVERIFY(strip->item(0)->data(Qt::UserRole + 4).toBool());
        QVERIFY(strip->item(1)->data(Qt::UserRole + 3).toBool());
    }

    /// The viewer's timecode and the clips strip show record timecode (the timeline's starting
    /// timecode added), like the Edit page.
    void timecodesUseTheStartingTimecode()
    {
        Fixture f;
        const TimelineData& d = f.project.timeline()->data();
        QVERIFY(d.startTimecode > 0); // a new project's timeline starts at 01:00:00:00
        f.project.timeline()->setPlayhead(60);
        f.showPage();
        auto* label = f.page()->findChild<QLabel*>(QStringLiteral("ColorTimecode"));
        QVERIFY(label);
        QCOMPARE(label->text(), timecode::formatRecord(60, d));
        QVERIFY(label->text().startsWith(QLatin1String("01:")));
        ColorClipStrip* strip = f.page()->clipStrip();
        QCOMPARE(strip->item(1)->data(Qt::UserRole + 2).toString(), timecode::formatRecord(50, d));
        // A new starting timecode follows at once.
        QVERIFY(f.project.setTimelineStartTimecode(f.project.activeTimelineUid(), 0));
        QCoreApplication::processEvents();
        QCOMPARE(label->text(), timecode::format(60, f.project.timeline()->data().fps));
    }

    void interfaceBarToggles()
    {
        Fixture f;
        f.showPage();
        ColorPage* page = f.page();
        for (const auto& [id, panel] : {std::pair {"view.toggle_gallery", PanelId::Gallery},
                 std::pair {"view.toggle_clips", PanelId::Clips},
                 std::pair {"view.toggle_color_timeline", PanelId::ColorTimeline},
                 std::pair {"view.toggle_nodes", PanelId::Nodes},
                 std::pair {"view.toggle_scopes", PanelId::Scopes}}) {
            QAction* a = f.action(id);
            QVERIFY2(a->isEnabled(), id);
            QVERIFY2(a->isChecked(), id);
            QVERIFY(page->isPanelVisible(panel));
            a->trigger();
            QVERIFY2(!page->isPanelVisible(panel), id);
            QVERIFY(!a->isChecked());
            a->trigger();
            QVERIFY(page->isPanelVisible(panel));
        }
        // Not on the Edit page.
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QVERIFY(!f.action("view.toggle_gallery")->isEnabled());
        // Layout round trip.
        f.action("view.page_color")->trigger();
        page->showPalette(ColorPage::Palette::Luts);
        page->setPanelVisible(PanelId::Gallery, false);
        QSettings settings;
        settings.beginGroup(QStringLiteral("colorLayoutTest"));
        page->saveLayout(settings);
        page->setPanelVisible(PanelId::Gallery, true);
        page->showPalette(ColorPage::Palette::Primaries);
        page->restoreLayout(settings);
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Luts);
        QVERIFY(!page->isPanelVisible(PanelId::Gallery));
    }

    void timelineTargetAndTypingKeys()
    {
        Fixture f;
        f.showPage();
        auto* timeline = f.page()->findChild<QToolButton*>(QStringLiteral("ColorTargetButton"));
        QVERIFY(timeline);
        timeline->click();
        QCOMPARE(f.session()->target(), ColorSession::Target::Timeline);
        QVERIFY(f.action("color.timeline_grade")->isChecked());
        // Clicking a clip in the strip goes back to the clip grade.
        ColorClipStrip* strip = f.page()->clipStrip();
        QTest::mouseClick(
            strip->viewport(), Qt::LeftButton, {}, strip->visualItemRect(strip->item(0)).center());
        QCOMPARE(f.session()->target(), ColorSession::Target::Clip);
        QVERIFY(!timeline->isChecked());
        // Typing in a field keeps Backspace / Ctrl+C for the text (no node deleted).
        f.key(Qt::Key_S, Qt::AltModifier);
        ValueField* contrast = f.page()->primaries()->field(gk::Contrast);
        contrast->startTextEdit();
        auto* edit = contrast->findChild<QLineEdit*>();
        QVERIFY(edit);
        edit->setFocus();
        const int nodes = int(f.grade(f.clips[0]).nodes.size());
        QTest::keyClick(edit, Qt::Key_Backspace);
        QCOMPARE(int(f.grade(f.clips[0]).nodes.size()), nodes);
    }

    void qualifierMatteFinesse()
    {
        // Spec 108: the mode menu, the Matte Finesse fields, the 3D picks, undo and Reset.
        Fixture f;
        f.showPage();
        f.splitInput();
        ColorPage* page = f.page();
        QualifierPanel* qp = page->qualifier();
        page->showPalette(ColorPage::Palette::Qualifier);
        const float red[3] = {0.86f, 0.16f, 0.16f};
        f.session()->setQualifier(0,
            qualifierpick::apply({}, {qualifierpick::toHsl(red)}, qualifierpick::Mode::Pick),
            QStringLiteral("Q"));
        auto* mode = qp->findChild<QComboBox*>(QStringLiteral("QualifierMode"));
        QVERIFY(mode);
        QCOMPARE(mode->count(), 4);
        // Each Matte Finesse field is one undo step and lands in the node's qualifier.
        const std::pair<const char*, double GradeQualifier::*> fields[]
            = {{"cleanBlack", &GradeQualifier::cleanBlack},
                {"cleanWhite", &GradeQualifier::cleanWhite},
                {"shrinkGrow", &GradeQualifier::shrinkGrow},
                {"blurRadius", &GradeQualifier::blurRadius},
                {"inOutRatio", &GradeQualifier::inOutRatio},
                {"denoise", &GradeQualifier::denoise}};
        for (const auto& [name, member] : fields) {
            ValueField* field = qp->field(QLatin1String(name));
            QVERIFY2(field, name);
            const int before = f.steps();
            Q_EMIT field->valueCommitted(-0.4 < field->minimum() ? 0.4 : -0.4);
            QCOMPARE(f.steps(), before + 1);
            const double v = f.session()->currentNode()->qualifier.*member;
            QVERIFY2(std::abs(std::abs(v) - 0.4) < 1e-9, name);
            QCOMPARE(field->value(), v);
        }
        QVERIFY(f.session()->currentNode()->qualifier.hasFinesse());
        // Highlight shows the cleaned matte (the page uses the same function as the export).
        page->setHighlight(true);
        QVERIFY(!page->overlay()->highlightImage().isNull());
        page->setHighlight(false);
        // The mode menu: one undo step, saved in the node; Luma hides the Hue and Sat rows.
        const int beforeMode = f.steps();
        mode->setCurrentIndex(2);
        Q_EMIT mode->activated(2);
        QCOMPARE(f.steps(), beforeMode + 1);
        QCOMPARE(f.session()->currentNode()->qualifier.mode, QualifierMode::Luma);
        QVERIFY(!qp->findChild<QCheckBox*>(QStringLiteral("QualifierUse_Hue"))->isVisibleTo(qp));
        QVERIFY(qp->findChild<QCheckBox*>(QStringLiteral("QualifierUse_Lum"))->isVisibleTo(qp));
        mode->setCurrentIndex(1); // RGB
        Q_EMIT mode->activated(1);
        QCOMPARE(f.session()->currentNode()->qualifier.mode, QualifierMode::Rgb);
        ValueField* redLow = qp->field(QStringLiteral("redLow"));
        QVERIFY(redLow);
        Q_EMIT redLow->valueCommitted(0.5);
        QCOMPARE(f.session()->currentNode()->qualifier.redLow, 0.5);
        // 3D: the eyedropper adds picked colours (Pick -: colours taken out), Clear forgets them.
        mode->setCurrentIndex(3);
        Q_EMIT mode->activated(3);
        QCOMPARE(f.session()->currentNode()->qualifier.mode, QualifierMode::ThreeD);
        qp->setPickMode(qualifierpick::Mode::Pick);
        ColorViewerOverlay* ov = page->overlay();
        const QRectF pic = ov->pictureRect();
        QTest::mouseClick(ov, Qt::LeftButton, {},
            QPoint(int(pic.left() + pic.width() * 0.25), int(pic.center().y())));
        QCoreApplication::processEvents();
        QCOMPARE(f.session()->currentNode()->qualifier.colors.size(), 1);
        const float blue[3] = {0.16f, 0.24f, 0.86f};
        qp->setPickMode(qualifierpick::Mode::Add);
        QTest::mouseClick(ov, Qt::LeftButton, {},
            QPoint(int(pic.left() + pic.width() * 0.75), int(pic.center().y())));
        QCoreApplication::processEvents();
        const GradeQualifier q3 = f.session()->currentNode()->qualifier;
        QCOMPARE(q3.colors.size(), 2);
        QVERIFY(grademath::qualifierKey(q3, red) > 0.9f);
        QVERIFY(grademath::qualifierKey(q3, blue) > 0.9f);
        qp->setPickMode(std::nullopt);
        auto* clear = qp->findChild<QToolButton*>(QStringLiteral("QualifierClearColors"));
        QVERIFY(clear && clear->isEnabled());
        const int beforeClear = f.steps();
        clear->click();
        QCOMPARE(f.steps(), beforeClear + 1);
        QVERIFY(f.session()->currentNode()->qualifier.colors.isEmpty());
        // The palette bar's Reset clears the mode, the colours and the finesse; undo brings them back.
        auto* reset = page->findChild<QToolButton*>(QStringLiteral("ColorPaletteReset"));
        QVERIFY(reset);
        const int beforeReset = f.steps();
        reset->click();
        QCOMPARE(f.steps(), beforeReset + 1);
        QCOMPARE(f.session()->currentNode()->qualifier, GradeQualifier {});
        QCOMPARE(qp->field(QStringLiteral("denoise"))->value(), 0.0);
        QCOMPARE(mode->currentIndex(), 0);
        f.project.undoStack()->undo();
        QCOMPARE(f.session()->currentNode()->qualifier.mode, QualifierMode::ThreeD);
        QVERIFY(f.session()->currentNode()->qualifier.hasFinesse());
    }

    void blurPalette()
    {
        Fixture f;
        f.showPage();
        ColorPage* page = f.page();
        auto* tab = page->findChild<QAbstractButton*>(QStringLiteral("ColorPaletteTab_5"));
        QVERIFY(tab);
        QCOMPARE(tab->text(), QStringLiteral("Blur"));
        tab->click();
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Blur);
        BlurPanel* blur = page->blur();
        QVERIFY(blur && blur->isVisible());
        const int steps = f.steps();
        // Radius R with the link on moves R, G and B together, as one undo step.
        ValueField* radius = blur->field(gk::BlurRadiusR);
        QVERIFY(radius && blur->linkButton(0)->isChecked());
        Q_EMIT radius->valueCommitted(0.9);
        QCOMPARE(f.steps(), steps + 1);
        const GradeNode* n = f.session()->currentNode();
        QVERIFY(n);
        for (const QString& key : {gk::BlurRadiusR, gk::BlurRadiusG, gk::BlurRadiusB})
            QCOMPARE(n->value(key, 0), 0.9);
        QCOMPARE(blur->field(gk::BlurRadiusB)->value(), 0.9);
        // Unlinked, one channel moves alone.
        blur->linkButton(0)->setChecked(false);
        Q_EMIT blur->field(gk::BlurRadiusG)->valueCommitted(0.3);
        QCOMPARE(f.session()->currentNode()->value(gk::BlurRadiusG, 0), 0.3);
        QCOMPARE(f.session()->currentNode()->value(gk::BlurRadiusR, 0), 0.9);
        // Sharpen and Mist pages edit their own keys.
        blur->modeButton(1)->click();
        QCOMPARE(blur->mode(), 1);
        Q_EMIT blur->field(gk::SharpCoring)->valueCommitted(0.4);
        blur->modeButton(2)->click();
        Q_EMIT blur->field(gk::MistRadius)->valueCommitted(0.6);
        Q_EMIT blur->field(gk::MistMix)->valueCommitted(0.8);
        QCOMPARE(f.session()->currentNode()->value(gk::SharpCoring, 0), 0.4);
        QCOMPARE(f.session()->currentNode()->value(gk::MistRadius, 0), 0.6);
        QCOMPARE(f.session()->currentNode()->value(gk::MistMix, 0), 0.8);
        // The palette bar's Reset puts every blur value back; one undo step brings them back.
        auto* reset = page->findChild<QToolButton*>(QStringLiteral("ColorPaletteReset"));
        QVERIFY(reset);
        const int before = f.steps();
        reset->click();
        QCOMPARE(f.steps(), before + 1);
        for (const QString& key : gk::blurKeys())
            QCOMPARE(f.session()->currentNode()->value(key, 0), gk::defaultValue(key));
        f.project.undoStack()->undo();
        QCOMPARE(f.session()->currentNode()->value(gk::MistRadius, 0), 0.6);
        // The layout remembers the palette.
        QSettings settings;
        settings.beginGroup(QStringLiteral("colorBlurLayoutTest"));
        page->saveLayout(settings);
        page->showPalette(ColorPage::Palette::Primaries);
        page->restoreLayout(settings);
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Blur);
    }

    void rgbMixerPalette()
    {
        Fixture f;
        f.showPage();
        ColorPage* page = f.page();
        auto* tab = page->findChild<QAbstractButton*>(QStringLiteral("ColorPaletteTab_7"));
        QVERIFY(tab);
        QCOMPARE(tab->text(), QStringLiteral("RGB Mixer"));
        tab->click();
        QCOMPARE(page->currentPalette(), ColorPage::Palette::RgbMixer);
        RgbMixerPanel* mixer = page->rgbMixer();
        QVERIFY(mixer && mixer->isVisible());
        // Nine fields in three tabs, identity at the start.
        const QString keys[9] = {gk::MixerRR, gk::MixerRG, gk::MixerRB, gk::MixerGR, gk::MixerGG,
            gk::MixerGB, gk::MixerBR, gk::MixerBG, gk::MixerBB};
        for (int i = 0; i < 9; ++i) {
            QVERIFY(mixer->field(keys[i]));
            QCOMPARE(mixer->field(keys[i])->value(), i % 4 == 0 ? 1.0 : 0.0);
        }
        QCOMPARE(mixer->page(), 0);
        // Typing a value is one undo step (the first edit makes the node).
        const int steps = f.steps();
        Q_EMIT mixer->field(gk::MixerBR)->valueCommitted(0.75);
        QCOMPARE(f.steps(), steps + 1);
        QCOMPARE(f.session()->currentNode()->value(gk::MixerBR, 0), 0.75);
        mixer->tabButton(2)->click();
        QCOMPARE(mixer->page(), 2);
        // Monochrome swaps the tabs for the Gray row; Preserve Luminance is a check box.
        mixer->monochrome()->setChecked(true);
        QCOMPARE(f.session()->currentNode()->value(gk::MixerMono, 0), 1.0);
        QCOMPARE(mixer->page(), 3);
        QVERIFY(mixer->field(gk::MixerMonoR));
        QCOMPARE(mixer->field(gk::MixerMonoG)->value(), 0.59);
        mixer->preserveLuminance()->setChecked(true);
        QCOMPARE(f.session()->currentNode()->value(gk::MixerPreserve, 0), 1.0);
        // The palette bar's Reset restores the identity as one undo step.
        auto* reset = page->findChild<QToolButton*>(QStringLiteral("ColorPaletteReset"));
        QVERIFY(reset);
        const int before = f.steps();
        reset->click();
        QCOMPARE(f.steps(), before + 1);
        for (const QString& key : gk::rgbMixerKeys())
            QCOMPARE(f.session()->currentNode()->value(key, 0), gk::defaultValue(key));
        QVERIFY(!mixer->monochrome()->isChecked());
        QCOMPARE(mixer->field(gk::MixerBR)->value(), 0.0);
        f.project.undoStack()->undo();
        QCOMPARE(f.session()->currentNode()->value(gk::MixerBR, 0), 0.75);
        QSettings settings;
        settings.beginGroup(QStringLiteral("colorMixerLayoutTest"));
        page->saveLayout(settings);
        page->showPalette(ColorPage::Palette::Primaries);
        page->restoreLayout(settings);
        QCOMPARE(page->currentPalette(), ColorPage::Palette::RgbMixer);
    }

    void agxPalette()
    {
        Fixture f;
        f.showPage();
        ColorPage* page = f.page();
        auto* tab = page->findChild<QAbstractButton*>(QStringLiteral("ColorPaletteTab_8"));
        QVERIFY(tab);
        QCOMPARE(tab->text(), QStringLiteral("AgX"));
        tab->click();
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Agx);
        AgxPanel* agx = page->agx();
        QVERIFY(agx && agx->isVisible());
        QVERIFY(!agx->enableBox()->isChecked());
        QCOMPARE(agx->inputBox()->currentIndex(), 0);
        for (QWidget* w : {static_cast<QWidget*>(agx->enableBox()), static_cast<QWidget*>(agx->inputBox()),
                 static_cast<QWidget*>(agx->lookBox()), static_cast<QWidget*>(agx->outputBox()),
                 static_cast<QWidget*>(agx->exposureField())})
            QVERIFY2(!w->toolTip().isEmpty(), qPrintable(w->objectName()));
        // Ticking the box is one undo step (the first edit makes the node).
        const int steps = f.steps();
        agx->enableBox()->setChecked(true);
        QCOMPARE(f.steps(), steps + 1);
        QCOMPARE(f.session()->currentNode()->value(gk::AgxOn, 0), 1.0);
        Q_EMIT agx->lookBox()->activated(2);
        QCOMPARE(f.session()->currentNode()->value(gk::AgxLook, 0), 2.0);
        Q_EMIT agx->inputBox()->activated(4);
        QCOMPARE(f.session()->currentNode()->value(gk::AgxInput, 0), 4.0);
        Q_EMIT agx->exposureField()->valueCommitted(-1.5);
        QCOMPARE(f.session()->currentNode()->value(gk::AgxExposure, 0), -1.5);
        auto* reset = page->findChild<QToolButton*>(QStringLiteral("ColorPaletteReset"));
        QVERIFY(reset);
        reset->click();
        for (const QString& key : gk::agxKeys())
            QCOMPARE(f.session()->currentNode()->value(key, 0), gk::defaultValue(key));
        QVERIFY(!agx->enableBox()->isChecked());
    }

    void hdrPalette()
    {
        Fixture f;
        f.showPage();
        ColorPage* page = f.page();
        auto* tab = page->findChild<QAbstractButton*>(QStringLiteral("ColorPaletteTab_11"));
        QVERIFY(tab);
        QCOMPARE(tab->text(), QStringLiteral("HDR"));
        tab->click();
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Hdr);
        HdrPanel* hdr = page->hdr();
        QVERIFY(hdr && hdr->isVisible());
        // Black Offset, Global, Shadow, Light, Highlight and Specular wheels (in that order on
        // screen) with Y R G B fields, every field with a tool tip.
        const gk::Wheel wheels[] = {gk::Wheel::HdrBlack, gk::Wheel::HdrGlobal, gk::Wheel::HdrShadow,
            gk::Wheel::HdrLight, gk::Wheel::HdrHighlight, gk::Wheel::HdrSpecular};
        int lastX = -1;
        for (const gk::Wheel w : wheels) {
            ColorWheel* wheel = hdr->wheel(w);
            QVERIFY(wheel && wheel->isVisible());
            QVERIFY(wheel->mapTo(hdr, QPoint(0, 0)).x() > lastX);
            lastX = wheel->mapTo(hdr, QPoint(0, 0)).x();
            for (int ch = 0; ch < 4; ++ch) {
                ValueField* field = hdr->field(gk::wheelKey(w, ch));
                QVERIFY2(field, qPrintable(gk::wheelKey(w, ch)));
                QVERIFY2(!field->toolTip().isEmpty(), qPrintable(gk::wheelKey(w, ch)));
            }
        }
        for (const QString& key : gk::hdrKeys()) {
            ValueField* field = hdr->field(key);
            QVERIFY2(field, qPrintable(key));
            QCOMPARE(field->value(), gk::defaultValue(key));
        }
        // Typing a value is one undo step (the first edit makes the node) and is saved in the grade.
        const int base = f.steps();
        Q_EMIT hdr->field(gk::HdrGlobalY)->valueCommitted(1.0);
        QCOMPARE(f.steps(), base + 1);
        QCOMPARE(f.session()->currentNode()->value(gk::HdrGlobalY, 0), 1.0);
        QCOMPARE(f.grade(f.clips[0]).nodes[0].value(gk::HdrGlobalY, 0), 1.0);
        Q_EMIT hdr->field(gk::HdrLightRange)->valueCommitted(0.5);
        QCOMPARE(f.session()->currentNode()->value(gk::HdrLightRange, 0), 0.5);
        // A drag on the Shadow wheel is one step and writes the HDR shadow keys only.
        ColorWheel* shadow = hdr->wheel(gk::Wheel::HdrShadow);
        const int beforeDrag = f.steps();
        const QPointF c = shadow->discRect().center();
        drag(shadow, c, c + QPointF(-15, 10));
        QCOMPARE(f.steps(), beforeDrag + 1);
        const GradeNode* n = f.session()->currentNode();
        QVERIFY(n->value(gk::HdrShadowR, 0) != 0.0 || n->value(gk::HdrShadowB, 0) != 0.0);
        QCOMPARE(n->value(gk::HdrGlobalR, 0), 0.0);
        QCOMPARE(n->value(gk::LogShadowR, 0), 0.0);
        // The master strip is the zone's exposure in stops.
        const int beforeStrip = f.steps();
        const QPointF s = shadow->stripRect().center();
        drag(shadow, s, s + QPointF(20, 0));
        QCOMPARE(f.steps(), beforeStrip + 1);
        QVERIFY(f.session()->currentNode()->value(gk::HdrShadowY, 0) > 0.0);
        // The grade is not an identity: it renders.
        QVERIFY(!kk::gradeops::isIdentity(f.grade(f.clips[0]), 0));
        // One undo step takes the strip drag back; a double-click on a wheel resets it.
        f.action("edit.undo")->trigger();
        QCOMPARE(f.session()->currentNode()->value(gk::HdrShadowY, 0), 0.0);
        QTest::mouseDClick(shadow, Qt::LeftButton, {}, c.toPoint());
        QCOMPARE(f.session()->currentNode()->value(gk::HdrShadowR, 0), 0.0);
        // The palette's Reset puts every HDR value back.
        auto* reset = page->findChild<QToolButton*>(QStringLiteral("ColorPaletteReset"));
        QVERIFY(reset);
        reset->click();
        for (const QString& key : gk::hdrKeys())
            QCOMPARE(f.session()->currentNode()->value(key, 0), gk::defaultValue(key));
        QVERIFY(kk::gradeops::isIdentity(f.grade(f.clips[0]), 0));
    }

    void warperPalette()
    {
        Fixture f;
        f.showPage();
        ColorPage* page = f.page();
        auto* tab = page->findChild<QAbstractButton*>(QStringLiteral("ColorPaletteTab_10"));
        QVERIFY(tab);
        QCOMPARE(tab->text(), QStringLiteral("Color Warper"));
        tab->click();
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Warper);
        // An all-identity grade may leave no node behind: read the warp through this.
        const auto warpNow = [&f]() {
            const GradeNode* node = f.session()->currentNode();
            return node ? node->warp : GradeWarp {};
        };
        WarperPanel* warper = page->warper();
        QVERIFY(warper && warper->isVisible());
        WarperGrid* grid = warper->gridWidget();
        QVERIFY(grid && grid->isVisible());
        QCOMPARE(grid->grid(), WarpGrid::HueSat);
        QCOMPARE(warper->resolutionBox()->currentIndex(), 1);
        for (QWidget* w : {static_cast<QWidget*>(warper->resolutionBox()),
                 static_cast<QWidget*>(warper->hueSatButton()),
                 static_cast<QWidget*>(warper->chromaLumaButton()),
                 static_cast<QWidget*>(warper->resetPinButton()),
                 static_cast<QWidget*>(warper->resetGridButton()), static_cast<QWidget*>(grid)})
            QVERIFY2(!w->toolTip().isEmpty(), qPrintable(w->objectName()));
        QVERIFY(!warper->resetPinButton()->isEnabled()); // no pin selected
        // The grey centre cannot be picked.
        QVERIFY(!grid->isMovable(0, 0));
        QVERIFY(grid->isMovable(3, 4));
        // A click selects a pin without a step; a drag moves it: one undo step for the whole drag.
        const int base = f.steps();
        const QPointF pin = grid->pinPosition(3, 4);
        QTest::mouseClick(grid, Qt::LeftButton, {}, pin.toPoint());
        QCOMPARE(grid->selectedPin(), QPoint(3, 4));
        QVERIFY(warper->resetPinButton()->isEnabled());
        QCOMPARE(f.steps(), base);
        const QPointF target = pin + QPointF(4, -3);
        drag(grid, pin, target);
        QCOMPARE(f.steps(), base + 1);
        const GradeNode* n = f.session()->currentNode();
        QVERIFY(n);
        const auto [dx, dy] = n->warp.pin(WarpGrid::HueSat, 3, 4);
        QVERIFY(dx != 0.0f || dy != 0.0f);
        QVERIFY(QLineF(grid->pinPosition(3, 4), target).length() < 2.5);
        QVERIFY(n->warp.isIdentity(WarpGrid::ChromaLuma));
        // Saved with the clip's grade, and the node now changes the picture.
        const ClipGrade stored = f.grade(f.clips[0]);
        QCOMPARE(stored.nodes[0].warp, n->warp);
        QVERIFY(!kk::gradeops::isIdentity(stored, 0));
        // Undo takes the whole drag back, redo brings it again.
        f.action("edit.undo")->trigger();
        const ClipGrade undone = f.grade(f.clips[0]);
        QVERIFY(undone.nodes.isEmpty() || undone.nodes[0].warp.isIdentity());
        f.action("edit.redo")->trigger();
        QCOMPARE(f.grade(f.clips[0]).nodes[0].warp, stored.nodes[0].warp);
        // Arrow keys nudge the selected pin (one step each), Delete puts it back.
        const int beforeKeys = f.steps();
        QTest::keyClick(grid, Qt::Key_Right);
        QCOMPARE(f.steps(), beforeKeys + 1);
        QVERIFY(warpNow().pin(WarpGrid::HueSat, 3, 4).first != dx);
        QTest::keyClick(grid, Qt::Key_Delete);
        QCOMPARE(f.steps(), beforeKeys + 2);
        QVERIFY(warpNow().isIdentity());
        // A double-click on a moved pin puts it back; Reset Pin does the same for the selection.
        drag(grid, grid->pinPosition(2, 3), grid->pinPosition(2, 3) + QPointF(3, 3));
        QVERIFY(!warpNow().isIdentity());
        QTest::mouseDClick(grid, Qt::LeftButton, {}, grid->pinPosition(2, 3).toPoint());
        QVERIFY(warpNow().isIdentity());
        drag(grid, grid->pinPosition(5, 2), grid->pinPosition(5, 2) + QPointF(3, 3));
        QVERIFY(!warpNow().isIdentity());
        warper->resetPinButton()->click();
        QVERIFY(warpNow().isIdentity());
        // Chroma - Luma: its own grid; the grey axis only moves up and down.
        warper->chromaLumaButton()->click();
        QCOMPARE(grid->grid(), WarpGrid::ChromaLuma);
        QVERIFY(grid->selectedPin().x() < 0);
        QVERIFY(grid->isMovable(0, 2));
        const QPointF p0 = grid->pinPosition(0, 2);
        drag(grid, p0, p0 + QPointF(8, -5));
        const auto [gx, gy] = warpNow().pin(WarpGrid::ChromaLuma, 0, 2);
        QCOMPARE(gx, 0.0f);
        QVERIFY(gy != 0.0f);
        const QPointF p1 = grid->pinPosition(3, 3);
        drag(grid, p1, p1 + QPointF(-6, 4));
        const auto [hx, hy] = warpNow().pin(WarpGrid::ChromaLuma, 3, 3);
        QVERIFY(hx != 0.0f && hy != 0.0f);
        QVERIFY(warpNow().isIdentity(WarpGrid::HueSat));
        // Resolution keeps the colours: the moved pin's vertex exists in the finer mesh as well.
        warper->hueSatButton()->click();
        drag(grid, grid->pinPosition(3, 4), grid->pinPosition(3, 4) + QPointF(4, -3));
        const auto before = warpNow().pin(WarpGrid::HueSat, 3, 4);
        const int beforeRes = f.steps();
        Q_EMIT warper->resolutionBox()->activated(2);
        QCOMPARE(f.steps(), beforeRes + 1);
        const GradeWarp fine = warpNow();
        QCOMPARE(int(fine.resolution), 2);
        QVERIFY(std::abs(fine.pin(WarpGrid::HueSat, 6, 7).first - before.first) < 1e-6f);
        QVERIFY(std::abs(fine.pin(WarpGrid::HueSat, 6, 7).second - before.second) < 1e-6f);
        QCOMPARE(warper->resolutionBox()->currentIndex(), 2);
        // Reset Grid empties the shown grid only; the palette's Reset empties everything.
        warper->resetGridButton()->click();
        QVERIFY(warpNow().isIdentity(WarpGrid::HueSat));
        QVERIFY(!warpNow().isIdentity(WarpGrid::ChromaLuma));
        auto* reset = page->findChild<QToolButton*>(QStringLiteral("ColorPaletteReset"));
        QVERIFY(reset);
        reset->click();
        QVERIFY(warpNow().isIdentity());
    }

    void trackerPalette()
    {
        Fixture f;
        f.showPage();
        ColorPage* page = f.page();
        auto* tab = page->findChild<QAbstractButton*>(QStringLiteral("ColorPaletteTab_6"));
        QVERIFY(tab);
        QCOMPARE(tab->text(), QStringLiteral("Tracker"));
        tab->click();
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Tracker);
        ColorTrackerPanel* tr = page->tracker();
        QVERIFY(tr && tr->isVisible());
        QCOMPARE(tr->mode(), ColorTrackerPanel::Mode::Window);
        // The window's outline stays on the viewer while the Tracker palette is open.
        QVERIFY(page->overlay()->windowControls());
        // Defaults: Pan and Tilt on, Zoom, Rotate and 3D off, Clip mode.
        QVERIFY(tr->panBox()->isChecked() && tr->tiltBox()->isChecked());
        QVERIFY(!tr->zoomBox()->isChecked() && !tr->rotateBox()->isChecked()
            && !tr->perspectiveBox()->isChecked());
        QVERIFY(tr->clipModeButton()->isChecked() && !tr->frameModeButton()->isChecked());
        // Nothing to track without a window.
        QVERIFY(!tr->forwardButton()->isEnabled() && !tr->reverseButton()->isEnabled());
        QVERIFY(!tr->clearButton()->isEnabled());
        QVERIFY(!tr->trackFromPlayhead(true));
        QVERIFY(!tr->status().isEmpty());

        f.session()->addWindow(GradeWindowShape::Circle);
        const quint64 clip = f.clips[0];
        const quint64 node = f.session()->selectedNode();
        f.session()->setWindowParams(0,
            {{wk::CenterX, 0.5}, {wk::CenterY, 0.5}, {wk::Width, 0.3}, {wk::Height, 0.5}}, QStringLiteral("t"));
        QVERIFY(tr->forwardButton()->isEnabled() && tr->reverseButton()->isEnabled());

        // The checkboxes are saved in the node, one undo step each.
        int steps = f.steps();
        tr->zoomBox()->click();
        QCOMPARE(f.steps(), steps + 1);
        QVERIFY(f.grade(clip).node(node)->track.zoom);
        tr->tiltBox()->click();
        QVERIFY(!f.grade(clip).node(node)->track.tilt);
        f.project.undoStack()->undo();
        QVERIFY(f.grade(clip).node(node)->track.tilt && tr->tiltBox()->isChecked());
        tr->zoomBox()->click();
        QVERIFY(!f.grade(clip).node(node)->track.zoom);

        // A picture that slides 1.5 px right and 0.5 px down every frame (320 x 180 timeline).
        std::vector<std::array<double, 3>> waves; // random plane waves, wavelengths 5 to 20 px
        std::mt19937 rng(99);
        std::uniform_real_distribution<double> wl(5.0, 20.0), dir(0.0, 2 * M_PI);
        for (int i = 0; i < 30; ++i) {
            const double k = 2 * M_PI / wl(rng), a = dir(rng);
            waves.push_back({k * std::cos(a), k * std::sin(a), dir(rng)});
        }
        tr->setFrameSourceOverride([waves](int64_t frame, QSize size) {
            QImage img(size, QImage::Format_ARGB32_Premultiplied);
            const double k = double(size.width()) / 320.0;
            for (int y = 0; y < size.height(); ++y) {
                auto* row = reinterpret_cast<QRgb*>(img.scanLine(y));
                for (int x = 0; x < size.width(); ++x) {
                    const double px = x / k - 1.5 * frame, py = y / k - 0.5 * frame;
                    double v = 0.5;
                    for (const auto& w : waves)
                        v += 0.07 * std::sin(w[0] * px + w[1] * py + w[2]);
                    const int g = std::clamp(int(v * 255.0), 0, 255);
                    row[x] = qRgb(g, g, g);
                }
            }
            auto fr = std::make_shared<RenderedFrame>();
            fr->frame = frame;
            fr->size = size;
            fr->ok = true;
            fr->image = img;
            std::promise<RenderedFramePtr> p;
            p.set_value(fr);
            return p.get_future();
        });
        f.project.timeline()->setPlayhead(0);
        steps = f.steps();
        tr->forwardButton()->click();
        QVERIFY(tr->isTracking());
        QVERIFY(tr->stopButton()->isEnabled() && !tr->forwardButton()->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!tr->isTracking(), 60000);
        QCOMPARE(f.steps(), steps + 1);
        const GradeTrack t = f.grade(clip).node(node)->track;
        QCOMPARE(t.samples.size(), 50); // the clip's 50 frames
        QVERIFY(tr->clearButton()->isEnabled());
        // The picture slid 1.5 px of 320 per frame: the window followed.
        const GradeNode* n = f.session()->currentNode();
        const auto g = grademath::trackedWindowGeometry(*n, n->windows[0], 30, 320.0 / 180.0);
        QVERIFY2(std::abs(g.cx - (0.5 + 45.0 / 320.0)) < 0.008, qPrintable(QString::number(g.cx)));
        QVERIFY2(std::abs(g.cy - (0.5 + 15.0 / 180.0)) < 0.012, qPrintable(QString::number(g.cy)));
        // The overlay draws the window where the track says (the Move handle of the shown
        // window is hit there, and a drag there edits the stored window by the same amount).
        f.project.timeline()->setPlayhead(30);
        QCoreApplication::processEvents();
        n = f.session()->currentNode(); // moving the playhead may replace the grade it pointed into
        ColorViewerOverlay* ov = page->overlay();
        const GradeWindow shownWindow = ov->shown(n->windows[0]);
        QVERIFY(std::abs(shownWindow.value(wk::CenterX, 30) - g.cx) < 1e-9);
        const QPointF centre = ov->handlePos(shownWindow, ColorViewerOverlay::Handle::Move);
        QCOMPARE(ov->hitTest(centre).handle, ColorViewerOverlay::Handle::Move);
        drag(ov, centre, centre + QPointF(20, 0));
        const GradeNode* after = f.session()->currentNode();
        const auto moved = grademath::trackedWindowGeometry(*after, after->windows[0], 30, 320.0 / 180.0);
        QVERIFY(moved.cx > g.cx + 0.001);
        QVERIFY(std::abs(after->windows[0].value(wk::CenterX, 0) - 0.5) > 0.001);
        f.project.undoStack()->undo(); // the drag
        // Undo takes the track away, redo brings it back.
        f.project.undoStack()->undo();
        QVERIFY(!f.grade(clip).node(node)->track.hasData());
        f.project.undoStack()->redo();
        QVERIFY(f.grade(clip).node(node)->track.hasData());
        // Clear Track removes the frames and keeps the checkboxes.
        steps = f.steps();
        tr->clearButton()->click();
        QCOMPARE(f.steps(), steps + 1);
        QVERIFY(!f.grade(clip).node(node)->track.hasData());
        QVERIFY(f.grade(clip).node(node)->track.pan);
        QVERIFY(!tr->clearButton()->isEnabled());
        // Frame mode: one run tracks one frame; Track Reverse works from the playhead.
        f.project.timeline()->setPlayhead(10);
        tr->frameModeButton()->click();
        QVERIFY(tr->frameModeButton()->isChecked());
        tr->forwardButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(!tr->isTracking(), 60000);
        QCOMPARE(f.grade(clip).node(node)->track.samples.size(), 2);
        QCOMPARE(f.grade(clip).node(node)->track.firstFrame, int64_t(10));
        tr->clearButton()->click();
        tr->clipModeButton()->click();
        f.project.timeline()->setPlayhead(25);
        tr->reverseButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(!tr->isTracking(), 60000);
        QCOMPARE(f.grade(clip).node(node)->track.firstFrame, int64_t(0));
        QCOMPARE(f.grade(clip).node(node)->track.samples.size(), 26);
        // The graph exists; the Stabilizer mode shows its own controls (a solid clip cannot
        // be stabilized: Stabilize is off).
        QVERIFY(tr->graph());
        tr->modeButton(ColorTrackerPanel::Mode::Stabilizer)->click();
        QCOMPARE(tr->mode(), ColorTrackerPanel::Mode::Stabilizer);
        QVERIFY(tr->stabilizerMode()->count() == 3);
        QVERIFY(!tr->stabilizeButton()->isEnabled());
        // The palette bar's Reset clears the tracker data of the node.
        auto* reset = page->findChild<QToolButton*>(QStringLiteral("ColorPaletteReset"));
        QVERIFY(reset);
        reset->click();
        QVERIFY(f.grade(clip).node(node)->track == GradeTrack {});
        // The layout remembers the palette.
        QSettings settings;
        settings.beginGroup(QStringLiteral("colorTrackerLayoutTest"));
        page->saveLayout(settings);
        page->showPalette(ColorPage::Palette::Primaries);
        QVERIFY(!page->overlay()->windowControls());
        page->restoreLayout(settings);
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Tracker);
    }

    void lutBrowserAndKeyPanel()
    {
        Fixture f;
        f.showPage();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile cube(dir.filePath(QStringLiteral("warm.cube")));
        QVERIFY(cube.open(QIODevice::WriteOnly));
        cube.write("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n");
        cube.close();
        f.page()->showPalette(ColorPage::Palette::Luts);
        LutBrowser* luts = f.page()->luts();
        luts->addFolder(dir.path());
        QTreeWidgetItem* found = nullptr;
        for (int r = 0; r < luts->tree()->topLevelItemCount(); ++r) {
            QTreeWidgetItem* root = luts->tree()->topLevelItem(r);
            for (int c = 0; c < root->childCount(); ++c) {
                if (root->child(c)->text(0) == QStringLiteral("warm.cube"))
                    found = root->child(c);
            }
        }
        QVERIFY(found);
        Q_EMIT luts->tree()->itemDoubleClicked(found, 0);
        ClipGrade g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes.size(), 1);
        QCOMPARE(g.nodes[0].lutPath, cube.fileName());
        auto* remove = luts->findChild<QToolButton*>(QStringLiteral("LutRemove"));
        QVERIFY(remove->isEnabled());
        remove->click();
        QVERIFY(f.grade(f.clips[0]).nodes[0].lutPath.isEmpty());
        // The Key tab: the key output gain is the node's Mix.
        KeyPanel* key = f.page()->keyPanel();
        Q_EMIT key->gainField()->valueCommitted(0.4);
        QCOMPARE(f.grade(f.clips[0]).nodes[0].value(gk::Mix, 0), 0.4);
    }

    void primariesExtrasAndAutoColor()
    {
        Fixture f;
        f.showPage();
        PrimariesPanel* prim = f.page()->primaries();
        // MD, Boost, Shad and Hi sit in the palette, neutral, with tooltips.
        for (const QString& key : {gk::MidtoneDetail, gk::ColorBoost, gk::Shadows, gk::Highlights}) {
            ValueField* field = prim->field(key);
            QVERIFY2(field, qPrintable(key));
            QVERIFY(!field->toolTip().isEmpty());
            QCOMPARE(field->value(), 0.0);
            QVERIFY(field->isVisible());
        }
        const int base = f.steps();
        Q_EMIT prim->field(gk::ColorBoost)->valueCommitted(40);
        QCOMPARE(f.steps(), base + 1); // the first edit makes the node
        QCOMPARE(f.session()->currentNode()->value(gk::ColorBoost, 0), 40.0);
        // A scrub of Shad is one step; MD and Hi commit like the other fields.
        Q_EMIT prim->field(gk::Shadows)->scrubStarted();
        Q_EMIT prim->field(gk::Shadows)->valueScrubbed(10);
        Q_EMIT prim->field(gk::Shadows)->valueScrubbed(25);
        Q_EMIT prim->field(gk::Shadows)->scrubFinished();
        QCOMPARE(f.steps(), base + 2);
        QCOMPARE(f.session()->currentNode()->value(gk::Shadows, 0), 25.0);
        Q_EMIT prim->field(gk::Highlights)->valueCommitted(-30);
        Q_EMIT prim->field(gk::MidtoneDetail)->valueCommitted(60);
        QCOMPARE(f.session()->currentNode()->value(gk::Highlights, 0), -30.0);
        QCOMPARE(f.session()->currentNode()->value(gk::MidtoneDetail, 0), 60.0);
        // Undo takes the last one back.
        f.project.undoStack()->undo();
        QCOMPARE(f.session()->currentNode()->value(gk::MidtoneDetail, 0), 0.0);
        // The Primaries reset puts them back with everything else.
        auto* reset = f.page()->findChild<QToolButton*>(QStringLiteral("ColorPaletteReset"));
        QVERIFY(reset);
        reset->click();
        for (const QString& key : {gk::ColorBoost, gk::Shadows, gk::Highlights})
            QCOMPARE(f.session()->currentNode()->value(key, 0), 0.0);
        QVERIFY(go::isNodeIdentity(*f.session()->currentNode(), 0));

        // Auto Color: from the node's input picture (red | blue halves, limited range) it sets
        // the per-channel lift, gamma and gain in one undo step.
        f.splitInput();
        QVERIFY(prim->autoButton());
        QVERIFY(!prim->autoButton()->toolTip().isEmpty());
        const int before = f.steps();
        prim->autoButton()->click();
        QCOMPARE(f.steps(), before + 1);
        const GradeNode* n = f.session()->currentNode();
        QVERIFY(n);
        bool changed = false;
        for (int ch = 0; ch < 3; ++ch) {
            changed = changed || n->value(gk::wheelKey(gk::Wheel::Lift, ch), 0) != 0.0
                || n->value(gk::wheelKey(gk::Wheel::Gain, ch), 0) != 1.0;
        }
        QVERIFY(changed);
        QCOMPARE(n->value(gk::LiftY, 0), 0.0); // the master wheels stay
        QCOMPARE(n->value(gk::GainY, 0), 1.0);
        // The red channel spans 40..220: its black point is lifted off (lift below 0) and its
        // gain is above 1 so the white point reaches 1.
        QVERIFY(n->value(gk::LiftR, 0) < 0.0);
        QVERIFY(n->value(gk::GainR, 0) > 1.0);
        f.project.undoStack()->undo();
        QCOMPARE(f.session()->currentNode()->value(gk::GainR, 0), 1.0);
    }

    void logPalette()
    {
        Fixture f;
        f.showPage();
        ColorPage* page = f.page();
        auto* tab = page->findChild<QAbstractButton*>(QStringLiteral("ColorPaletteTab_9"));
        QVERIFY(tab);
        QCOMPARE(tab->text(), QStringLiteral("Log"));
        tab->click();
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Log);
        PrimariesPanel* log = page->logPalette();
        QVERIFY(log && log->isVisible());
        QCOMPARE(log->kind(), PrimariesPanel::Kind::Log);
        QVERIFY(!log->autoButton());
        // Shadow, Midtone, Highlight and Offset wheels, each with Y R G B fields.
        for (const gk::Wheel w : {gk::Wheel::LogShadow, gk::Wheel::LogMidtone,
                 gk::Wheel::LogHighlight, gk::Wheel::LogOffset}) {
            QVERIFY(log->wheel(w));
            for (int ch = 0; ch < 4; ++ch)
                QVERIFY2(log->field(gk::wheelKey(w, ch)), qPrintable(gk::wheelKey(w, ch)));
        }
        // Low and High Range with Resolve's defaults.
        QCOMPARE(log->field(gk::LogLowRange)->value(), 0.33);
        QCOMPARE(log->field(gk::LogHighRange)->value(), 0.55);
        // A drag on the Shadow wheel is one step and writes the log shadow keys only.
        ColorWheel* shadow = log->wheel(gk::Wheel::LogShadow);
        const int base = f.steps();
        const QPointF c = shadow->discRect().center();
        drag(shadow, c, c + QPointF(-15, 10));
        QCOMPARE(f.steps(), base + 1);
        const GradeNode* n = f.session()->currentNode();
        QVERIFY(n);
        QVERIFY(n->value(gk::LogShadowB, 0) != 0.0 || n->value(gk::LogShadowR, 0) != 0.0);
        QCOMPARE(n->value(gk::LiftR, 0), 0.0);
        QCOMPARE(n->value(gk::LogMidR, 0), 0.0);
        // The master strip of the Highlight wheel and the range fields.
        ColorWheel* high = log->wheel(gk::Wheel::LogHighlight);
        const QPointF s = high->stripRect().center();
        drag(high, s, s + QPointF(20, 0));
        QVERIFY(f.session()->currentNode()->value(gk::LogHighY, 0) > 0.0);
        Q_EMIT log->field(gk::LogLowRange)->valueCommitted(0.4);
        QCOMPARE(f.session()->currentNode()->value(gk::LogLowRange, 0), 0.4);
        // The grade through the viewer path: the node is not identity and renders differently
        // in the CPU reference.
        QVERIFY(!go::isNodeIdentity(*f.session()->currentNode(), 0));
        // The Log palette's wheels show the node's values after another selection.
        QVERIFY(log->field(gk::LogHighY)->value() > 0.0);
        // Reset (the bar's button) clears the Log values.
        auto* reset = page->findChild<QToolButton*>(QStringLiteral("ColorPaletteReset"));
        reset->click();
        n = f.session()->currentNode();
        QCOMPARE(n->value(gk::LogShadowB, 0), 0.0);
        QCOMPARE(n->value(gk::LogHighY, 0), 0.0);
        QCOMPARE(n->value(gk::LogLowRange, 0), 0.33);
        // Primaries and Log share Temp / Tint / Contrast / Pivot / Sat / Hue: a value typed in
        // one palette shows in the other.
        Q_EMIT log->field(gk::Saturation)->valueCommitted(1.4);
        QCOMPARE(page->primaries()->field(gk::Saturation)->value(), 1.4);
    }

    void curvesSoftClipSatLumAndPick()
    {
        Fixture f;
        f.showPage();
        f.splitInput();
        ColorPage* page = f.page();
        page->showPalette(ColorPage::Palette::Curves);
        ColorCurvesPanel* panel = page->curves();
        // Sat vs Lum is the seventh mode.
        auto* satLum = page->findChild<QAbstractButton*>(
            QStringLiteral("CurveMode_") + QString::number(int(GradeCurve::SatLum)));
        QVERIFY(satLum);
        QCOMPARE(satLum->text(), QStringLiteral("Sat vs Lum"));
        satLum->click();
        QCOMPARE(panel->editor()->curve(), GradeCurve::SatLum);
        panel->editor()->resize(300, 240);
        const QRectF plot = panel->editor()->plotRect();
        QTest::mouseClick(panel->editor(), Qt::LeftButton, {},
            QPoint(int(plot.left() + plot.width() * 0.5), int(plot.bottom() - plot.height() * 0.8)));
        QCOMPARE(f.session()->currentNode()->curves[GradeCurve::SatLum].size(), 3);
        panel->resetCurve();
        QVERIFY(f.session()->currentNode()->curves[GradeCurve::SatLum].isEmpty());

        // Soft Clip: four fields in the Custom mode, one undo step each, saved in the node.
        panel->showCurve(GradeCurve::Luma);
        ValueField* low = panel->softClipField(gk::SoftClipLow);
        ValueField* high = panel->softClipField(gk::SoftClipHigh);
        QVERIFY(low && high && panel->softClipField(gk::SoftClipLowSoft)
            && panel->softClipField(gk::SoftClipHighSoft));
        QVERIFY(low->isVisible());
        QCOMPARE(low->value(), 0.0);
        QCOMPARE(high->value(), 1.0);
        const int base = f.steps();
        Q_EMIT low->valueCommitted(0.1);
        Q_EMIT high->valueCommitted(0.9);
        QCOMPARE(f.steps(), base + 2);
        QCOMPARE(f.session()->currentNode()->value(gk::SoftClipLow, 0), 0.1);
        QCOMPARE(f.session()->currentNode()->value(gk::SoftClipHigh, 0), 0.9);
        // The soft clip does change the rendered picture (CPU reference of the grade).
        const ClipGrade graded = f.grade(f.clips[0]);
        QImage img(8, 8, QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(250, 5, 128));
        render::applyGrade(graded, 0, img);
        QVERIFY(std::abs(qRed(img.pixel(0, 0)) - 230) <= 1);
        QVERIFY(std::abs(qGreen(img.pixel(0, 0)) - 26) <= 1);
        // Other curve modes hide the Soft Clip row.
        panel->showCurve(GradeCurve::HueSat);
        QVERIFY(!low->isVisible());
        panel->showCurve(GradeCurve::Luma);
        // The palette's reset in Custom mode also resets the Soft Clip.
        panel->resetCurve();
        QCOMPARE(f.session()->currentNode()->value(gk::SoftClipLow, 0), 0.0);
        QCOMPARE(f.session()->currentNode()->value(gk::SoftClipHigh, 0), 1.0);

        // Pick: the eyedropper over the viewer adds the picked colour's points.
        QVERIFY(panel->pickButton());
        QVERIFY(!page->overlay()->pickMode());
        panel->pickButton()->setChecked(true);
        QVERIFY(panel->pickActive());
        QVERIFY(page->overlay()->pickMode());
        const QRectF pic = page->overlay()->pictureRect();
        const int beforePick = f.steps();
        QTest::mouseClick(page->overlay(), Qt::LeftButton, {},
            QPoint(int(pic.left() + pic.width() * 0.25), int(pic.center().y())));
        QCOMPARE(f.steps(), beforePick + 1);
        const CurvePoints y = f.session()->currentNode()->curves[GradeCurve::Luma];
        QCOMPARE(y.size(), 1);
        const float red[3] = {220.0f / 255, 40.0f / 255, 40.0f / 255};
        QVERIFY(std::abs(y[0].x - grademath::luma(red)) < 0.02);
        QVERIFY(std::abs(y[0].y - y[0].x) < 1e-9); // on the diagonal: nothing changes yet
        // The qualifier did not change, the palette stayed on Curves.
        QVERIFY(!f.session()->currentNode()->qualifier.enabled);
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Curves);
        // Hue vs Sat: three neutral points around the colour's hue; blue half next.
        panel->showCurve(GradeCurve::HueSat);
        QTest::mouseClick(page->overlay(), Qt::LeftButton, {},
            QPoint(int(pic.left() + pic.width() * 0.75), int(pic.center().y())));
        const CurvePoints hs = f.session()->currentNode()->curves[GradeCurve::HueSat];
        QCOMPARE(hs.size(), 3);
        const float blue[3] = {40.0f / 255, 60.0f / 255, 220.0f / 255};
        float hsl[3];
        grademath::rgbToHsl(blue, hsl);
        QVERIFY(std::abs(hs[1].x - hsl[0]) < 0.02 || std::abs(hs[0].x - hsl[0]) < 0.02
            || std::abs(hs[2].x - hsl[0]) < 0.02);
        for (const CurvePoint& p : hs)
            QCOMPARE(p.y, 0.5);
        // Pick off: a click on the viewer is the qualifier's again (no pick mode at all).
        panel->pickButton()->setChecked(false);
        QVERIFY(!page->overlay()->pickMode());
    }

    void generateLutFromAGrade()
    {
        Fixture f;
        f.showPage();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("grade.cube"));
        QString error;
        // An ungraded clip has nothing to bake.
        QVERIFY(!f.session()->generateLut(f.clips[0], 17, path, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFile::exists(path));
        f.session()->setParam(0, gk::Saturation, 0.0, QStringLiteral("test"));
        QVERIFY2(f.session()->generateLut(f.clips[0], 17, path, &error), qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray text = file.readAll();
        QVERIFY(text.contains("LUT_3D_SIZE 17"));
        QVERIFY(text.contains("TITLE \"Clip 1\""));
        // Saturation 0: every row is grey (r = g = b).
        int rows = 0;
        for (const QByteArray& line : text.split('\n')) {
            const QList<QByteArray> parts = line.split(' ');
            if (parts.size() != 3 || !(line[0] >= '0' && line[0] <= '9'))
                continue;
            ++rows;
            QVERIFY(std::abs(parts[0].toDouble() - parts[1].toDouble()) < 2e-3);
            QVERIFY(std::abs(parts[1].toDouble() - parts[2].toDouble()) < 2e-3);
        }
        QCOMPARE(rows, 17 * 17 * 17);
        QVERIFY(!f.session()->generateLut(999999, 17, path, &error)); // not a clip
        QVERIFY(!f.session()->generateLut(f.clips[0], 17, dir.filePath(QStringLiteral("no/such/dir/x.cube")), &error));
    }

    /// Color Match palette (spec 153): the chart overlay in the viewer, Match on a synthetic
    /// chart picture, and Shot Match to this clip from the clips strip.
    void colorMatchPalette()
    {
        namespace cm = kk::colormatch;
        Fixture f;
        f.showPage();
        ColorPage* page = f.page();
        auto* tab = page->findChild<QAbstractButton*>(QStringLiteral("ColorPaletteTab_14"));
        QVERIFY(tab);
        QCOMPARE(tab->text(), QStringLiteral("Color Match"));
        tab->click();
        QCOMPARE(page->currentPalette(), ColorPage::Palette::ColorMatch);
        ColorMatchPanel* panel = page->colorMatch();
        QVERIFY(panel && panel->isVisible());
        for (QWidget* w : {static_cast<QWidget*>(panel->chartCombo()),
                 static_cast<QWidget*>(panel->sourceGammaCombo()),
                 static_cast<QWidget*>(panel->targetGammaCombo()),
                 static_cast<QWidget*>(panel->colorSpaceCombo()),
                 static_cast<QWidget*>(panel->whitePointCombo()),
                 static_cast<QWidget*>(panel->matchButton()),
                 static_cast<QWidget*>(panel->showChartButton())})
            QVERIFY(w->isVisible());
        QCOMPARE(panel->chartCombo()->count(), 2);
        ColorViewerOverlay* overlay = page->viewerOverlay();
        QVERIFY(!overlay->chartVisible());
        panel->showChartButton()->click();
        QVERIFY(overlay->chartVisible());
        // Dragging a corner handle moves that corner of the panel's outline.
        const cm::ChartQuad before = panel->quad();
        const QPointF from = overlay->chartCornerPos(0);
        drag(overlay, from, from + QPointF(14, 9));
        QVERIFY(panel->quad().corner[0] != before.corner[0]);
        QCOMPARE(panel->quad().corner[2], before.corner[2]);
        // Dragging inside moves the whole outline.
        const cm::ChartQuad moved = panel->quad();
        const QPointF mid = (overlay->chartCornerPos(0) + overlay->chartCornerPos(2)) / 2.0;
        drag(overlay, mid, mid + QPointF(10, 6));
        QVERIFY(panel->quad().corner[2] != moved.corner[2]);
        // Another palette hides the outline again.
        page->showPalette(ColorPage::Palette::Primaries);
        QVERIFY(!overlay->chartVisible());
        page->showPalette(ColorPage::Palette::ColorMatch);
        QVERIFY(overlay->chartVisible());

        // A picture of the chart as a camera would show it: darker, with a green cast.
        panel->resetAll();
        panel->setChartShown(true);
        const cm::ChartQuad quad = panel->quad();
        const auto want = cm::chartTargetColors(panel->chartType(), panel->target());
        QImage img(320, 180, QImage::Format_ARGB32);
        img.fill(qRgb(30, 30, 30));
        for (int p = 0; p < cm::kChartPatches; ++p) {
            for (int j = 0; j < 40; ++j) {
                for (int i = 0; i < 40; ++i) {
                    const QPointF pt = cm::chartPoint(quad,
                        ((p % cm::kChartColumns) + (i + 0.5) / 40) / cm::kChartColumns,
                        ((p / cm::kChartColumns) + (j + 0.5) / 40) / cm::kChartRows);
                    const auto& c = want[size_t(p)];
                    img.setPixel(int(pt.x() * 320), int(pt.y() * 180),
                        qRgb(int(c[0] * 0.8f * 255), int(c[1] * 1.0f * 255), int(c[2] * 0.7f * 255)));
                }
            }
        }
        page->frames()->setInput(img);
        const int steps = f.steps();
        QVERIFY(panel->match());
        QCOMPARE(f.steps(), steps + 1); // one undo step
        QVERIFY2(panel->statusText().contains(QStringLiteral("Chart matched")),
            qPrintable(panel->statusText()));
        const ClipGrade g = f.grade(f.clips[0]);
        QCOMPARE(g.nodes.size(), 1);
        QVERIFY(g.nodes[0].value(gk::GainR, 0) > 1.0); // the red the camera lost comes back
        QVERIFY(g.nodes[0].value(gk::GainB, 0) > 1.0);
        // The graded picture is closer to the reference than the raw one.
        QImage graded = img;
        render::applyGrade(g, 0, graded);
        const QPointF white = cm::chartPoint(quad, 0.5 / cm::kChartColumns, 3.5 / cm::kChartRows);
        const QRgb raw = img.pixel(int(white.x() * 320), int(white.y() * 180));
        const QRgb now = graded.pixel(int(white.x() * 320), int(white.y() * 180));
        const int target = int(want[cm::kChartFirstNeutral][0] * 255);
        QVERIFY(std::abs(qRed(now) - target) < std::abs(qRed(raw) - target));
        QVERIFY(std::abs(qBlue(now) - target) < std::abs(qBlue(raw) - target));
        f.project.undoStack()->undo();
        QVERIFY(f.grade(f.clips[0]).isEmpty() || f.grade(f.clips[0]).nodes[0].value(gk::GainR, 0) == 1.0);

        // The outline outside the picture cannot be measured: no change, a message.
        cm::ChartQuad outside = panel->quad();
        outside.corner[1] = QPointF(1.6, 0.1);
        panel->setQuad(outside);
        const int again = f.steps();
        QVERIFY(!panel->match());
        QCOMPARE(f.steps(), again);
        QVERIFY(!panel->statusText().isEmpty());

        // Shot Match to This Clip: clips 2 and 3 get the look of clip 1 (graded thumbnails).
        ColorClipStrip* strip = page->clipStrip();
        QImage dull(112, 63, QImage::Format_ARGB32);
        QImage warm(112, 63, QImage::Format_ARGB32);
        for (int y = 0; y < 63; ++y) {
            for (int x = 0; x < 112; ++x) {
                const int v = 40 + x + y / 2;
                dull.setPixel(x, y, qRgb(v / 2, v / 2, v));
                warm.setPixel(x, y, qRgb(std::min(255, v * 2 - 20), v + 20, v / 2));
            }
        }
        page->frames()->setThumbnailImage(f.clips[0], warm);
        page->frames()->setThumbnailImage(f.clips[1], dull);
        page->frames()->setThumbnailImage(f.clips[2], dull);
        const int before2 = f.steps();
        QCOMPARE(strip->shotMatch(f.clips[0], {f.clips[0], f.clips[1], f.clips[2]}), 2);
        QCOMPARE(f.steps(), before2 + 1); // both clips in one step
        QVERIFY(f.grade(f.clips[0]).isEmpty()); // the target is not touched
        for (const quint64 clip : {f.clips[1], f.clips[2]}) {
            const ClipGrade cg = f.grade(clip);
            QCOMPARE(cg.nodes.size(), 1);
            QCOMPARE(cg.nodes[0].label, cm::kShotMatchLabel);
            QVERIFY(cg.nodes[0].value(gk::GainR, 0) > 1.0); // dull is less red than warm
        }
        f.project.undoStack()->undo();
        QVERIFY(f.grade(f.clips[1]).isEmpty() && f.grade(f.clips[2]).isEmpty());
        // Without a thumbnail of the target nothing happens.
        page->frames()->setThumbnailImage(f.clips[0], QImage());
        QCOMPARE(strip->shotMatch(f.clips[0], {f.clips[1]}), 0);
    }

    void sizingPalette()
    {
        Fixture f;
        f.showPage();
        ColorPage* page = f.page();
        auto* tab = page->findChild<QAbstractButton*>(QStringLiteral("ColorPaletteTab_12"));
        QVERIFY(tab);
        QCOMPARE(tab->text(), QStringLiteral("Sizing"));
        tab->click();
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Sizing);
        SizingPanel* s = page->sizing();
        QVERIFY(s && s->isVisible());
        for (const char* name : {"zoom", "pan", "tilt", "rotate", "width", "height"}) {
            QVERIFY2(s->field(QLatin1String(name)) && s->field(QLatin1String(name))->isVisible(), name);
        }
        QVERIFY(s->flipButton(false)->isVisible() && s->flipButton(true)->isVisible());
        const quint64 uid = f.project.activeTimelineUid();
        f.project.timeline()->setPlayhead(10);
        const auto transform = [&] { return f.project.clipTransform(uid, f.clips[0]); };
        // Zoom is one undo step and lands in the clip's transform: the Inspector's own values.
        const int steps = f.steps();
        Q_EMIT s->field(QStringLiteral("zoom"))->valueCommitted(1.5);
        QCOMPARE(f.steps(), steps + 1);
        QCOMPARE(transform().value(keys::ZoomX, 10), 1.5);
        QCOMPARE(transform().value(keys::ZoomY, 10), 1.5);
        QCOMPARE(s->field(QStringLiteral("width"))->value(), 1.5);
        // Width changes one axis; Zoom shows the geometric mean.
        Q_EMIT s->field(QStringLiteral("width"))->valueCommitted(2.0);
        QCOMPARE(transform().value(keys::ZoomX, 10), 2.0);
        QCOMPARE(transform().value(keys::ZoomY, 10), 1.5);
        QVERIFY(std::abs(s->field(QStringLiteral("zoom"))->value() - std::sqrt(3.0)) < 1e-3);
        Q_EMIT s->field(QStringLiteral("height"))->valueCommitted(2.0);
        Q_EMIT s->field(QStringLiteral("pan"))->valueCommitted(40.0);
        Q_EMIT s->field(QStringLiteral("tilt"))->valueCommitted(25.0); // up
        Q_EMIT s->field(QStringLiteral("rotate"))->valueCommitted(12.0);
        QCOMPARE(transform().value(keys::PosX, 10), 40.0);
        QCOMPARE(transform().value(keys::PosY, 10), -25.0);
        QCOMPARE(transform().value(keys::Rotation, 10), 12.0);
        s->flipButton(false)->click();
        QVERIFY(transform().staticValue(keys::FlipH, false).toBool());
        s->flipButton(false)->click();
        QVERIFY(!transform().staticValue(keys::FlipH, false).toBool());
        // The Inspector (the clip's transform) and the palette are the same data.
        ParamSet t = transform();
        t.setBase(keys::PosX, 7.0);
        QVERIFY(f.project.setClipTransform(uid, f.clips[0], t, QStringLiteral("Inspector")));
        QCOMPARE(s->field(QStringLiteral("pan"))->value(), 7.0);
        // Undo goes back through the steps.
        f.project.undoStack()->undo();
        QCOMPARE(s->field(QStringLiteral("pan"))->value(), 40.0);
        // A drag is one step.
        const int before = f.steps();
        ValueField* pan = s->field(QStringLiteral("pan"));
        Q_EMIT pan->scrubStarted();
        Q_EMIT pan->valueScrubbed(50.0);
        Q_EMIT pan->valueScrubbed(60.0);
        Q_EMIT pan->scrubFinished();
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(transform().value(keys::PosX, 10), 60.0);
        // Output Sizing is the timeline's own set: another clip is not touched.
        s->scopeButton(1)->click();
        QCOMPARE(s->scope(), ColorSession::SizingScope::Output);
        Q_EMIT s->field(QStringLiteral("zoom"))->valueCommitted(1.25);
        QCOMPARE(f.project.outputSizing(uid).value(keys::ZoomX, 10), 1.25);
        QCOMPARE(f.project.outputSizing(uid).value(keys::ZoomY, 10), 1.25);
        QCOMPARE(transform().value(keys::PosX, 10), 60.0);
        QCOMPARE(f.project.clipTransform(uid, f.clips[1]).value(keys::ZoomX, 0), 1.0);
        s->flipButton(true)->click();
        QVERIFY(f.project.outputSizing(uid).staticValue(keys::FlipV, false).toBool());
        // The palette bar's Reset puts the shown scope back as one step.
        auto* reset = page->findChild<QToolButton*>(QStringLiteral("ColorPaletteReset"));
        QVERIFY(reset);
        const int resetSteps = f.steps();
        reset->click();
        QCOMPARE(f.steps(), resetSteps + 1);
        QVERIFY(f.project.outputSizing(uid).values.isEmpty());
        QVERIFY(!f.project.outputSizing(uid).staticValue(keys::FlipV, false).toBool());
        QCOMPARE(s->field(QStringLiteral("zoom"))->value(), 1.0);
        // Input Sizing is unavailable while the timeline grade is open.
        f.session()->setTarget(ColorSession::Target::Timeline);
        QVERIFY(!f.session()->hasSizing(ColorSession::SizingScope::Input));
        QVERIFY(!s->scopeButton(0)->isEnabled());
        f.session()->setTarget(ColorSession::Target::Clip);
        // The layout remembers the palette.
        QSettings settings;
        settings.beginGroup(QStringLiteral("colorSizingLayoutTest"));
        page->saveLayout(settings);
        page->showPalette(ColorPage::Palette::Primaries);
        page->restoreLayout(settings);
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Sizing);
        QVERIFY(!page->grab().isNull());
    }

    void keyframesPalette()
    {
        Fixture f;
        f.showPage();
        ColorPage* page = f.page();
        auto* tab = page->findChild<QAbstractButton*>(QStringLiteral("ColorPaletteTab_13"));
        QVERIFY(tab);
        QCOMPARE(tab->text(), QStringLiteral("Keyframes"));
        tab->click();
        QCOMPARE(page->currentPalette(), ColorPage::Palette::Keyframes);
        ColorKeyframesPanel* kf = page->keyframes();
        QVERIFY(kf && kf->isVisible());
        ColorKeyStrip* strip = kf->strip();
        ColorSession* session = f.session();
        Timeline* timeline = f.project.timeline();
        // An ungraded clip: node 01 is made by the first edit; rows All, 01, Input, Output.
        QVERIFY(session->setParam(0, gk::GainY, 1.2, QStringLiteral("Gain")));
        QCOMPARE(strip->rowCount(), 4);
        QCOMPARE(strip->rowLabel(0), QStringLiteral("All"));
        QCOMPARE(strip->rowLabel(1), QStringLiteral("01"));
        QCOMPARE(strip->rowLabel(2), QStringLiteral("Input Sizing"));
        QCOMPARE(strip->rowLabel(3), QStringLiteral("Output Sizing"));
        QVERIFY(strip->isVisible() && strip->laneRect(1).height() > 4);
        // Add (All): every row gets keys at the playhead, one undo step.
        timeline->setPlayhead(0);
        int steps = f.steps();
        kf->addButton()->click();
        QCOMPARE(f.steps(), steps + 1);
        for (int row = 0; row < 4; ++row)
            QCOMPARE(strip->frames(row), QVector<int64_t> {0});
        QCOMPARE(strip->selection(), (ColorKeyStrip::Selection {0, 0}));
        timeline->setPlayhead(20);
        kf->addButton()->click();
        for (int row = 0; row < 4; ++row)
            QCOMPARE(strip->frames(row), (QVector<int64_t> {0, 20}));
        // Every grade parameter is keyed.
        for (const QString& key : gk::allKeys())
            QVERIFY2(session->currentNode()->params.values.value(key).keys.size() == 2, qPrintable(key));
        // Dynamic keys interpolate: a value edited at frame 20 changes the middle.
        QVERIFY(session->setParam(0, gk::GainY, 2.0, QStringLiteral("Gain")));
        QVERIFY(std::abs(session->currentNode()->value(gk::GainY, 10) - 1.6) < 1e-6);
        // Static holds the value until the next key.
        strip->select(1, 0);
        QVERIFY(kf->dynamicButton()->isChecked() && !kf->staticButton()->isChecked());
        steps = f.steps();
        kf->staticButton()->click();
        QCOMPARE(f.steps(), steps + 1);
        QVERIFY(kf->staticButton()->isChecked() && !kf->dynamicButton()->isChecked());
        QCOMPARE(int(session->rowKeyMode(session->keyRows()[0], 0)), int(keyrows::Mode::Static));
        QVERIFY(std::abs(session->currentNode()->value(gk::GainY, 10) - 1.2) < 1e-6);
        QVERIFY(std::abs(session->currentNode()->value(gk::GainY, 20) - 2.0) < 1e-6);
        kf->dynamicButton()->click();
        QVERIFY(std::abs(session->currentNode()->value(gk::GainY, 10) - 1.6) < 1e-6);
        // Dissolve type: Ease Out leaves the key slowly.
        QCOMPARE(kf->dissolveCombo()->count(), 5);
        kf->dissolveCombo()->setCurrentIndex(2);
        Q_EMIT kf->dissolveCombo()->activated(2);
        QCOMPARE(int(session->rowKeyInterp(session->keyRows()[0], 0)), int(Interp::EaseOut));
        QVERIFY(session->currentNode()->value(gk::GainY, 10) < 1.59);
        kf->dissolveCombo()->setCurrentIndex(0);
        Q_EMIT kf->dissolveCombo()->activated(0);
        QVERIFY(std::abs(session->currentNode()->value(gk::GainY, 10) - 1.6) < 1e-6);
        // Move a key with the mouse: one undo step, the keys of the row move together.
        strip->clearSelection();
        steps = f.steps();
        const int y = strip->laneRect(1).center().y();
        QTest::mousePress(strip, Qt::LeftButton, Qt::NoModifier,
            QPoint(int(std::lround(strip->frameToX(20))), y));
        for (const int64_t frame : {24, 28, 30})
            QTest::mouseMove(strip, QPoint(int(std::lround(strip->frameToX(frame))), y));
        QTest::mouseRelease(strip, Qt::LeftButton, Qt::NoModifier,
            QPoint(int(std::lround(strip->frameToX(30))), y));
        QCOMPARE(f.steps(), steps + 1);
        QCOMPARE(strip->frames(1), (QVector<int64_t> {0, 30}));
        QCOMPARE(strip->frames(2), (QVector<int64_t> {0, 20})); // another row stays
        QCOMPARE(strip->selection(), (ColorKeyStrip::Selection {1, 30}));
        QVERIFY(std::abs(session->currentNode()->value(gk::GainY, 30) - 2.0) < 1e-6);
        // Previous / next keyframe move the playhead.
        timeline->setPlayhead(10);
        kf->nextButton()->click();
        QCOMPARE(session->stripPlayhead(), int64_t(30));
        kf->previousButton()->click();
        QCOMPARE(session->stripPlayhead(), int64_t(0));
        // Delete removes the selected key (this row only); undo brings it back.
        strip->select(1, 30);
        steps = f.steps();
        kf->deleteButton()->click();
        QCOMPARE(f.steps(), steps + 1);
        QCOMPARE(strip->frames(1), QVector<int64_t> {0});
        QCOMPARE(strip->frames(2), (QVector<int64_t> {0, 20}));
        f.project.undoStack()->undo();
        QCOMPARE(strip->frames(1), (QVector<int64_t> {0, 30}));
        // The Master row selects the keys of every row at its frame.
        strip->select(0, 20);
        QCOMPARE(strip->targets(strip->selection()).size(), 2); // Input and Output (node moved)
        kf->deleteButton()->click();
        QCOMPARE(strip->frames(2), QVector<int64_t> {0});
        QCOMPARE(strip->frames(3), QVector<int64_t> {0});
        // Auto Keyframe: another clip, an edit with the toggle on sets a key at the playhead.
        timeline->setPlayhead(60); // clip 2, local frame 10
        QVERIFY(session->setParam(0, gk::Contrast, 1.3, QStringLiteral("Contrast")));
        QVERIFY(!session->currentNode()->params.values.value(gk::Contrast).isAnimated());
        kf->autoButton()->click();
        QVERIFY(session->autoKeyframe() && kf->autoButton()->isChecked());
        QVERIFY(session->setParam(0, gk::Saturation, 1.4, QStringLiteral("Saturation")));
        const AnimatedValue& sat = session->currentNode()->params.values.value(gk::Saturation);
        QCOMPARE(sat.keys.size(), 1);
        QCOMPARE(sat.keys[0].frame, int64_t(10));
        QVERIFY(session->setSizing(ColorSession::SizingScope::Input, {{keys::PosX, 30.0}}, QStringLiteral("Pan")));
        QVERIFY(session->sizingOf(ColorSession::SizingScope::Input).values.value(keys::PosX).hasKeyAt(10));
        // Output Sizing keys are timeline frames; the strip counts from the clip's start.
        const ColorSession::KeyRow output = session->keyRows().last();
        QVERIFY(output.kind == ColorSession::KeyRow::Kind::Output);
        QVERIFY(session->setSizing(ColorSession::SizingScope::Output, {{keys::PosY, 9.0}}, QStringLiteral("Tilt")));
        QCOMPARE(session->rowFrames(output), QVector<int64_t> {10});
        const quint64 uid = f.project.activeTimelineUid();
        QVERIFY(f.project.outputSizing(uid).values.value(keys::PosY).hasKeyAt(60));
        QVERIFY(session->moveRowKey(output, 10, 15));
        QVERIFY(f.project.outputSizing(uid).values.value(keys::PosY).hasKeyAt(65));
        QVERIFY(!f.project.outputSizing(uid).values.value(keys::PosY).hasKeyAt(60));
        kf->autoButton()->click();
        QVERIFY(!session->autoKeyframe());
        // Everything paints.
        QVERIFY(!kf->grab().isNull());
        // The palette bar's Reset removes the keys of the selected row, as one step.
        strip->select(1, 0);
        steps = f.steps();
        auto* reset = page->findChild<QToolButton*>(QStringLiteral("ColorPaletteReset"));
        QVERIFY(reset);
        QCOMPARE(strip->frames(1), QVector<int64_t> {10});
        reset->click();
        QCOMPARE(f.steps(), steps + 1);
        QVERIFY(strip->frames(1).isEmpty());
    }

    /// Writes a demo project for screenshots when KK_COLOR_DEMO_PROJECT names a file (the
    /// Color page screenshots in docs; skipped otherwise).
    void demoProject()
    {
        const QString path = qEnvironmentVariable("KK_COLOR_DEMO_PROJECT");
        if (path.isEmpty())
            QSKIP("KK_COLOR_DEMO_PROJECT not set");
        QString why;
        if (!test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Project project;
        const ImportResult imported
            = project.importMedia({test::fixture("h264_25fps.mp4"), test::fixture("still.jpg"),
                test::fixture("rotated_portrait.mp4"), test::fixture("perf_1080p.mp4")});
        QVERIFY(imported.importedIds.size() >= 2);
        project.beginEdit(QStringLiteral("Demo"));
        TimelineData& tl = project.timeline()->mutableData();
        int64_t at = 0;
        QVector<quint64> ids;
        for (int round = 0; round < 3; ++round) {
            for (const QString& media : imported.importedIds) {
                ClipData clip;
                clip.kind = ClipKind::Media;
                clip.mediaId = media;
                clip.start = at;
                clip.duration = 40;
                at += 40;
                ids.append(editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, clip));
            }
        }
        project.endEdit();
        // Grades: a warm serial pair with a window, a parallel node, a qualifier.
        ClipGrade g;
        const quint64 a = gradeops::initialize(g);
        g.node(a)->params.animated(gk::GainR, 1.0).setValue(0, 1.12);
        g.node(a)->params.animated(gk::LiftB, 0.0).setValue(0, 0.03);
        const quint64 b = gradeops::addSerialNode(g, a);
        gradeops::addWindow(g, b, GradeWindowShape::Circle);
        g.node(b)->params.animated(gk::GammaY, 0.0).setValue(0, 0.05);
        const quint64 c = gradeops::addParallelNode(g, b);
        g.node(c)->qualifier.enabled = true;
        g.node(c)->qualifier.hueCenter = 0.08;
        g.node(c)->params.animated(gk::Saturation, 1.0).setValue(0, 1.3);
        QVERIFY(project.setClipGrade(project.activeTimelineUid(), ids[0], g, QStringLiteral("G")));
        QVERIFY(project.setClipGrade(project.activeTimelineUid(), ids[2], g, QStringLiteral("G")));
        QImage still(320, 180, QImage::Format_RGB32);
        still.fill(QColor(120, 90, 70));
        project.gallery()->addStill(still, g);
        project.gallery()->addStill(still, ClipGrade {});
        QVERIFY(project.save(path));
    }
};

QTEST_MAIN(TestColorPageUi)
#include "test_colorpageui.moc"
