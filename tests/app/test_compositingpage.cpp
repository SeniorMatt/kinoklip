// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the Compositing page (docs/behavior-spec.md 42b): page enabling (Shift+5),
// opening the selected clip's composition, the empty state, tool bar and node editor keys,
// viewer assignment with 1 / 2, inspector edits as undo steps, Reset Composition and layout
// save / restore. With KK_SCREENSHOT_DIR set it also saves a screenshot of the page with a
// small graph (MediaIn, Background, Text, Merge, Blur, MediaOut).
#include "../media/MediaTestUtil.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/pages/CompositingPage.h"
#include "core/CompOps.h"
#include "core/EditOps.h"
#include "core/NodeRegistry.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/RenderTypes.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/comp/CompAdapter.h"
#include "ui/comp/CompInspector.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/CompTransport.h"
#include "ui/comp/CompViewer.h"
#include "ui/comp/CompViewerOverlay.h"
#include "ui/comp/NodeEditor.h"
#include "ui/comp/NodeLibrary.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"
#include "ui/inspector/ValueField.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>

#include <memory>

using namespace kk;

namespace {

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<PlaybackEngine> playback; ///< only with withEngine (viewers render)
    std::unique_ptr<MainWindow> window;
    quint64 clipId = 0;

    explicit Fixture(bool withClip = true, bool withEngine = false)
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        if (withEngine) {
            PlaybackEngine::Options options;
            options.noAudio = true;
            playback = std::make_unique<PlaybackEngine>(std::move(options));
            playback->setMediaPool(project.mediaPool());
            playback->setTimeline(project.timeline());
            ctx.playback = playback.get();
        }
        if (withClip) {
            ClipData clip;
            clip.kind = ClipKind::Solid;
            clip.name = QStringLiteral("Plate");
            clip.start = 10;
            clip.duration = 50;
            project.beginEdit(QStringLiteral("Add Clip"));
            clipId = editops::overwriteClip(
                project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
            project.endEdit();
        }
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }

    QAction* action(const char* id) const { return actions.action(QLatin1String(id)); }
    CompositingPage* page() const { return window->compositingPage(); }
    CompSession* session() const { return page()->session(); }
    NodeEditor* editor() const { return page()->nodeEditor(); }
    const Composition& comp() const { return session()->composition(); }
    int steps() const { return project.undoStack()->count(); }
    const ClipData* clip() const { return editops::clipById(project.timeline()->data(), clipId); }
    quint64 nodeOfType(const QString& type) const
    {
        for (const CompNode& n : comp().nodes)
            if (n.type == type)
                return n.id;
        return 0;
    }
    quint64 nodeNamed(const QString& name) const
    {
        for (const CompNode& n : comp().nodes)
            if (n.name == name)
                return n.id;
        return 0;
    }

    void showPage()
    {
        window->resize(1800, 1000);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        action("view.page_compositing")->trigger();
        QCoreApplication::processEvents();
    }
    /// Key @p key (with @p mods) to the node editor, through the shortcut override path.
    void key(Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        editor()->setFocus();
        QTest::keyClick(editor(), key, mods);
        QCoreApplication::processEvents();
    }
};

} // namespace

