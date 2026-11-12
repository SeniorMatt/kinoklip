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
#include "compositing/tracking/TrackStore.h"
#include "core/CompOps.h"
#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/MediaPool.h"
#include "core/NodeRegistry.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "playback/PlaybackEngine.h"
#include "render/RenderTypes.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/DragMime.h"
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
#include "ui/comp/TrackerPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"
#include "ui/inspector/ValueField.h"
#include "ui/keyframes/CurveView.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/keyframes/KeyframeItemModel.h"
#include "ui/keyframes/KeyframeParamTree.h"
#include "ui/keyframes/KeyframesPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QProgressBar>
#include <QSettings>
#include <QSignalSpy>
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

// A Transform node with animated Center X (frames 0, 20, 40 -> 0.2, 0.8, 0.5) and Angle.
quint64 animatedTransform(Fixture& f)
{
    const quint64 t = f.session()->addNode(nodetypes::Transform);
    f.session()->edit(QStringLiteral("Animate"), [t](Composition& c) {
        CompNode* n = c.node(t);
        AnimatedValue& x = n->params.animated(nodekeys::CenterX, 0.5);
        x.setKey(0, 0.2, Interp::Linear);
        x.setKey(20, 0.8, Interp::Linear);
        x.setKey(40, 0.4, Interp::Linear);
        AnimatedValue& a = n->params.animated(nodekeys::Angle, 0.0);
        a.setKey(0, 0.0, Interp::Linear);
        a.setKey(30, 90.0, Interp::Linear);
        return true;
    });
    f.session()->setSelection({t});
    return t;
}

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

    // Spec 42b.29: a viewer that shows no node ("No Node") draws no handles of the selected
    // node; the viewer that shows one does.
    void emptyViewerDrawsNoHandles()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.session()->setSelection({f.nodeNamed(QStringLiteral("MediaIn1"))});
        f.page()->toolButton(nodetypes::Transform)->click();
        QVERIFY(f.page()->viewer(0)->headerText().contains(QStringLiteral("No Node")));
        QVERIFY(!f.page()->viewer(0)->controls()->hasControls());
        QVERIFY(!f.page()->viewer(0)->controls()->handlePos(CompViewerOverlay::Handle::Centre));
        QVERIFY(f.page()->viewer(1)->controls()->hasControls());
        f.session()->setViewerNode(0, f.nodeNamed(QStringLiteral("Transform1")));
        QVERIFY(f.page()->viewer(0)->controls()->hasControls());
    }

    // Spec 42b.30: a media pool item dragged onto the node editor becomes an unconnected
    // MediaIn reading it (one undo step); the MediaIn Inspector's Source picks or replaces
    // the item. Items without a picture are refused. The page has a Media Pool panel.
    void mediaPoolItemDropsAsMediaIn()
    {
        Fixture f;
        MediaItem logo;
        logo.name = QStringLiteral("logo.png");
        logo.path = QStringLiteral("/nonexistent/logo.png");
        logo.info.valid = true;
        logo.info.hasVideo = true;
        logo.info.width = 400;
        logo.info.height = 200;
        const QString logoId = f.project.mediaPool()->addItem(logo);
        MediaItem music;
        music.name = QStringLiteral("music.wav");
        music.path = QStringLiteral("/nonexistent/music.wav");
        music.info.valid = true;
        music.info.hasAudio = true;
        const QString musicId = f.project.mediaPool()->addItem(music);
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        QVERIFY(f.page()->hasPanel(PanelId::MediaPool));
        QVERIFY(!f.page()->isPanelVisible(PanelId::MediaPool));
        f.page()->setPanelVisible(PanelId::MediaPool, true);
        QVERIFY(f.page()->mediaPool()->isVisible());

        auto drop = [&](const QString& ids) {
            QMimeData data;
            data.setData(QLatin1String(mime::MediaItems), ids.toUtf8());
            const QPointF at = f.editor()->rect().center();
            QDragEnterEvent enter(at.toPoint(), Qt::CopyAction, &data, Qt::LeftButton, {});
            QApplication::sendEvent(f.editor(), &enter);
            if (!enter.isAccepted())
                return false;
            QDropEvent dropEvent(at, Qt::CopyAction, &data, Qt::LeftButton, {});
            QApplication::sendEvent(f.editor(), &dropEvent);
            return dropEvent.isAccepted();
        };
        const int nodes = int(f.comp().nodes.size());
        QVERIFY(!drop(musicId)); // no picture: refused
        QCOMPARE(int(f.comp().nodes.size()), nodes);
        const int before = f.steps();
        QVERIFY(drop(logoId));
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(int(f.comp().nodes.size()), nodes + 1);
        const CompNode* in = f.comp().node(f.session()->currentNode());
        QVERIFY(in);
        QCOMPARE(in->type, nodetypes::MediaIn);
        QCOMPARE(in->mediaId, logoId);
        QVERIFY(in->inputs.isEmpty());

        // The Inspector shows and changes the source.
        QComboBox* source = f.page()->inspector()->sourcePicker();
        QVERIFY(source);
        QCOMPARE(source->currentData().toString(), logoId);
        QCOMPARE(source->findData(musicId), -1);
        const quint64 id = in->id;
        const int index = source->findData(QString());
        QVERIFY(index >= 0);
        source->setCurrentIndex(index);
        emit source->activated(index);
        QCOMPARE(f.comp().node(id)->mediaId, QString());
        QCOMPARE(f.steps(), before + 2);
        f.project.undo();
        QCOMPARE(f.comp().node(id)->mediaId, logoId);
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

    // ---- Tracker (spec 50) ----
    void trackerViewerBoxesAreDragged()
    {
        // Spec 50.3: the viewer shows the point with its pattern and search boxes; dragging
        // the point or a corner edits Center X / Y, Pattern Size or Search Size, one step each.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.session()->setSelection({f.nodeNamed(QStringLiteral("MediaIn1"))});
        const quint64 t = f.session()->addNode(nodetypes::Tracker);
        QVERIFY(t);
        f.session()->setSelection({t});
        CompViewerOverlay* overlay = f.page()->viewer(1)->controls();
        QVERIFY(overlay->hasControls());
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
        const int64_t frame = f.session()->compFrame();
        auto value
            = [&](const QString& key) { return f.comp().node(t)->params.value(key, frame, -1.0); };
        const QPointF pattern = *overlay->handlePos(CompViewerOverlay::Handle::Size);
        const QPointF search = *overlay->handlePos(CompViewerOverlay::Handle::Width);
        QVERIFY(search.x() > pattern.x() && search.y() > pattern.y()); // the larger box
        QVERIFY(!overlay->handlePos(CompViewerOverlay::Handle::Angle)); // no rotation knob
        // Move the point right and down.
        const QPointF c = *overlay->handlePos(CompViewerOverlay::Handle::Centre);
        QCOMPARE(overlay->handleAt(c), CompViewerOverlay::Handle::Centre);
        int before = f.steps();
        const double x0 = value(nodekeys::CenterX), y0 = value(nodekeys::CenterY);
        drag(c, c + QPointF(30, 20));
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(value(nodekeys::CenterX) > x0 + 0.01);
        QVERIFY(value(nodekeys::CenterY) < y0 - 0.01);
        // Grow the pattern box and the search box.
        const QPointF p2 = *overlay->handlePos(CompViewerOverlay::Handle::Size);
        QCOMPARE(overlay->handleAt(p2), CompViewerOverlay::Handle::Size);
        before = f.steps();
        const double size0 = value(nodekeys::PatternSize);
        drag(p2, p2 + QPointF(20, 20));
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(value(nodekeys::PatternSize) > size0 + 5);
        QVERIFY(value(nodekeys::SearchSize) >= value(nodekeys::PatternSize)); // never smaller
        const QPointF s2 = *overlay->handlePos(CompViewerOverlay::Handle::Width);
        QCOMPARE(overlay->handleAt(s2), CompViewerOverlay::Handle::Width);
        before = f.steps();
        const double search0 = value(nodekeys::SearchSize);
        drag(s2, s2 + QPointF(25, 25));
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(value(nodekeys::SearchSize) > search0 + 5);
    }

    void trackerPanelTracksIntoOneUndoStep()
    {
        // Spec 50.4 / 50.5: Track Forward from the playhead follows the picture off the UI
        // thread, shows progress, ends by storing the positions as keys in ONE undo step;
        // Clear removes them; Stop keeps what was tracked. The tracked picture is static noise,
        // so the point stays where it was put.
        Fixture f(true, true);
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.session()->setSelection({f.nodeNamed(QStringLiteral("MediaIn1"))});
        const quint64 noise = f.session()->addNodeAt(nodetypes::FastNoise, QPointF(0, 5));
        QVERIFY(noise);
        f.session()->edit(QStringLiteral("Noise"), [noise](Composition& c) {
            c.node(noise)->params.setBase(nodekeys::Scale, 60.0);
            c.node(noise)->params.setBase(nodekeys::Detail, 3.0);
            return true;
        });
        const quint64 t = f.session()->addNode(nodetypes::Tracker);
        QVERIFY(t);
        f.session()->connectNodes(noise, t, nodeports::Input);
        f.session()->setSelection({t});
        TrackerPanel* panel = f.page()->inspector()->trackerPanel();
        QVERIFY(panel && panel->isVisibleTo(f.page()->inspector()));
        QVERIFY(panel->forwardButton()->isEnabled());
        QVERIFY(!panel->stopButton()->isEnabled());
        const int before = f.steps();
        const int64_t start = f.session()->compFrame();
        QSignalSpy done(panel->runner(), &tracking::TrackerRunner::finished);
        panel->forwardButton()->click();
        QVERIFY(panel->isRunning());
        QVERIFY(panel->stopButton()->isEnabled());
        QVERIFY(done.wait(60000));
        QCOMPARE(
            done.first().first().value<tracking::TrackOutcome>(), tracking::TrackOutcome::Finished);
        QCOMPARE(f.steps(), before + 1); // one step for the whole track
        const AnimatedValue& x = f.comp().node(t)->params.values.value(nodekeys::CenterX);
        QCOMPARE(x.keys.size(), int(f.session()->compFrameCount() - start));
        QVERIFY(std::abs(x.valueAt(f.session()->compFrameCount() - 1) - 0.5) < 0.01);
        QCOMPARE(f.comp().node(t)->params.value(nodekeys::RefFrame, 0, -1), double(start));
        QVERIFY(panel->statusText().contains(QStringLiteral("Tracked")));
        // Undo takes the whole track back.
        f.project.undoStack()->undo();
        QVERIFY(!f.comp().node(t)->params.values.value(nodekeys::CenterX).isAnimated());
        f.project.undoStack()->redo();
        // Clear: one step, the point stays where it is.
        const int beforeClear = f.steps();
        panel->findChild<QToolButton*>(QStringLiteral("TrackClear"))->click();
        QCOMPARE(f.steps(), beforeClear + 1);
        QVERIFY(!f.comp().node(t)->params.values.value(nodekeys::CenterX).isAnimated());
        // Backward from the last frame, stopped at once: the partial track is still stored.
        f.session()->seekCompFrame(f.session()->compFrameCount() - 1);
        QSignalSpy done2(panel->runner(), &tracking::TrackerRunner::finished);
        QVERIFY(panel->track(false));
        panel->stop();
        QCOMPARE(done2.count(), 1);
        QCOMPARE(
            done2.first().first().value<tracking::TrackOutcome>(), tracking::TrackOutcome::Stopped);
        QVERIFY(!panel->isRunning());
    }

    void trackerPanelNeedsAPicture()
    {
        Fixture f(true, true);
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        const quint64 t = f.session()->addNode(nodetypes::Tracker); // nothing connected
        f.session()->disconnectInput(t, nodeports::Input);
        f.session()->setSelection({t});
        TrackerPanel* panel = f.page()->inspector()->trackerPanel();
        QVERIFY(!panel->track(true));
        QVERIFY(panel->statusText().contains(QStringLiteral("Connect")));
        QVERIFY(!panel->isRunning());
    }

    void planarViewerCornersAreDragged()
    {
        // Spec 50.8: a Planar Tracker shows its quad with four corner handles; dragging a corner
        // edits that corner's keys, dragging inside the quad moves the whole region; one undo
        // step per drag.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.session()->setSelection({f.nodeNamed(QStringLiteral("MediaIn1"))});
        const quint64 t = f.session()->addNode(nodetypes::PlanarTracker);
        QVERIFY(t);
        f.session()->setSelection({t});
        CompViewerOverlay* overlay = f.page()->viewer(1)->controls();
        QVERIFY(overlay->hasControls());
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
        const int64_t frame = f.session()->compFrame();
        auto value = [&](int corner, bool y) {
            return f.comp().node(t)->params.value(nodekeys::planarCorner(corner, y), frame, -1.0);
        };
        int index = -1;
        const QPointF topLeft = *overlay->handlePos(CompViewerOverlay::Handle::Point, 0);
        QCOMPARE(overlay->handleAt(topLeft, &index), CompViewerOverlay::Handle::Point);
        QCOMPARE(index, 0);
        const double x0 = value(0, false), y0 = value(0, true), x2 = value(2, false);
        int before = f.steps();
        drag(topLeft, topLeft + QPointF(-20, -12));
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(value(0, false) < x0 - 0.005); // moved left
        QVERIFY(value(0, true) > y0 + 0.005); // and up (y is bottom-up)
        QCOMPARE(value(2, false), x2); // the other corners stay
        // Inside the quad: the whole region moves.
        const QPointF centre = *overlay->handlePos(CompViewerOverlay::Handle::Centre);
        QCOMPARE(overlay->handleAt(centre), CompViewerOverlay::Handle::Centre);
        before = f.steps();
        drag(centre, centre + QPointF(30, 0));
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(value(2, false) > x2 + 0.01);
        QVERIFY(value(1, false) > 0.75 + 0.01);
    }

    void planarPanelTracksCornersIntoOneUndoStep()
    {
        // Spec 50.9: the Planar Tracker has the same Track buttons as the Tracker; the corners of
        // the region are stored as keys in one undo step. The picture is static noise, so the
        // corners stay where they were put.
        Fixture f(true, true);
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.session()->setSelection({f.nodeNamed(QStringLiteral("MediaIn1"))});
        const quint64 noise = f.session()->addNodeAt(nodetypes::FastNoise, QPointF(0, 5));
        QVERIFY(noise);
        f.session()->edit(QStringLiteral("Noise"), [noise](Composition& c) {
            c.node(noise)->params.setBase(nodekeys::Scale, 60.0);
            c.node(noise)->params.setBase(nodekeys::Detail, 3.0);
            return true;
        });
        const quint64 t = f.session()->addNode(nodetypes::PlanarTracker);
        QVERIFY(t);
        f.session()->connectNodes(noise, t, nodeports::Input);
        f.session()->setSelection({t});
        TrackerPanel* panel = f.page()->inspector()->trackerPanel();
        QVERIFY(panel && panel->isVisibleTo(f.page()->inspector()));
        QVERIFY(panel->forwardButton()->isEnabled());
        const int before = f.steps();
        const int64_t start = f.session()->compFrame();
        QSignalSpy done(panel->runner(), &tracking::TrackerRunner::finished);
        panel->forwardButton()->click();
        QVERIFY(panel->isRunning());
        QVERIFY(done.wait(120000));
        QVERIFY2(done.first().first().value<tracking::TrackOutcome>()
                == tracking::TrackOutcome::Finished,
            qPrintable(panel->statusText()));
        QCOMPARE(f.steps(), before + 1);
        const AnimatedValue& x
            = f.comp().node(t)->params.values.value(nodekeys::planarCorner(1, false));
        QCOMPARE(x.keys.size(), int(f.session()->compFrameCount() - start));
        QVERIFY(std::abs(x.valueAt(f.session()->compFrameCount() - 1) - 0.75) < 0.01);
        f.project.undoStack()->undo();
        QVERIFY(
            !f.comp().node(t)->params.values.value(nodekeys::planarCorner(1, false)).isAnimated());
        f.project.undoStack()->redo();
        panel->findChild<QToolButton*>(QStringLiteral("TrackClear"))->click();
        QVERIFY(
            !f.comp().node(t)->params.values.value(nodekeys::planarCorner(1, false)).isAnimated());
    }

    void followTrackConnectsAnotherNodeInOneUndoStep()
    {
        // Spec 50.10: a node with a Center has a "Follow Track" row; Connect copies the chosen
        // track into its Center keys in one undo step, keeping its place at the playhead.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.session()->setSelection({f.nodeNamed(QStringLiteral("MediaIn1"))});
        const quint64 tracker = f.session()->addNode(nodetypes::Tracker);
        const quint64 xf = f.session()->addNode(nodetypes::Transform);
        QVERIFY(tracker && xf);
        f.session()->setSelection({xf});
        CompInspector* inspector = f.page()->inspector();
        QVERIFY(inspector->followPicker() && inspector->followButton());
        QVERIFY(!inspector->followButton()->isEnabled()); // no track anywhere yet
        f.session()->edit(QStringLiteral("Track"), [tracker](Composition& c) {
            std::vector<tracking::TrackSample> samples;
            for (int i = 0; i <= 6; ++i)
                samples.push_back({i, 0.3 + 0.05 * i, 0.6, 1.0});
            return tracking::storeTrack(*c.node(tracker), samples, 0);
        });
        QVERIFY(inspector->followButton()->isEnabled());
        QCOMPARE(inspector->followPicker()->currentData().toULongLong(), tracker);
        f.session()->seekCompFrame(3);
        QCOMPARE(f.session()->compFrame(), int64_t(3));
        const int before = f.steps();
        inspector->followButton()->click();
        QCOMPARE(f.steps(), before + 1);
        const ParamSet& p = f.comp().node(xf)->params;
        QCOMPARE(p.values.value(nodekeys::CenterX).keys.size(), 7);
        const int64_t frame = f.session()->compFrame();
        QVERIFY(std::abs(p.value(nodekeys::CenterX, frame, -1) - 0.5) < 1e-9);
        QVERIFY(std::abs(p.value(nodekeys::CenterX, 6, -1) - (0.5 + 0.05 * (6 - frame))) < 1e-9);
        f.project.undoStack()->undo();
        QVERIFY(!f.comp().node(xf)->params.values.value(nodekeys::CenterX).isAnimated());
        // The trackers themselves have no such row.
        f.session()->setSelection({tracker});
        QVERIFY(!inspector->followPicker());
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
        for (const char* id : {"edit.ripple_delete", "edit.split", "clip.toggle_enabled",
                 "multicam.angle_1", "mark.in", "trim.nudge_left", "timeline.target_video_1"})
            QVERIFY2(!f.action(id)->isEnabled(), id);
        for (const char* id : {"transport.play_pause", "transport.next_frame", "view.page_edit",
                 "clip.reset_composition", "mark.go_to_in", "comp.delete"})
            QVERIFY2(f.action(id)->isEnabled(), id);
        // Delete with the focus outside the node editor leaves the hidden timeline alone. Edit's
        // Cut, Copy, Paste, Delete and Select All are on only while the node editor has focus
        // (they act on the nodes then, spec 82.8).
        f.window->activateWindow();
        f.page()->transport()->setFocus();
        for (const char* id : {"edit.delete", "edit.copy", "edit.paste", "edit.cut", "edit.select_all"})
            QTRY_VERIFY2(!f.action(id)->isEnabled(), id);
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
        // Spec 42b.32: Edit > Compositing Page is greyed out away from the page.
        for (const char* id : {"comp.delete", "comp.copy", "comp.select_node", "comp.fit"})
            QVERIFY2(!f.action(id)->isEnabled(), id);
        f.selection.setClips({f.clipId});
        f.action("edit.ripple_delete")->trigger();
        QVERIFY(!f.clip());
        // The list never touches what must stay.
        const QStringList ids = CompositingPage::suspendedCommandIds(f.actions);
        for (const char* id : {"edit.undo", "edit.redo", "transport.play_pause",
                 "view.page_compositing", "comp.copy", "clip.reset_composition"})
            QVERIFY2(!ids.contains(QLatin1String(id)), id);
    }

    void selectNodePopupStaysInsideTheWindow()
    {
        // Spec 42b.27: the Shift+Space list opens at the mouse but, like a menu, never runs
        // past the window or screen edge.
        const QSize size(280, 340);
        const QRect bounds(100, 50, 1000, 700);
        // Room around the point: centred on it, search field under it (unchanged).
        const QPoint mid = SelectNodePopup::placement({500, 300}, size, bounds);
        QCOMPARE(mid.x(), 500 - 140);
        QVERIFY(mid.y() < 300 && mid.y() > 300 - 40);
        // Near the bottom-right corner: pushed back in.
        const QRect corner(SelectNodePopup::placement({1095, 745}, size, bounds), size);
        QVERIFY(bounds.contains(corner));
        QCOMPARE(corner.right(), bounds.right());
        QCOMPARE(corner.bottom(), bounds.bottom());
        // Near the top-left corner.
        const QRect topLeft(SelectNodePopup::placement({102, 52}, size, bounds), size);
        QVERIFY(bounds.contains(topLeft));
        // Larger than the bounds: the left / top edge wins.
        QCOMPARE(SelectNodePopup::placement({500, 300}, {2000, 2000}, bounds), bounds.topLeft());

        // On the page: opened at the window's bottom-right corner it stays in the window.
        Fixture f;
        f.showPage();
        QCoreApplication::processEvents();
        SelectNodePopup* popup = f.page()->selectNodePopup();
        const QRect window(f.window->mapToGlobal(QPoint(0, 0)), f.window->size());
        const QPoint at = window.bottomRight() - QPoint(3, 3);
        popup->popup(at);
        QVERIFY(popup->isVisible());
        const QRect onPage = popup->popupBounds(at);
        QVERIFY(window.contains(onPage));
        const QRect expected(SelectNodePopup::placement(at, popup->size(), onPage), popup->size());
        QVERIFY(onPage.contains(expected));
        QCOMPARE(expected.right(), onPage.right());
        QCOMPARE(expected.bottom(), onPage.bottom());
        // Where it went (the offscreen platform adds its 2 px frame margin to the position).
        QVERIFY2((popup->pos() - expected.topLeft()).manhattanLength() <= 4,
            qPrintable(QStringLiteral("%1,%2").arg(popup->x()).arg(popup->y())));
        popup->hide();
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

    // ---- Spline and Keyframes panels (spec 43) ----
    void spec43_splineAndKeyframesButtons()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        for (const char* id : {"view.toggle_spline", "view.toggle_keyframes"})
            QVERIFY2(f.action(id)->isEnabled(), id);
        QVERIFY(!f.page()->isPanelVisible(PanelId::Spline));
        QVERIFY(!f.page()->isPanelVisible(PanelId::Keyframes));
        f.action("view.toggle_spline")->trigger();
        QVERIFY(f.page()->isPanelVisible(PanelId::Spline));
        QVERIFY(f.page()->splinePanel()->isVisible());
        QVERIFY(f.page()->splinePanel()->isCompositionPanel());
        QCOMPARE(f.page()->splinePanel()->view(), KeyframesPanel::View::Curves);
        f.action("view.toggle_keyframes")->trigger();
        QVERIFY(f.page()->keyframesPanel()->isVisible());
        QCOMPARE(f.page()->keyframesPanel()->view(), KeyframesPanel::View::Keyframes);
        // Both panels and the node editor share the node row.
        QVERIFY(f.page()->nodeEditor()->isVisible());
        f.action("view.toggle_spline")->trigger();
        QVERIFY(!f.page()->splinePanel()->isVisible());
    }

    void spec43_splinePanelListsSelectedNodesAnimatedParams()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.action("view.toggle_spline")->trigger();
        KeyframesPanel* panel = f.page()->splinePanel();
        // Nothing animated, nothing selected: no parameter rows.
        QCOMPARE(panel->model().entries.size(), 1);
        const quint64 t = animatedTransform(f);
        // Master row + one group (the node) + its two animated parameters.
        QCOMPARE(panel->model().entries.size(), 4);
        QCOMPARE(panel->model().entries[1].label, f.comp().node(t)->name);
        QStringList labels;
        for (const KeyframeEntry& e : panel->model().entries)
            if (e.kind == KeyRow::Kind::Param)
                labels << e.label;
        QCOMPARE(labels.size(), 2);
        QVERIFY(labels.contains(QStringLiteral("Angle")));
        QCOMPARE(panel->curveView()->curves().size(), 2);
        // Another node selected: its (empty) list replaces it; Sel off lists every node.
        f.session()->setSelection({f.nodeOfType(nodetypes::MediaOut)});
        QCOMPARE(panel->model().entries.size(), 1);
        panel->selectedNodesButton()->click();
        QCOMPARE(panel->model().entries.size(), 4);
        panel->selectedNodesButton()->click();
        // Show All lists the node's other animatable parameters too.
        f.session()->setSelection({t});
        QToolButton* showAll = panel->findChild<QToolButton*>(QStringLiteral("Show All"));
        QVERIFY(showAll);
        showAll->click();
        QVERIFY(panel->model().entries.size() > 4);
        showAll->click();
        // The Keyframes panel lists the same rows in the dope sheet.
        f.action("view.toggle_keyframes")->trigger();
        QCOMPARE(f.page()->keyframesPanel()->model().entries.size(), 4);
    }

    void spec43_loopOnTheSplinePanelChangesEvaluation()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.action("view.toggle_spline")->trigger();
        KeyframesPanel* panel = f.page()->splinePanel();
        const quint64 t = animatedTransform(f);
        const QString path = keyframeops::nodePath(t, nodekeys::CenterX);
        auto x = [&](int64_t frame) {
            return f.comp().node(t)->params.values.value(nodekeys::CenterX).valueAt(frame);
        };
        QCOMPARE(x(50), 0.4); // held after the last key
        const int before = f.steps();
        // Loop with the panel's Loop button: no key selected acts on the shown curves.
        panel->loopButton()->click();
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(
            f.comp().node(t)->params.values.value(nodekeys::CenterX).post, Extrapolation::Loop);
        QVERIFY(qAbs(x(50) - 0.2 - (0.8 - 0.2) * 0.5) < 1e-9); // frame 50 = frame 10 of the range
        // Angle looped too (every shown curve), and undo removes the loop again.
        QCOMPARE(f.comp().node(t)->params.values.value(nodekeys::Angle).post, Extrapolation::Loop);
        f.project.undoStack()->undo();
        QCOMPARE(x(50), 0.4);
        f.project.undoStack()->redo();
        QVERIFY(qAbs(x(50) - 0.5) < 1e-9);
        // Relative on the selected key's parameter only, through the registry command.
        f.selection.setKeyframes({KeyframeRef {f.clipId, path, 20}});
        f.action("clip.keyframe_relative")->trigger();
        QCOMPARE(
            f.comp().node(t)->params.values.value(nodekeys::CenterX).post, Extrapolation::Relative);
        QCOMPARE(f.comp().node(t)->params.values.value(nodekeys::Angle).post, Extrapolation::Loop);
        QVERIFY(qAbs(x(60) - (0.8 + 0.2)) < 1e-9); // one repeat later, 0.2 higher (0.4 - 0.2)
        // Reverse and Time Stretch are one undo step each and keep the keys selected.
        f.selection.setKeyframes({KeyframeRef {f.clipId, path, 0}, KeyframeRef {f.clipId, path, 20},
            KeyframeRef {f.clipId, path, 40}});
        const int steps = f.steps();
        QVERIFY(keyframecommands::reverseSelected(f.ctx));
        QCOMPARE(f.steps(), steps + 1);
        QCOMPARE(x(0), 0.4);
        QCOMPARE(x(40), 0.2);
        QVERIFY(keyframecommands::stretchSelected(f.ctx, 0.0, 2.0));
        QCOMPARE(f.steps(), steps + 2);
        QCOMPARE(f.comp().node(t)->params.values.value(nodekeys::CenterX).keys.back().frame, 80);
        f.project.undoStack()->undo();
        f.project.undoStack()->undo();
        QCOMPARE(x(0), 0.2);
        QCOMPARE(f.comp().node(t)->params.values.value(nodekeys::CenterX).keys.back().frame, 40);
    }

    void spec43_splineTimeAndValueBoxesEditTheSelectedKey()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.action("view.toggle_spline")->trigger();
        KeyframesPanel* panel = f.page()->splinePanel();
        const quint64 t = animatedTransform(f);
        const QString path = keyframeops::nodePath(t, nodekeys::CenterX);
        QVERIFY(!panel->timeBox()->isEnabled());
        f.selection.setKeyframes({KeyframeRef {f.clipId, path, 20}});
        QVERIFY(panel->timeBox()->isEnabled());
        QCOMPARE(panel->timeBox()->value(), 20.0);
        QCOMPARE(panel->valueBox()->value(), 0.8);
        panel->valueBox()->setValue(0.9);
        QCOMPARE(f.comp().node(t)->params.values.value(nodekeys::CenterX).keys[1].value, 0.9);
        panel->timeBox()->setValue(25);
        QCOMPARE(f.comp().node(t)->params.values.value(nodekeys::CenterX).keys[1].frame, 25);
    }

    void spec43_panelsFitA1366Window()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.window->resize(1366, 768);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        f.action("view.page_compositing")->trigger();
        f.action("view.toggle_spline")->trigger();
        f.action("view.toggle_keyframes")->trigger();
        QCoreApplication::processEvents();
        const QRect win = f.window->rect();
        for (QWidget* w : {static_cast<QWidget*>(f.page()->splinePanel()),
                 static_cast<QWidget*>(f.page()->keyframesPanel())}) {
            const QRect r(w->mapTo(f.window.get(), QPoint(0, 0)), w->size());
            QVERIFY2(win.contains(r), qPrintable(w->objectName()));
            QVERIFY(w->width() >= 150 && w->height() >= 150);
        }
    }

    void screenshotSplinePanel()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.window->resize(1366, 768);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        f.action("view.page_compositing")->trigger();
        const quint64 t = animatedTransform(f);
        f.session()->edit(QStringLiteral("Loop"), [t](Composition& c) {
            c.node(t)->params.values[nodekeys::CenterX].post = Extrapolation::PingPong;
            c.node(t)->params.values[nodekeys::CenterX].pre = Extrapolation::Loop;
            c.node(t)->params.values[nodekeys::Angle].post = Extrapolation::Relative;
            return true;
        });
        f.action("view.toggle_spline")->trigger();
        f.action("view.toggle_nodes")->trigger();
        f.action("view.toggle_nodes")->trigger();
        f.page()->splinePanel()->curveView()->fitView();
        QCoreApplication::processEvents();
        const QString dir = qEnvironmentVariable("KK_SCREENSHOT_DIR");
        if (dir.isEmpty())
            QSKIP("KK_SCREENSHOT_DIR not set");
        QDir().mkpath(dir);
        QTest::qWait(200);
        QVERIFY(
            f.window->grab().save(QDir(dir).filePath(QStringLiteral("compositing-spline.png"))));
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