class TestCompositingPage : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_compositingpage"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // ---- page wiring ----
    void pageEnabledWithShift5()
    {
        Fixture f;
        QVERIFY(f.page());
        QVERIFY(f.action("view.page_compositing")->isEnabled());
        QCOMPARE(f.actions.shortcuts(QStringLiteral("view.page_compositing")).value(0),
            QKeySequence(QStringLiteral("Shift+5")));
        f.showPage();
        QCOMPARE(f.window->currentPageId(), QStringLiteral("compositing"));
        // The interface bar offers Effects (library), Nodes and Inspector there.
        for (const char* id : {"view.toggle_effects", "view.toggle_nodes", "view.toggle_inspector"})
            QVERIFY2(f.action(id)->isEnabled(), id);
        QVERIFY(!f.action("view.toggle_mixer")->isEnabled());
        QVERIFY(!f.page()->isPanelVisible(PanelId::Effects));
        f.action("view.toggle_effects")->trigger();
        QVERIFY(f.page()->isPanelVisible(PanelId::Effects));
        f.action("view.toggle_nodes")->trigger();
        QVERIFY(!f.page()->isPanelVisible(PanelId::Nodes));
        f.action("view.toggle_nodes")->trigger();
        QVERIFY(f.page()->isPanelVisible(PanelId::Nodes));
    }

    void opensClipUnderPlayheadWithoutStoring()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        QCOMPARE(f.session()->clipId(), f.clipId);
        QCOMPARE(f.comp().nodes.size(), 2);
        QVERIFY(!f.clip()->composition); // viewing alone stores nothing
        QCOMPARE(f.session()->compFrame(), 10);
        QVERIFY(f.page()->transport()->positionText().startsWith(QStringLiteral("Frame 10")));
        // The right viewer shows MediaOut by default.
        QVERIFY(f.page()->viewer(1)->headerText().contains(QStringLiteral("MediaOut1")));
        QVERIFY(f.page()->viewer(0)->headerText().contains(QStringLiteral("No Node")));
    }

    void opensSelectedClipAndEmptyState()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(0); // before the clip
        f.showPage();
        QCOMPARE(f.session()->clipId(), quint64(0)); // nothing under the playhead
        QCOMPARE(f.page()->inspector()->shownNode(), quint64(0));
        // The menu command opens the selected clip.
        f.action("view.page_edit")->trigger();
        f.selection.setClips({f.clipId});
        f.action("clip.open_in_compositing")->trigger();
        QCOMPARE(f.window->currentPageId(), QStringLiteral("compositing"));
        QCOMPARE(f.session()->clipId(), f.clipId);
    }

    void toolbarAndKeysAreUndoSteps()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.session()->setSelection({in});
        int before = f.steps();
        QToolButton* blur
            = f.page()->toolButton(NodeRegistry::effectNodeType(QStringLiteral("gaussian_blur")));
        QVERIFY(blur);
        QVERIFY(f.page()->toolButton(nodetypes::Background));
        QCOMPARE(compadapter::toolbarTypes().size(), 10);
        blur->click();
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(f.clip()->composition); // created on the first edit
        const quint64 b = f.session()->currentNode();
        QCOMPARE(f.comp().node(b)->inputs.value(nodeports::Input).nodeId, in);

        // Ctrl+P pass-through, again on, F2 rename, Ctrl+C / Ctrl+V, Delete: keys of the
        // node editor (Ctrl+C would copy clips elsewhere).
        f.key(Qt::Key_P, Qt::ControlModifier);
        QVERIFY(!f.comp().node(b)->enabled);
        QVERIFY(f.action("comp.pass_through")->isChecked());
        f.key(Qt::Key_P, Qt::ControlModifier);
        QVERIFY(f.comp().node(b)->enabled);
        f.key(Qt::Key_F2);
        QVERIFY(f.editor()->renameField());
        f.editor()->renameField()->setText(QStringLiteral("Soft"));
        QTest::keyClick(f.editor()->renameField(), Qt::Key_Return);
        QCOMPARE(f.comp().node(b)->name, QStringLiteral("Soft"));
        const int nodes = int(f.comp().nodes.size());
        f.key(Qt::Key_C, Qt::ControlModifier);
        f.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(int(f.comp().nodes.size()), nodes + 1);
        f.key(Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(int(f.comp().nodes.size()), nodes + 2);
        before = f.steps();
        f.key(Qt::Key_Delete);
        QCOMPARE(int(f.comp().nodes.size()), nodes + 1);
        QCOMPARE(f.steps(), before + 1);
        // The timeline clip is untouched by those keys.
        QVERIFY(f.clip());
        // Ctrl+Z goes back step by step.
        f.project.undo();
        QCOMPARE(int(f.comp().nodes.size()), nodes + 2);

        // Shift+Space: the Select Node popup.
        f.session()->setSelection({b});
        f.key(Qt::Key_Space, Qt::ShiftModifier);
        QVERIFY(f.page()->selectNodePopup()->isVisible());
        QTest::keyClicks(f.page()->selectNodePopup()->searchField(), QStringLiteral("Transform"));
        QTest::keyClick(f.page()->selectNodePopup()->searchField(), Qt::Key_Return);
        QVERIFY(f.nodeNamed(QStringLiteral("Transform1")));
    }

    void swapInputsWithCtrlT()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.session()->setSelection({in});
        f.page()->toolButton(nodetypes::Background)->click(); // adds a Merge over MediaIn
        const quint64 bg = f.nodeNamed(QStringLiteral("Background1"));
        const quint64 merge = f.nodeNamed(QStringLiteral("Merge1"));
        QVERIFY(bg && merge);
        QCOMPARE(f.comp().node(merge)->inputs.value(nodeports::Foreground).nodeId, bg);
        f.session()->setSelection({merge});
        f.key(Qt::Key_T, Qt::ControlModifier);
        QCOMPARE(f.comp().node(merge)->inputs.value(nodeports::Background).nodeId, bg);
        QCOMPARE(f.comp().node(merge)->inputs.value(nodeports::Foreground).nodeId, in);
    }

    void viewerAssignmentWith1And2()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.session()->setSelection({in});
        const int before = f.steps();
        f.key(Qt::Key_1);
        QCOMPARE(f.session()->viewerNode(0), in);
        QVERIFY(f.page()->viewer(0)->headerText().contains(QStringLiteral("MediaIn1")));
        f.key(Qt::Key_2);
        QCOMPARE(f.session()->viewerNode(1), in);
        QVERIFY(f.page()->viewer(1)->headerText().contains(QStringLiteral("MediaIn1")));
        QCOMPARE(f.steps(), before); // viewing is not an edit
        QVERIFY(!f.clip()->composition);
        // The next edit saves the viewers with the composition.
        f.session()->renameNode(in, QStringLiteral("Plate"));
        QCOMPARE(f.clip()->composition->leftViewerNode, in);
        QVERIFY(f.page()->viewer(0)->headerText().contains(QStringLiteral("Plate")));
    }

    void inspectorEditsAreUndoSteps()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.session()->setSelection({in});
        f.page()->toolButton(nodetypes::Transform)->click();
        const quint64 t = f.nodeNamed(QStringLiteral("Transform1"));
        QCOMPARE(f.page()->inspector()->shownNode(), t);
        ParamGroupBox* group = f.page()->inspector()->group();
        QVERIFY(group);
        ParamRow* sizeRow = nullptr;
        for (ParamRow* row : group->rows())
            if (row->ref().key == nodekeys::Size)
                sizeRow = row;
        QVERIFY(sizeRow);
        // Write through the host like a typed value does: one step, the node changes.
        const int before = f.steps();
        CompInspector* host = f.page()->inspector();
        host->editClip(QStringLiteral("Change Size"), CompInspector::kProxyClipId,
            [](ClipData& c) { c.effects.first().params.setBase(nodekeys::Size, 2.0); });
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(f.comp().node(t)->params.value(nodekeys::Size, 0, 0), 2.0);
        // A gesture (slider drag) is one step too.
        host->beginParamEdit(QStringLiteral("Change Angle"));
        for (int i = 1; i <= 5; ++i)
            host->modifyClip(CompInspector::kProxyClipId,
                [i](ClipData& c) { c.effects.first().params.setBase(nodekeys::Angle, i * 10.0); });
        host->endParamEdit();
        QCOMPARE(f.steps(), before + 2);
        QCOMPARE(f.comp().node(t)->params.value(nodekeys::Angle, 0, 0), 50.0);
        // A key at the comp frame (keyframe button = toggleKey through the host).
        host->editClip(
            QStringLiteral("Add Keyframe"), CompInspector::kProxyClipId, [](ClipData& c) {
                c.effects.first().params.animated(nodekeys::Size, 1.0).setKey(10, 2.0);
            });
        QCOMPARE(f.page()->transport()->ruler()->keyFrames(), QVector<int64_t> {10});
        f.project.undo();
        f.project.undo();
        QCOMPARE(f.comp().node(t)->params.value(nodekeys::Angle, 0, 0), 0.0);
        // Header: the enable toggle is pass-through.
        f.page()->inspector()->enableToggle()->click();
        QVERIFY(!f.comp().node(t)->enabled);
    }

    void viewerHandlesMoveTheCentre()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.session()->setSelection({f.nodeNamed(QStringLiteral("MediaIn1"))});
        f.page()->toolButton(nodetypes::Transform)->click();
        const quint64 t = f.nodeNamed(QStringLiteral("Transform1"));
        QCOMPARE(f.session()->currentNode(), t);
        CompViewerOverlay* overlay = f.page()->viewer(1)->controls();
        QVERIFY(overlay->hasControls());
        const auto centre = overlay->handlePos(CompViewerOverlay::Handle::Centre);
        QVERIFY(centre);
        QVERIFY(overlay->handlePos(CompViewerOverlay::Handle::Angle));
        auto send
            = [overlay](QEvent::Type type, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs) {
                  QMouseEvent e(type, p, overlay->mapToGlobal(p), b, bs, Qt::NoModifier);
                  QApplication::sendEvent(overlay, &e);
              };
        const int before = f.steps();
        send(QEvent::MouseButtonPress, *centre, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 4; ++i)
            send(QEvent::MouseMove, *centre + QPointF(10 * i, -10 * i), Qt::NoButton,
                Qt::LeftButton);
        send(QEvent::MouseButtonRelease, *centre + QPointF(40, -40), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(f.steps(), before + 1); // the whole drag is one step
        const CompNode* n = f.comp().node(t);
        QVERIFY(n->params.value(nodekeys::CenterX, 0, 0.5) > 0.5); // right
        QVERIFY(n->params.value(nodekeys::CenterY, 0, 0.5) > 0.5); // up (y from bottom to top)
        // A locked node shows no handles.
        f.session()->toggleLock();
        QCOMPARE(overlay->handleAt(*overlay->handlePos(CompViewerOverlay::Handle::Centre)),
            CompViewerOverlay::Handle::None);
    }

    void inspectorNoteForNodesWithoutParameters()
    {
        // Polish: MediaIn / MediaOut have no parameters; the Inspector says so instead of an
        // empty group to unfold.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        auto note = [&f] {
            for (QLabel* l :
                f.page()->inspector()->findChildren<QLabel*>(QStringLiteral("PlaceholderText")))
                if (l->isVisibleTo(f.page()->inspector())
                    && l->text() == QStringLiteral("This node has no parameters"))
                    return true;
            return false;
        };
        f.session()->setSelection({f.comp().mediaOutId});
        QVERIFY(note());
        f.session()->addNode(nodetypes::Transform);
        f.session()->setSelection({f.nodeNamed(QStringLiteral("Transform1"))});
        QVERIFY(!note());
        auto* group
            = f.page()->inspector()->findChild<ParamGroupBox*>(QStringLiteral("CompNodeParams"));
        QVERIFY(group && group->isVisibleTo(f.page()->inspector()));
    }

    void viewerMaskAndPolygonHandles()
    {
        // Spec 42b.15: Rectangle and Ellipse masks show centre / size / width / height
        // handles, a Polygon its points; every drag is one undo step.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        CompViewerOverlay* overlay = f.page()->viewer(1)->controls();
        auto send
            = [overlay](QEvent::Type type, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs) {
                  QMouseEvent e(type, p, overlay->mapToGlobal(p), b, bs, Qt::NoModifier);
                  QApplication::sendEvent(overlay, &e);
              };
        auto drag = [&](QPointF from, QPointF to) {
            send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= 4; ++i)
                send(QEvent::MouseMove, from + (to - from) * i / 4.0, Qt::NoButton, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        };
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const int64_t frame = f.session()->compFrame();
        for (const QString& type :
            {QString(nodetypes::RectangleMask), QString(nodetypes::EllipseMask)}) {
            f.session()->setSelection({in});
            f.page()->toolButton(type)->click();
            const quint64 m = f.nodeOfType(type);
            QVERIFY2(m, qPrintable(type));
            f.session()->setSelection({m});
            QVERIFY(overlay->hasControls());
            const compadapter::ViewerControlKeys k = compadapter::viewerControls(type);
            auto value = [&](const QString& key) {
                return f.comp().node(m)->params.value(key, frame, -1.0);
            };
            // The centre: right and down (node y goes up).
            const QPointF c = *overlay->handlePos(CompViewerOverlay::Handle::Centre);
            QCOMPARE(overlay->handleAt(c), CompViewerOverlay::Handle::Centre);
            int before = f.steps();
            const double x0 = value(k.centerX), y0 = value(k.centerY);
            drag(c, c + QPointF(30, 20));
            QCOMPARE(f.steps(), before + 1);
            QVERIFY(value(k.centerX) > x0 + 0.01);
            QVERIFY(value(k.centerY) < y0 - 0.01);
            // The corner: width and height grow together.
            const QPointF corner = *overlay->handlePos(CompViewerOverlay::Handle::Size);
            QCOMPARE(overlay->handleAt(corner), CompViewerOverlay::Handle::Size);
            before = f.steps();
            const double w0 = value(k.width), h0 = value(k.height);
            drag(corner, corner + QPointF(20, 16));
            QCOMPARE(f.steps(), before + 1);
            QVERIFY(value(k.width) > w0 + 0.01);
            QVERIFY(value(k.height) > h0 + 0.01);
            // The right edge: only the width.
            const QPointF edge = *overlay->handlePos(CompViewerOverlay::Handle::Width);
            QCOMPARE(overlay->handleAt(edge), CompViewerOverlay::Handle::Width);
            before = f.steps();
            const double w1 = value(k.width), h1 = value(k.height);
            drag(edge, edge - QPointF(15, 0));
            QCOMPARE(f.steps(), before + 1);
            QVERIFY(value(k.width) < w1 - 0.005);
            QCOMPARE(value(k.height), h1);
            // Undo takes back exactly the last drag.
            f.project.undo();
            QCOMPARE(value(k.width), w1);
        }

        f.session()->setSelection({in});
        f.page()->toolButton(nodetypes::PolygonMask)->click();
        const quint64 poly = f.nodeOfType(nodetypes::PolygonMask);
        QVERIFY(poly);
        f.session()->setSelection({poly});
        const MaskPath path0 = f.comp().node(poly)->path.at(double(frame));
        QVERIFY(path0.size() >= 3);
        const QPointF p = *overlay->handlePos(CompViewerOverlay::Handle::Point, 1);
        int index = -1;
        QCOMPARE(overlay->handleAt(p, &index), CompViewerOverlay::Handle::Point);
        QCOMPARE(index, 1);
        QVERIFY(!overlay->handlePos(CompViewerOverlay::Handle::Centre)); // points only
        const int before = f.steps();
        drag(p, p + QPointF(12, 9));
        QCOMPARE(f.steps(), before + 1);
        const MaskPath path1 = f.comp().node(poly)->path.at(double(frame));
        QCOMPARE(path1.size(), path0.size());
        QVERIFY(path1[1].x > path0[1].x + 1); // mask-local px, y down like the screen
        QVERIFY(path1[1].y > path0[1].y + 1);
        for (int i = 0; i < path0.size(); ++i) {
            if (i != 1)
                QVERIFY(path1[i].x == path0[i].x && path1[i].y == path0[i].y);
        }
        // The handle follows the point.
        const QPointF moved = *overlay->handlePos(CompViewerOverlay::Handle::Point, 1);
        QVERIFY(std::abs(moved.x() - (p.x() + 12)) < 1.0);
        QVERIFY(std::abs(moved.y() - (p.y() + 9)) < 1.0);
        f.project.undo();
        QCOMPARE(f.comp().node(poly)->path.at(double(frame))[1].x, path0[1].x);
    }

    void timelineCommandsSuspendedOnThePage()
    {
        // Spec 42b: the Edit page's timeline is hidden here, so its editing keys do nothing
        // (Delete, Ctrl+C, the multicam number keys, ...) unless the node editor claims them;
        // transport, undo and the page keys stay. Back on the Edit page they work again.
        Fixture f(true, true);
        f.project.timeline()->setPlayhead(20);
        f.selection.setClips({f.clipId});
        f.showPage();
        for (const char* id : {"edit.ripple_delete", "edit.delete", "edit.copy", "edit.paste",
                 "edit.split", "clip.toggle_enabled", "multicam.angle_1", "mark.in",
                 "trim.nudge_left", "timeline.target_video_1"})
            QVERIFY2(!f.action(id)->isEnabled(), id);
        for (const char* id : {"transport.play_pause", "transport.next_frame", "view.page_edit",
                 "clip.reset_composition", "mark.go_to_in", "comp.delete"})
            QVERIFY2(f.action(id)->isEnabled(), id);
        // Delete with the focus outside the node editor leaves the hidden timeline alone.
        f.page()->transport()->setFocus();
        f.action("edit.ripple_delete")->trigger();
        QTest::keyClick(f.page()->transport(), Qt::Key_Delete);
        QCoreApplication::processEvents();
        QVERIFY(f.clip());
        // The node editor still gets its own Delete (deletes the selected node, one step).
        f.session()->addNode(nodetypes::Transform);
        const quint64 t = f.nodeNamed(QStringLiteral("Transform1"));
        f.session()->setSelection({t});
        f.key(Qt::Key_Delete);
        QVERIFY(!f.comp().node(t));
        QVERIFY(f.clip());
        // Transport keys still drive the playhead.
        const int64_t at = f.project.timeline()->playhead();
        f.action("transport.next_frame")->trigger();
        QTRY_COMPARE(f.project.timeline()->playhead(), at + 1);
        // Back on the Edit page the commands work again.
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QVERIFY(f.action("edit.ripple_delete")->isEnabled());
        QVERIFY(f.action("multicam.angle_1")->isEnabled());
        f.selection.setClips({f.clipId});
        f.action("edit.ripple_delete")->trigger();
        QVERIFY(!f.clip());
        // The list never touches what must stay.
        const QStringList ids = CompositingPage::suspendedCommandIds(f.actions);
        for (const char* id : {"edit.undo", "edit.redo", "transport.play_pause",
                 "view.page_compositing", "comp.copy", "clip.reset_composition"})
            QVERIFY2(!ids.contains(QLatin1String(id)), id);
    }

    void viewerShowsMediaInThroughTheEngine()
    {
        // The viewers render through PlaybackEngine::renderCompositionNode(), which passes the
        // engine's media table and proxies: MediaIn shows the clip's picture (it used to be
        // black when the compositor had no table).
        QString why;
        if (!test::ensureFixtures(&why))
            QSKIP(qPrintable(why));
        Fixture f(false, true);
        const QString id
            = f.project.importMedia({test::fixture("h264_25fps.mp4")}).importedIds.value(0);
        QVERIFY(!id.isEmpty());
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = id;
        clip.duration = 50;
        f.project.beginEdit(QStringLiteral("Place"));
        f.clipId = editops::overwriteClip(
            f.project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
        f.project.endEdit();
        QVERIFY(f.project.setClipComposition(f.project.activeTimelineUid(), f.clipId,
            compadapter::defaultComposition(), QStringLiteral("Composition")));
        f.project.timeline()->setPlayhead(7);
        f.showPage();
        QVERIFY(f.session()->isStored());
        const quint64 in = f.nodeOfType(nodetypes::MediaIn);
        // The convenience call itself.
        const RenderedFramePtr frame = f.playback
                                           ->renderCompositionNode(f.project.timeline()->snapshot(),
                                               f.clipId, in, 7, QSize(320, 180), OutputImage)
                                           .get();
        QVERIFY(frame && frame->ok);
        QCOMPARE(test::readFrameIndex(frame->image), 7);
        // The left viewer showing MediaIn draws that picture.
        f.session()->setViewerNode(0, in);
        CompViewer* viewer = f.page()->viewer(0);
        auto shown = [viewer] {
            const QImage all = viewer->surface()->widget()->grab().toImage();
            const QRect r = viewer->surface()->pictureRect();
            return test::readFrameIndex(all.copy(r));
        };
        QTRY_COMPARE_WITH_TIMEOUT(shown(), 7, 10000);
    }

    void resetComposition()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.session()->addNode(nodetypes::Transform);
        QVERIFY(f.clip()->composition);
        f.selection.setClips({f.clipId});
        f.action("clip.reset_composition")->trigger();
        QVERIFY(!f.clip()->composition);
        QCOMPARE(f.comp().nodes.size(), 2); // the default is shown again
        f.project.undo();
        QVERIFY(f.clip()->composition);
    }

    void layoutSaveRestore()
    {
        QByteArray columns;
        {
            Fixture f;
            f.showPage();
            f.page()->setPanelVisible(PanelId::Effects, true);
            f.page()->setPanelVisible(PanelId::Inspector, false);
            auto* split = f.page()->findChild<QSplitter*>(QStringLiteral("CompCentreSplitter"));
            split->setSizes({300, 600});
            QSettings s;
            s.beginGroup(QStringLiteral("pages/compositing"));
            f.page()->saveLayout(s);
            s.endGroup();
            columns = split->saveState();
        }
        Fixture g;
        g.showPage();
        QSettings s;
        s.beginGroup(QStringLiteral("pages/compositing"));
        g.page()->restoreLayout(s);
        s.endGroup();
        QVERIFY(g.page()->isPanelVisible(PanelId::Effects));
        QVERIFY(!g.page()->isPanelVisible(PanelId::Inspector));
        QCOMPARE(g.page()->findChild<QSplitter*>(QStringLiteral("CompCentreSplitter"))->saveState(),
            columns);
    }

    void screenshotSmallGraph()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        // Background (bg) + MediaIn -> Blur (fg) + Text (mask) -> Merge -> MediaOut.
        f.session()->edit(QStringLiteral("Build"), [](Composition& c) {
            const quint64 in = c.nodes.first().id;
            const quint64 out = c.mediaOutId;
            c.node(in)->position = QPointF(0, 0);
            const quint64 blur = compadapter::addNode(
                c, NodeRegistry::effectNodeType(QStringLiteral("gaussian_blur")), QPointF(4.5, 0));
            const quint64 bg = compadapter::addNode(c, nodetypes::Background, QPointF(9, -3));
            const quint64 text = compadapter::addNode(c, nodetypes::Text, QPointF(4.5, 3));
            const quint64 merge = compadapter::addNode(c, nodetypes::Merge, QPointF(9, 0));
            c.node(out)->position = QPointF(13.5, 0);
            compadapter::connectNodes(c, in, 0, blur, nodeports::Input);
            compadapter::connectNodes(c, bg, 0, merge, nodeports::Background);
            compadapter::connectNodes(c, blur, 0, merge, nodeports::Foreground);
            compadapter::connectNodes(c, text, 0, merge, nodeports::Mask);
            compadapter::connectNodes(c, merge, 0, out, nodeports::Input);
            return true;
        });
        QCOMPARE(f.comp().node(f.comp().mediaOutId)->inputs.value(nodeports::Input).nodeId,
            f.nodeNamed(QStringLiteral("Merge1")));
        f.session()->setSelection({f.nodeNamed(QStringLiteral("Merge1"))});
        f.session()->setViewerNode(0, f.nodeNamed(QStringLiteral("MediaIn1")));
        f.editor()->fitToView();
        QCoreApplication::processEvents();
        const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR");
        if (dir.isEmpty())
            QSKIP("KK_SCREENSHOT_DIR not set");
        QDir().mkpath(dir);
        QTest::qWait(200);
        QVERIFY(f.window->grab().save(QDir(dir).filePath(QStringLiteral("compositing-page.png"))));

        // The same graph over real media as a project file, for the application's own
        // screenshot with rendered viewers (--screenshot compositing <png> <project>).
        Project demo;
        const QString media = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4");
        const QString id = demo.importMedia({media}).importedIds.value(0);
        if (id.isEmpty())
            QSKIP("test media missing");
        ClipData clip;
        clip.kind = ClipKind::Media;
        clip.mediaId = id;
        clip.name = QStringLiteral("h264_25fps");
        clip.duration = 100;
        demo.beginEdit(QStringLiteral("Place"));
        const quint64 clipId = editops::overwriteClip(
            demo.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, clip);
        demo.endEdit();
        Composition c = f.comp();
        // A half-transparent blue background with the text "Kinoklip" as the mask.
        for (CompNode& n : c.nodes) {
            if (n.type == nodetypes::Background)
                n.params.setStatic(nodekeys::Color, QColor(40, 90, 200));
            if (n.type == nodetypes::Text) {
                n.params.setStatic(nodekeys::Text, QStringLiteral("Kinoklip"));
                n.params.setBase(nodekeys::FontSize, 260);
            }
            if (n.type == nodetypes::MediaIn)
                c.leftViewerNode = n.id;
        }
        QVERIFY(demo.setClipComposition(
            demo.activeTimelineUid(), clipId, c, QStringLiteral("Composition")));
        demo.timeline()->setPlayhead(30);
        QVERIFY(demo.save(QDir(dir).filePath(QStringLiteral("compositing-demo.kkproj"))));
    }
};

QTEST_MAIN(TestCompositingPage)
#include "test_compositingpage.moc"
