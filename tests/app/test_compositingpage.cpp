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
#include "core/CompGroups.h"
#include "core/CompLinks.h"
#include "core/CompOps.h"
#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/MediaPool.h"
#include "core/MacroFiles.h"
#include "core/Modifiers.h"
#include "core/NodeRegistry.h"
#include "core/Project.h"
#include "core/Serialization.h"
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
#include "ui/comp/CompViewOptions.h"
#include "ui/comp/CompViewer.h"
#include "ui/comp/CompViewerOverlay.h"
#include "ui/comp/MakeMacroDialog.h"
#include "ui/comp/NodeEditor.h"
#include "ui/comp/NodeLibrary.h"
#include "ui/comp/ViewerCache.h"
#include "ui/comp/ViewerFx.h"
#include "ui/comp/ViewerRegion.h"
#include "ui/comp/TrackerPanel.h"
#include "ui/inspector/ParamGroupBox.h"
#include "ui/inspector/ParamRows.h"
#include "ui/inspector/ValueField.h"
#include "ui/effects/EffectActions.h"
#include "ui/effects/EffectsLibraryPanel.h"
#include "ui/keyframes/CurveView.h"
#include "ui/keyframes/KeyframeCommands.h"
#include "ui/keyframes/KeyframeItemModel.h"
#include "ui/keyframes/KeyframeParamTree.h"
#include "ui/keyframes/KeyframesPanel.h"
#include "ui/mediapool/MediaPoolPanel.h"
#include "ui/viewer/ViewerOverlaySettings.h"
#include "ui/viewer/ViewerSurface.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QDoubleSpinBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QSplitter>
#include <QStandardPaths>
#include <QTest>
#include <QToolBar>
#include <QTreeWidget>
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
        QCOMPARE(compadapter::toolbarTypes().size(), 14);
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

    void colourNodesHaveTooltipsAndFormulaEditsAreUndoSteps()
    {
        // Spec 124: the new colour nodes show every control with a tooltip; a formula typed
        // into the Custom Tool is one undo step; the curve nodes show curve editors.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.session()->setSelection({f.nodeNamed(QStringLiteral("MediaIn1"))});
        for (const QString& type : {nodetypes::BrightnessContrast, nodetypes::ChannelMapper,
                 nodetypes::ColorCurves, nodetypes::HueCurves, nodetypes::CustomTool,
                 nodetypes::VectorMotionBlur}) {
            const quint64 id = f.session()->addNode(type);
            QVERIFY(id);
            f.session()->setSelection({id});
            QCOMPARE(f.page()->inspector()->shownNode(), id);
            ParamGroupBox* group = f.page()->inspector()->group();
            QVERIFY(group && !group->rows().isEmpty());
            for (ParamRow* row : group->rows()) {
                QVERIFY2(!row->descriptor().tooltip.isEmpty(), qPrintable(type + row->descriptor().key));
                bool shown = false;
                for (QWidget* w : row->vimWidgets())
                    shown = shown || w->toolTip().contains(row->descriptor().tooltip);
                QVERIFY2(shown, qPrintable(type + row->descriptor().key));
            }
            if (type == nodetypes::CustomTool) {
                QLineEdit* red = group->findChild<QLineEdit*>(QStringLiteral("text:") + nodekeys::CustomRed);
                QVERIFY(red);
                QCOMPARE(red->text(), QStringLiteral("r1"));
                const int before = f.steps();
                red->setText(QStringLiteral("1 - r1"));
                emit red->editingFinished();
                QCOMPARE(f.steps(), before + 1);
                QCOMPARE(f.comp().node(id)->params.staticValue(nodekeys::CustomRed).toString(),
                    QStringLiteral("1 - r1"));
                f.project.undo();
                QVERIFY(f.comp().node(id)->params.staticValue(nodekeys::CustomRed).toString()
                    != QStringLiteral("1 - r1"));
            }
            if (type == nodetypes::ColorCurves || type == nodetypes::HueCurves) {
                int curves = 0;
                for (ParamRow* row : group->rows())
                    curves += row->descriptor().type == ParamType::Curve;
                QCOMPARE(curves, 5);
            }
        }
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

    void gridWarpViewerPointsAreDragged()
    {
        // Spec 114.4: a Grid Warp shows its grid with a handle on every point; dragging a point
        // writes that point's movement in px, one undo step per drag, and the picture follows.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        f.session()->setSelection({f.nodeNamed(QStringLiteral("MediaIn1"))});
        const quint64 t = f.session()->addNode(nodetypes::GridWarp);
        QVERIFY(t);
        f.session()->setSelection({t});
        CompViewerOverlay* overlay = f.page()->viewer(1)->controls();
        QVERIFY(overlay->hasControls());
        auto send
            = [overlay](QEvent::Type type, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs) {
                  QMouseEvent e(type, p, overlay->mapToGlobal(p), b, bs, Qt::NoModifier);
                  QApplication::sendEvent(overlay, &e);
              };
        const int64_t frame = f.session()->compFrame();
        auto value = [&](int row, int col, int axis) {
            return f.comp().node(t)->params.value(nodekeys::gridKey(row, col, axis), frame, -999.0);
        };
        // Point (1, 1) of the default 4 x 4 grid is index 5.
        const QPointF from = *overlay->handlePos(CompViewerOverlay::Handle::Point, 5);
        int index = -1;
        QCOMPARE(overlay->handleAt(from, &index), CompViewerOverlay::Handle::Point);
        QCOMPARE(index, 5);
        const QPointF to = from + QPointF(24, 10);
        const int before = f.steps();
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 4; ++i)
            send(QEvent::MouseMove, from + (to - from) * i / 4.0, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(value(1, 1, 0) > 1.0); // moved right
        QVERIFY(value(1, 1, 1) > 1.0); // and down (the grid's y is down)
        QCOMPARE(value(0, 0, 0), 0.0); // the other points stay
        QCOMPARE(value(2, 2, 1), 0.0);
        // The handle now sits where the mouse left it.
        const QPointF now = *overlay->handlePos(CompViewerOverlay::Handle::Point, 5);
        QVERIFY(std::abs(now.x() - to.x()) < 1.5 && std::abs(now.y() - to.y()) < 1.5);
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

    void modifierMenuAndSections()
    {
        // Spec 119.6 / 119.7 / 119.10: the right-click menu of a number row puts modifiers on a
        // parameter and connects it to another node's; the Inspector shows a section per modifier.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        const quint64 a = f.session()->addNode(nodetypes::Transform);
        const quint64 b = f.session()->addNode(nodetypes::Transform);
        f.session()->setSelection({a});
        CompInspector* host = f.page()->inspector();
        QCOMPARE(host->shownNode(), a);
        QVERIFY(host->modifierGroups().isEmpty());
        const compadapter::NodeTypeInfo* info = compadapter::nodeType(nodetypes::Transform);
        const auto descOf = [&](const QString& key) {
            for (const ParamDescriptor& d : info->params)
                if (d.key == key)
                    return d;
            return ParamDescriptor {};
        };
        const auto refOf = [&](quint64 node, const QString& key) {
            ParamRef r;
            r.clipId = CompInspector::kProxyClipId;
            r.scope = ParamScope::Effect;
            r.effectId = node;
            r.key = key;
            return r;
        };
        // The menu offers Modify With, Connect To and Publish; XY Path only on a point.
        {
            QMenu menu;
            host->fillParamMenu(refOf(a, nodekeys::Size), descOf(nodekeys::Size), menu);
            QVERIFY(!menu.findChildren<QAction*>(QStringLiteral("modify:perturb")).isEmpty());
            QVERIFY(menu.findChildren<QAction*>(QStringLiteral("modify:path")).isEmpty());
            QVERIFY(!menu.findChildren<QAction*>(QStringLiteral("modify:publish")).isEmpty());
        }
        // Perturb on Center X: both halves of the point get one, as two sections with tooltips.
        int before = f.steps();
        {
            QMenu menu;
            host->fillParamMenu(refOf(a, nodekeys::CenterX), descOf(nodekeys::CenterX), menu);
            QVERIFY(!menu.findChildren<QAction*>(QStringLiteral("modify:path")).isEmpty());
            menu.findChildren<QAction*>(QStringLiteral("modify:perturb")).first()->trigger();
        }
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(modifiers::first(f.comp().node(a)->params, nodekeys::CenterX, ModifierKind::Perturb));
        QVERIFY(modifiers::first(f.comp().node(a)->params, nodekeys::CenterY, ModifierKind::Perturb));
        QCOMPARE(host->modifierGroups().size(), 2);
        for (ParamGroupBox* box : host->modifierGroups()) {
            QVERIFY(box->objectName().startsWith(QStringLiteral("CompModifier:")));
            QCOMPARE(box->rows().size(), 3); // Period, Strength, Random Seed
            for (ParamRow* row : box->rows())
                for (QWidget* w : row->vimWidgets())
                    QVERIFY2(!w->toolTip().isEmpty(), qPrintable(row->descriptor().label));
        }
        QVERIFY(host->paramModifierNote(refOf(a, nodekeys::CenterX)).contains(QStringLiteral("Perturb")));
        f.project.undoStack()->undo();
        QVERIFY(modifiers::modifiedKeys(f.comp().node(a)->params).isEmpty());
        QVERIFY(host->modifierGroups().isEmpty());
        f.project.undoStack()->redo();
        QCOMPARE(host->modifierGroups().size(), 2);
        // Remove it through the menu: the section goes.
        {
            QMenu menu;
            host->fillParamMenu(refOf(a, nodekeys::CenterX), descOf(nodekeys::CenterX), menu);
            QVERIFY(!menu.findChildren<QAction*>(QRegularExpression(QStringLiteral("^remove:"))).isEmpty());
            menu.findChildren<QAction*>(QRegularExpression(QStringLiteral("^remove:"))).first()->trigger();
        }
        QVERIFY(!modifiers::first(f.comp().node(a)->params, nodekeys::CenterX, ModifierKind::Perturb));
        QCOMPARE(host->modifierGroups().size(), 1);
        // Connect To: Node b's Size follows node a's Size, found by name in the menu.
        f.session()->setSelection({b});
        QVERIFY(host->shownNode() == b);
        before = f.steps();
        {
            QMenu menu;
            host->fillParamMenu(refOf(b, nodekeys::Size), descOf(nodekeys::Size), menu);
            QMenu* connectMenu = nullptr;
            for (QAction* act : menu.actions())
                if (act->menu() && act->text() == QStringLiteral("Connect To"))
                    connectMenu = act->menu();
            QVERIFY(connectMenu);
            QAction* target = nullptr;
            for (QAction* act : connectMenu->actions())
                if (act->menu() && act->text() == f.comp().node(a)->name)
                    for (QAction* leaf : act->menu()->actions())
                        if (leaf->text() == descOf(nodekeys::Size).label)
                            target = leaf;
            QVERIFY(target);
            target->trigger();
        }
        QCOMPARE(f.steps(), before + 1);
        const ParamModifier* link = modifiers::first(f.comp().node(b)->params, nodekeys::Size, ModifierKind::Connect);
        QVERIFY(link && link->targetNode == a);
        QVERIFY(host->paramModifierNote(refOf(b, nodekeys::Size)).contains(f.comp().node(a)->name));
        QCOMPARE(host->modifierGroups().size(), 1);
        // The linked value shows what the target has.
        f.session()->edit(QStringLiteral("Set"), [a](Composition& c) {
            c.node(a)->params.setBase(nodekeys::Size, 2.5);
            return true;
        });
        QVERIFY(complinks::hasLinks(f.comp()));
        QCOMPARE(complinks::resolved(f.comp(), 20).node(b)->params.value(nodekeys::Size, 20, 0), 2.5);
        // A circle is refused: a cannot connect to b.
        f.session()->setSelection({a});
        before = f.steps();
        {
            QMenu menu;
            host->fillParamMenu(refOf(a, nodekeys::Size), descOf(nodekeys::Size), menu);
            QMenu* connectMenu = nullptr;
            for (QAction* act : menu.actions())
                if (act->menu() && act->text() == QStringLiteral("Connect To"))
                    connectMenu = act->menu();
            for (QAction* act : connectMenu->actions())
                if (act->menu() && act->text() == f.comp().node(b)->name)
                    for (QAction* leaf : act->menu()->actions())
                        if (leaf->text() == descOf(nodekeys::Size).label)
                            leaf->trigger();
        }
        QCOMPARE(f.steps(), before);
        // Disconnect keeps the value.
        f.session()->setSelection({b});
        {
            QMenu menu;
            host->fillParamMenu(refOf(b, nodekeys::Size), descOf(nodekeys::Size), menu);
            menu.findChildren<QAction*>(QRegularExpression(QStringLiteral("^remove:"))).first()->trigger();
        }
        QVERIFY(f.comp().node(b)->params.values[nodekeys::Size].modifiers.isEmpty());
        QCOMPARE(f.comp().node(b)->params.value(nodekeys::Size, 20, 0), 2.5);
        // Publish and the Nodes library entry (the parameter touched last).
        before = f.steps();
        {
            QMenu menu;
            host->fillParamMenu(refOf(b, nodekeys::Size), descOf(nodekeys::Size), menu);
            menu.findChildren<QAction*>(QStringLiteral("modify:publish")).first()->trigger();
        }
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(complinks::published(f.comp()).size(), 1);
        QVERIFY(f.session()->addModifier(0, QString(), ModifierKind::Wiggle));
        QVERIFY(modifiers::first(f.comp().node(b)->params, nodekeys::Size, ModifierKind::Wiggle));
    }

    void expressionsTypedIntoValueFields()
    {
        // Spec 119.10: "= formula" typed into a number makes an Expression modifier.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        const quint64 a = f.session()->addNode(nodetypes::Transform);
        f.session()->setSelection({a});
        CompInspector* host = f.page()->inspector();
        auto* field = host->group()->findChild<ValueField*>(QStringLiteral("value:") + nodekeys::Size);
        QVERIFY(field);
        const int before = f.steps();
        field->startTextEdit();
        auto* editor = field->findChild<QLineEdit*>();
        QVERIFY(editor);
        editor->setText(QStringLiteral("= 1 + time / 10"));
        emit editor->editingFinished();
        QCOMPARE(f.steps(), before + 1);
        const ParamModifier* m = modifiers::first(f.comp().node(a)->params, nodekeys::Size, ModifierKind::Expression);
        QVERIFY(m);
        QCOMPARE(m->text, QStringLiteral("1 + time / 10"));
        // The field shows the value at the playhead (frame 10 of the clip: 1 + 10 / 10).
        QCOMPARE(f.comp().node(a)->params.value(nodekeys::Size, 10, 0), 2.0);
        // The Inspector was rebuilt for the new section: the field is a new widget.
        field = host->group()->findChild<ValueField*>(QStringLiteral("value:") + nodekeys::Size);
        QVERIFY(field && field->toolTip().contains(QStringLiteral("Expression")));
        QCOMPARE(host->modifierGroups().size(), 1);
        auto* formula = host->modifierGroups().first()->findChild<QLineEdit*>(
            QStringLiteral("CompModifierFormula:") + m->id);
        QVERIFY(formula && !formula->toolTip().isEmpty());
        QCOMPARE(formula->text(), QStringLiteral("1 + time / 10"));
        // A formula with a mistake is refused (no step), and one edit of the formula line is a step.
        host->setParamExpression(
            [&] { ParamRef r; r.key = nodekeys::Size; return r; }(), ParamDescriptor {}, QStringLiteral("1 +"));
        QCOMPARE(f.steps(), before + 1);
        formula->setText(QStringLiteral("n1 * 3"));
        emit formula->editingFinished();
        QCOMPARE(f.steps(), before + 2);
        QCOMPARE(modifiers::first(f.comp().node(a)->params, nodekeys::Size, ModifierKind::Expression)->text,
            QStringLiteral("n1 * 3"));
        // A formula that reads a parameter that reads this one: refused.
        const quint64 b = f.session()->addNode(nodetypes::Transform);
        const QString an = f.comp().node(a)->name;
        f.session()->edit(QStringLiteral("Link"), [&](Composition& c) {
            const QString id = modifiers::add(c.node(b)->params, nodekeys::Size, ModifierKind::Expression);
            modifiers::find(c.node(b)->params, nodekeys::Size, id)->text = an + QStringLiteral(".Size");
            return true;
        });
        f.session()->setSelection({a});
        const int steps = f.steps();
        host->setParamExpression(
            [&] { ParamRef r; r.key = nodekeys::Size; return r; }(), ParamDescriptor {},
            f.comp().node(b)->name + QStringLiteral(".Size"));
        QCOMPARE(f.steps(), steps);
        // An empty formula removes the modifier.
        host->setParamExpression(
            [&] { ParamRef r; r.key = nodekeys::Size; return r; }(), ParamDescriptor {}, QString());
        QVERIFY(!modifiers::first(f.comp().node(a)->params, nodekeys::Size, ModifierKind::Expression));
    }

    void groupsCollapseMoveAndUngroup()
    {
        // Spec 120.1 / 120.2: Ctrl+G groups the selected nodes; a collapsed group is one tile (its
        // members are hidden, links end on it); a double-click opens and closes it; the group moves
        // with everything in it; Ctrl+Shift+G ungroups.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        const quint64 a = f.session()->addNode(nodetypes::Transform);
        const quint64 b = f.session()->addNode(nodetypes::Transform);
        const QPointF posA = f.comp().node(a)->position, posB = f.comp().node(b)->position;
        f.session()->setSelection({a, b}, {}, b);
        int before = f.steps();
        f.key(Qt::Key_G, Qt::ControlModifier);
        QCOMPARE(f.steps(), before + 1);
        const quint64 g = f.nodeOfType(nodetypes::Group);
        QVERIFY(g);
        QCOMPARE(f.comp().node(a)->group, g);
        QCOMPARE(f.comp().node(b)->group, g);
        QCOMPARE(f.session()->currentNode(), g);
        // Expanded: the group is a frame with a title bar over its members, which stay clickable.
        NodeEditor* ed = f.editor();
        const CompNode& gn = *f.comp().node(g);
        QVERIFY(ed->groupFrame(gn).contains(ed->tileRect(*f.comp().node(a))));
        QVERIFY(ed->groupFrame(gn).contains(ed->tileRect(*f.comp().node(b))));
        QCOMPARE(ed->hitTest(ed->tileRect(*f.comp().node(a)).center()).node, a);
        QCOMPARE(ed->hitTest(ed->tileRect(gn).center()).node, g);
        QVERIFY(!ed->isHidden(*f.comp().node(a)));
        // Collapse with the context menu entry, one undo step.
        before = f.steps();
        f.session()->setSelection({g}, {}, g);
        {
            std::unique_ptr<QMenu> menu(ed->buildContextMenu(ed->tileRect(gn).center(), ed));
            auto* act = menu->findChild<QAction*>(QStringLiteral("CollapseGroupAction"));
            QVERIFY(act && act->text() == QStringLiteral("Collapse Group") && !act->toolTip().isEmpty());
            QVERIFY(menu->findChild<QAction*>(QStringLiteral("UngroupAction")));
            QVERIFY(menu->findChild<QAction*>(QStringLiteral("MakeMacroAction")));
            QVERIFY(menu->findChild<QAction*>(QStringLiteral("SaveMacroAction")));
            act->trigger();
        }
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(f.comp().node(g)->collapsed);
        QVERIFY(ed->isHidden(*f.comp().node(a)) && ed->isHidden(*f.comp().node(b)));
        QCOMPARE(ed->tileRect(*f.comp().node(a)), ed->tileRect(*f.comp().node(g)));
        // The hidden members cannot be hit; the group tile can; a box select skips them.
        QCOMPARE(ed->hitTest(ed->tileRect(*f.comp().node(g)).center()).node, g);
        // Double-click the tile: expands. Again: collapses.
        const QPoint tile = ed->tileRect(*f.comp().node(g)).center().toPoint();
        QTest::mouseDClick(ed, Qt::LeftButton, Qt::NoModifier, tile);
        QVERIFY(!f.comp().node(g)->collapsed);
        QTest::mouseDClick(ed, Qt::LeftButton, Qt::NoModifier, ed->tileRect(*f.comp().node(g)).center().toPoint());
        QVERIFY(f.comp().node(g)->collapsed);
        f.project.undoStack()->undo();
        QVERIFY(!f.comp().node(g)->collapsed);
        // Moving the group moves the members (live too: tileRect follows the drag).
        f.session()->setSelection({g}, {}, g);
        QVERIFY(f.session()->moveSelection(QPointF(3, 2)));
        QCOMPARE(f.comp().node(a)->position, posA + QPointF(3, 2));
        QCOMPARE(f.comp().node(b)->position, posB + QPointF(3, 2));
        f.project.undoStack()->undo();
        QCOMPARE(f.comp().node(a)->position, posA);
        // Ctrl+Shift+G ungroups: the group node goes, the members stay and are selected (the step
        // count is the stack's index here: an undone step was replaced).
        before = f.project.undoStack()->index();
        f.session()->setSelection({g}, {}, g);
        f.key(Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(f.project.undoStack()->index(), before + 1);
        QVERIFY(!f.comp().node(g));
        QCOMPARE(f.comp().node(a)->group, quint64(0));
        QCOMPARE(f.session()->selectedNodes().size(), 2);
        // Deleting a group frees its members as well.
        f.session()->setSelection({a, b}, {}, b);
        const quint64 g2 = f.session()->groupSelection();
        QVERIFY(g2);
        f.session()->setSelection({g2}, {}, g2);
        QVERIFY(f.session()->deleteSelection());
        QVERIFY(f.comp().node(a) && f.comp().node(b));
        QCOMPARE(f.comp().node(a)->group, quint64(0));
    }

    void macroDialogInspectorAndTitle()
    {
        // Spec 120.3 / 120.4 / 120.5: Make Macro asks which parameters become controls; the macro's
        // Inspector shows them and they drive the inner parameters; the macro is saved as a title in
        // the Effects Library and adds a clip with its composition.
        QStandardPaths::setTestModeEnabled(true);
        QDir(macrofiles::directory()).removeRecursively();
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        const quint64 a = f.session()->addNode(nodetypes::Background);
        const quint64 b = f.session()->addNode(nodetypes::Transform);
        f.session()->setSelection({a, b}, {}, b);
        const quint64 g = f.session()->groupSelection();
        QVERIFY(g);
        CompInspector* inspector = f.page()->inspector();
        QCOMPARE(inspector->shownNode(), g);
        QVERIFY(inspector->groupSection());
        auto* members = inspector->groupSection()->findChild<QLabel*>(QStringLiteral("CompGroupMembers"));
        QVERIFY(members && members->text().contains(QLatin1Char('2')) && !members->toolTip().isEmpty());
        // The button opens the dialog (a child of the page); tick Size and press Ok.
        auto* make = inspector->groupSection()->findChild<QPushButton*>(QStringLiteral("CompMakeMacro"));
        QVERIFY(make && !make->toolTip().isEmpty());
        make->click();
        auto* dialog = f.page()->findChild<MakeMacroDialog*>(QStringLiteral("MakeMacroDialog"));
        QVERIFY(dialog);
        QCOMPARE(dialog->nameField()->text(), f.comp().node(g)->name);
        QVERIFY(!dialog->nameField()->toolTip().isEmpty() && !dialog->tree()->toolTip().isEmpty());
        QVERIFY(dialog->picks().isEmpty());
        QVERIFY(dialog->setTicked(b, nodekeys::Size, true));
        QVERIFY(!dialog->setTicked(b, QStringLiteral("nothing"), true));
        dialog->nameField()->setText(QStringLiteral("Zoomer"));
        const int before = f.steps();
        dialog->accept();
        QCOMPARE(f.steps(), before + 1);
        const CompNode& m = *f.comp().node(g);
        QVERIFY(m.macro);
        QCOMPARE(m.name, QStringLiteral("Zoomer"));
        QCOMPARE(m.controls.size(), 1);
        // The Inspector shows the control as a row and the Group section has its Remove button.
        QCOMPARE(inspector->shownNode(), g);
        QCOMPARE(inspector->group()->rows().size(), 1);
        QCOMPARE(inspector->group()->rows().first()->ref().key, m.controls[0].key);
        QVERIFY(inspector->groupSection()->findChild<QToolButton*>(QStringLiteral("CompRemoveControl:") + m.controls[0].key));
        // Editing the control drives the inner parameter.
        f.session()->edit(QStringLiteral("Set"), [&](Composition& c) {
            c.node(g)->params.setBase(c.node(g)->controls[0].key, 3.0);
            return true;
        });
        QCOMPARE(complinks::resolved(f.comp(), 20).node(b)->params.value(nodekeys::Size, 20, 0), 3.0);
        // Save as a title: the input dialog opens; Ok writes the file.
        auto* save = inspector->groupSection()->findChild<QPushButton*>(QStringLiteral("CompSaveMacro"));
        QVERIFY(save && !save->toolTip().isEmpty());
        save->click();
        auto* ask = f.page()->findChild<QInputDialog*>(QStringLiteral("SaveMacroDialog"));
        QVERIFY(ask);
        QCOMPARE(ask->textValue(), QStringLiteral("Zoomer"));
        ask->setTextValue(QStringLiteral("Zoomer Title"));
        ask->accept();
        QCOMPARE(macrofiles::list().size(), 1);
        QCOMPARE(macrofiles::list().first().name, QStringLiteral("Zoomer Title"));
        // The Effects library lists it under Titles and adds it at the playhead.
        EffectsLibraryPanel library(f.ctx);
        library.refreshMacros();
        const QString id = macrofiles::list().first().id();
        QVERIFY(library.visibleDescriptorIds().contains(id));
        std::unique_ptr<QMenu> menu(library.createContextMenu(id));
        QVERIFY(menu && menu->findChild<QAction*>(QStringLiteral("DeleteMacroEntry")));
        const int clipsBefore = f.project.timeline()->data().videoTracks[0].clips.size()
            + (f.project.timeline()->data().videoTracks.size() > 1 ? f.project.timeline()->data().videoTracks[1].clips.size() : 0);
        QVERIFY(effectactions::activate(f.ctx, id));
        int clipsAfter = 0;
        const ClipData* made = nullptr;
        for (const TrackData& t : f.project.timeline()->data().videoTracks)
            for (const ClipData& c : t.clips) {
                ++clipsAfter;
                if (c.name == QStringLiteral("Zoomer Title"))
                    made = &c;
            }
        QCOMPARE(clipsAfter, clipsBefore + 1);
        QVERIFY(made && made->composition);
        QVERIFY(made->composition->node(made->composition->mediaOutId)->inputs.value(nodeports::Input).isConnected());
        QVERIFY(std::any_of(made->composition->nodes.cbegin(), made->composition->nodes.cend(),
            [](const CompNode& n) { return n.macro && n.controls.size() == 1; }));
        // Remove a control through the Inspector: the inner parameter keeps its value.
        auto* remove = inspector->groupSection()->findChild<QToolButton*>(QStringLiteral("CompRemoveControl:") + f.comp().node(g)->controls[0].key);
        QVERIFY(remove);
        remove->click();
        QVERIFY(!f.comp().node(g)->macro);
        QCOMPARE(f.comp().node(b)->params.value(nodekeys::Size, 20, 0), 3.0);
        // The library entry can be deleted again.
        {
            std::unique_ptr<QMenu> m2(library.createContextMenu(id));
            m2->findChild<QAction*>(QStringLiteral("DeleteMacroEntry"))->trigger();
        }
        QVERIFY(!library.visibleDescriptorIds().contains(id));
        QCOMPARE(macrofiles::list().size(), 0);
    }

    void instancesAndUnderlays()
    {
        // Spec 120.6 / 120.7: an instance shows its original's settings and cannot be edited; an
        // underlay carries the nodes on it.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        const quint64 a = f.session()->addNode(nodetypes::Transform);
        f.session()->edit(QStringLiteral("Set"), [a](Composition& c) {
            c.node(a)->params.setBase(nodekeys::Size, 1.75);
            return true;
        });
        f.session()->setSelection({a}, {}, a);
        {
            std::unique_ptr<QMenu> menu(f.editor()->buildContextMenu(
                f.editor()->tileRect(*f.comp().node(a)).center(), f.editor()));
            auto* act = menu->findChild<QAction*>(QStringLiteral("CreateInstanceAction"));
            QVERIFY(act && !act->toolTip().isEmpty());
            QVERIFY(menu->findChild<QAction*>(QStringLiteral("GroupAction")));
            QVERIFY(menu->findChild<QAction*>(QStringLiteral("UnderlayAction")));
            QVERIFY(!menu->findChild<QAction*>(QStringLiteral("MakeIndependentAction")));
            act->trigger();
        }
        const quint64 inst = f.session()->currentNode();
        QVERIFY(inst != a);
        QCOMPARE(f.comp().node(inst)->instanceOf, a);
        CompInspector* inspector = f.page()->inspector();
        QCOMPARE(inspector->shownNode(), inst);
        QVERIFY(inspector->groupSection());
        QVERIFY(!inspector->group()->isEnabled()); // the settings are the original's
        auto* of = inspector->groupSection()->findChild<QLabel*>(QStringLiteral("CompInstanceOf"));
        QVERIFY(of && of->text().contains(f.comp().node(a)->name));
        QCOMPARE(complinks::resolved(f.comp(), 20).node(inst)->params.value(nodekeys::Size, 20, 0), 1.75);
        // Make Independent: its own copy.
        auto* indep = inspector->groupSection()->findChild<QPushButton*>(QStringLiteral("CompMakeIndependent"));
        QVERIFY(indep && !indep->toolTip().isEmpty());
        indep->click();
        QCOMPARE(f.comp().node(inst)->instanceOf, quint64(0));
        QVERIFY(inspector->group()->isEnabled());
        QVERIFY(!inspector->groupSection());
        QCOMPARE(f.comp().node(inst)->params.value(nodekeys::Size, 20, 0), 1.75);
        // An underlay around the selection: the nodes on it move with it, and only its title strip is
        // grabbed.
        f.session()->setSelection({a, inst}, {}, inst);
        const quint64 u = f.session()->addUnderlay();
        QVERIFY(u);
        const CompNote* note = nullptr;
        for (const CompNote& n : f.comp().notes)
            if (n.id == u)
                note = &n;
        QVERIFY(note && note->underlay);
        QVERIFY(QRectF(note->position, note->size).contains(QRectF(f.comp().node(a)->position, QSizeF(3.4, 1))));
        QVERIFY(QRectF(note->position, note->size).contains(QRectF(f.comp().node(inst)->position, QSizeF(3.4, 1))));
        const QPointF posA = f.comp().node(a)->position;
        const QPointF inside = f.editor()->fromGrid(note->position + QPointF(0.5, 3.5));
        QCOMPARE(f.editor()->hitTest(f.editor()->tileRect(*f.comp().node(a)).center()).node, a);
        const QPointF strip = f.editor()->fromGrid(note->position + QPointF(1.0, 0.4));
        QCOMPARE(f.editor()->hitTest(strip).kind, NodeEditor::Hit::Kind::Note);
        Q_UNUSED(inside);
        f.session()->setSelection({}, {u}, 0);
        QVERIFY(f.session()->moveSelection(QPointF(2, 1)));
        QCOMPARE(f.comp().node(a)->position, posA + QPointF(2, 1));
        // The underlay is saved with the project.
        const Composition back = serialization::compositionFromJson(serialization::compositionToJson(f.comp()));
        QVERIFY(back.notes.last().underlay);
    }

    void viewerOnionSkinAndRegionFunctions()
    {
        // Spec 121.1 / 121.2: the pure picture functions of Onion Skin and Region of Interest.
        QImage cur(40, 20, QImage::Format_ARGB32_Premultiplied);
        cur.fill(Qt::black);
        QImage ghostBefore(40, 20, QImage::Format_ARGB32_Premultiplied), ghostAfter = ghostBefore;
        ghostBefore.fill(Qt::white);
        ghostAfter.fill(Qt::white);
        const QImage one = viewerfx::onionSkin(cur, {ghostBefore}, {});
        const QRgb b = one.pixel(5, 5);
        QVERIFY2(qBlue(b) > qGreen(b) && qGreen(b) > qRed(b) && qRed(b) > 30, "frames before are blue");
        const QImage two = viewerfx::onionSkin(cur, {}, {ghostAfter});
        const QRgb a = two.pixel(5, 5);
        QVERIFY2(qRed(a) > qGreen(a) && qGreen(a) > qBlue(a) && qBlue(a) > 20, "frames after are orange");
        // Further frames are fainter; null pictures are skipped; no ghosts: unchanged.
        const QImage far = viewerfx::onionSkin(cur, {QImage(), ghostBefore}, {});
        QVERIFY(qBlue(far.pixel(5, 5)) < qBlue(b));
        QCOMPARE(viewerfx::onionSkin(cur, {}, {}), cur);
        QCOMPARE(viewerfx::onionSkin(QImage(), {ghostBefore}, {}).isNull(), true);
        // The current picture is never covered by something fully: black stays darker than white.
        QVERIFY(qRed(one.pixel(5, 5)) < 200);
        // Region: outside it the picture is cut away.
        QImage red(100, 50, QImage::Format_ARGB32_Premultiplied);
        red.fill(Qt::red);
        const QImage cut = viewerfx::applyRegion(red, QRectF(0.25, 0.2, 0.5, 0.6));
        QCOMPARE(qAlpha(cut.pixel(50, 25)), 255);
        QCOMPARE(qAlpha(cut.pixel(10, 25)), 0);
        QCOMPARE(qAlpha(cut.pixel(50, 5)), 0);
        QCOMPARE(qAlpha(cut.pixel(90, 25)), 0);
        QCOMPARE(viewerfx::applyRegion(red, QRectF(0, 0, 1, 1)), red);
        QCOMPARE(viewerfx::applyRegion(red, QRectF()), red);
    }


    void viewerDiskCacheStoresFindsAndTrims()
    {
        // Spec 121.6: keys follow what decides the picture; pictures round-trip; the folder is trimmed
        // oldest first.
        QStandardPaths::setTestModeEnabled(true);
        viewercache::clear();
        const QByteArray a = viewercache::makeKey("comp", 3, 10, QSize(320, 180), 3);
        QCOMPARE(a.size(), 40);
        QCOMPARE(viewercache::makeKey("comp", 3, 10, QSize(320, 180), 3), a);
        for (const QByteArray& other : {viewercache::makeKey("comp2", 3, 10, QSize(320, 180), 3),
                 viewercache::makeKey("comp", 4, 10, QSize(320, 180), 3),
                 viewercache::makeKey("comp", 3, 11, QSize(320, 180), 3),
                 viewercache::makeKey("comp", 3, 10, QSize(160, 90), 3),
                 viewercache::makeKey("comp", 3, 10, QSize(320, 180), 1)})
            QVERIFY(other != a);
        QImage img(64, 36, QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(200, 30, 60, 255));
        QPainter p(&img);
        p.fillRect(QRect(10, 10, 20, 10), QColor(0, 255, 0, 128));
        p.end();
        QVERIFY(!viewercache::contains(a));
        QVERIFY(viewercache::load(a).isNull());
        QVERIFY(viewercache::store(a, img));
        QVERIFY(viewercache::contains(a));
        QCOMPARE(viewercache::load(a), img);
        QCOMPARE(viewercache::usage().files, 1);
        QVERIFY(!viewercache::store(a, QImage()));
        // A damaged file is a miss, not a crash.
        QFile bad(viewercache::directory() + QStringLiteral("/") + QString::fromLatin1(a) + QStringLiteral(".kkv"));
        QVERIFY(bad.open(QIODevice::WriteOnly | QIODevice::Truncate));
        bad.write("not a picture");
        bad.close();
        QVERIFY(viewercache::load(a).isNull());
        // Trimming: five files, the least recently used go first.
        viewercache::clear();
        QVector<QByteArray> keys;
        for (int i = 0; i < 5; ++i) {
            keys.append(viewercache::makeKey("t", 1, i, QSize(64, 36), 0));
            QVERIFY(viewercache::store(keys.last(), img));
            QFile f(viewercache::directory() + QStringLiteral("/") + QString::fromLatin1(keys.last()) + QStringLiteral(".kkv"));
            QVERIFY(f.open(QIODevice::ReadWrite));
            f.setFileTime(QDateTime::currentDateTime().addSecs(-1000 + i * 100), QFileDevice::FileModificationTime);
        }
        // Reading the oldest counts as using it.
        QVERIFY(!viewercache::load(keys[0]).isNull());
        const qint64 each = viewercache::usage().bytes / 5;
        QCOMPARE(viewercache::trim(each * 2 + 10), 3);
        QCOMPARE(viewercache::usage().files, 2);
        QVERIFY(viewercache::contains(keys[0])); // used last: stays
        QVERIFY(viewercache::contains(keys[4])); // newest of the rest
        QVERIFY(!viewercache::contains(keys[1]) && !viewercache::contains(keys[2]) && !viewercache::contains(keys[3]));
        QCOMPARE(viewercache::trim(), 0); // within the limit
        QCOMPARE(viewercache::clear(), 2);
        QCOMPARE(viewercache::usage().files, 0);
    }

    void viewerQualityOptionsAndDraftTimeline()
    {
        // Spec 121.5: the switches are stored, proxies are fractions of the viewer, HiQ / MB off make a
        // draft copy of the composition for the viewer only.
        QSettings().clear();
        CompViewOptions& options = CompViewOptions::instance();
        options.load();
        QCOMPARE(options.options(), CompViewOptionsData {});
        QVERIFY(options.options().highQuality && options.options().motionBlur && !options.options().proxy);
        QSignalSpy changed(&options, &CompViewOptions::changed);
        CompViewOptionsData o;
        o.proxy = true;
        o.proxyRatio = 4;
        options.setOptions(o);
        QCOMPARE(changed.count(), 1);
        options.setOptions(o); // no change: no signal
        QCOMPARE(changed.count(), 1);
        QCOMPARE(options.renderSize(QSize(800, 400), false), QSize(200, 100));
        o.proxyRatio = 2;
        options.setOptions(o);
        QCOMPARE(options.renderSize(QSize(800, 400), false), QSize(400, 200));
        QCOMPARE(options.renderSize(QSize(20, 10), false), QSize(16, 10)); // never below 16 px, never above the size
        // Auto Proxy: only while interacting.
        o.proxy = false;
        o.autoProxy = true;
        options.setOptions(o);
        QCOMPARE(options.renderSize(QSize(800, 400), false), QSize(800, 400));
        QCOMPARE(options.renderSize(QSize(800, 400), true), QSize(400, 200));
        QVERIFY(options.proxyInEffect(true) && !options.proxyInEffect(false));
        // Stored: a fresh load sees them.
        options.load();
        QVERIFY(options.options().autoProxy && options.options().proxyRatio == 2);
        // The draft timeline: untouched at full quality, a copy otherwise; the original is never changed.
        TimelineData tl;
        tl.width = 320;
        tl.height = 180;
        ClipData clip;
        clip.id = 7;
        clip.kind = ClipKind::Solid;
        Composition comp = compadapter::defaultComposition();
        const quint64 merge = compops::addNode(comp, nodetypes::Merge, {});
        comp.node(merge)->params.setStatic(nodekeys::MotionBlur, true);
        clip.composition = comp;
        TrackData track;
        track.clips.push_back(clip);
        tl.videoTracks.push_back(track);
        const auto snapshot = std::make_shared<const TimelineData>(tl);
        options.setOptions({});
        QCOMPARE(options.viewerTimeline(snapshot, 7), snapshot);
        o = {};
        o.motionBlur = false;
        options.setOptions(o);
        const auto draft = options.viewerTimeline(snapshot, 7);
        QVERIFY(draft != snapshot);
        QCOMPARE(draft->videoTracks[0].clips[0].composition->node(merge)->params.staticValue(nodekeys::MotionBlur).toBool(), false);
        QCOMPARE(snapshot->videoTracks[0].clips[0].composition->node(merge)->params.staticValue(nodekeys::MotionBlur).toBool(), true);
        QCOMPARE(draft->videoTracks[0].clips[0].composition->node(merge)->params.staticValue(nodekeys::FilterMethod).toString(), snapshot->videoTracks[0].clips[0].composition->node(merge)->params.staticValue(nodekeys::FilterMethod).toString()); // HiQ still on: the filter is the node's own
        o = {};
        o.highQuality = false;
        options.setOptions(o);
        const auto rough = options.viewerTimeline(snapshot, 7);
        QCOMPARE(rough->videoTracks[0].clips[0].composition->node(merge)->params.staticValue(nodekeys::FilterMethod).toString(), filtermethods::Nearest);
        QCOMPARE(options.viewerTimeline(snapshot, 99), snapshot); // another clip: nothing to change
        QVERIFY(options.cacheFlags() != CompViewOptionsData {}.highQuality);
        options.setOptions({});
        QSettings().clear();
    }

    void viewerToolsRegionRulersAndSafeArea()
    {
        // Spec 121.2 / 121.3 / 121.4: Region of Interest, Rulers and Safe Area on the viewer's tool bar.
        QSettings().clear();
        ViewerOverlaySettings::instance().load();
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        CompViewer* v = f.page()->viewer(0);
        f.session()->setViewerNode(0, f.nodeNamed(QStringLiteral("MediaIn1")));
        for (const char* id : {"onion", "onionframes", "roi", "rulers", "safe"}) {
            QAction* a = v->toolAction(QLatin1String(id));
            QVERIFY2(a && !a->toolTip().isEmpty(), id);
        }
        // Rulers and Safe Area are the shared View options.
        auto* ruler = v->findChild<QWidget*>(QStringLiteral("CompViewerRulerTop"));
        QVERIFY(ruler && !ruler->isVisible());
        v->toolAction(QStringLiteral("rulers"))->trigger();
        QVERIFY(v->rulersShown());
        QVERIFY(ViewerOverlaySettings::instance().options().showRulers);
        QVERIFY(ruler->isVisible());
        QVERIFY(v->toolAction(QStringLiteral("rulers"))->isChecked());
        v->toolAction(QStringLiteral("safe"))->trigger();
        QVERIFY(v->safeAreaShown());
        QVERIFY(ViewerOverlaySettings::instance().options().actionSafe && ViewerOverlaySettings::instance().options().titleSafe);
        v->toolAction(QStringLiteral("safe"))->trigger();
        QVERIFY(!v->safeAreaShown());
        v->toolAction(QStringLiteral("rulers"))->trigger();
        QVERIFY(!ruler->isVisible());
        // Onion Skin: the tool, its frame count and the O key.
        QVERIFY(!v->onionShown());
        QTest::keyClick(v, Qt::Key_O);
        QVERIFY(v->onionShown() && v->toolAction(QStringLiteral("onion"))->isChecked());
        v->toolAction(QStringLiteral("onionframes"))->trigger();
        QCOMPARE(v->onionFrames(), 2);
        QVERIFY(v->toolAction(QStringLiteral("onionframes"))->text().contains(QLatin1Char('2')));
        v->toolAction(QStringLiteral("onionframes"))->trigger();
        v->toolAction(QStringLiteral("onionframes"))->trigger();
        QCOMPARE(v->onionFrames(), 1);
        v->toolAction(QStringLiteral("onion"))->trigger();
        QVERIFY(!v->onionShown());
        // Region of Interest: the overlay draws and edits the region.
        ViewerRegion* roi = v->regionOverlay();
        QVERIFY(roi && !roi->isVisible() && !roi->isSet());
        v->toolAction(QStringLiteral("roi"))->trigger();
        QVERIFY(v->regionShown() && roi->isVisible());
        QCOMPARE(roi->region(), QRectF(0, 0, 1, 1));
        roi->setRegion(QRectF(0.25, 0.25, 0.5, 0.5));
        QCOMPARE(v->region(), QRectF(0.25, 0.25, 0.5, 0.5));
        QVERIFY(roi->isSet());
        const QRectF w = roi->regionInWidget();
        QVERIFY(w.width() > 10 && w.height() > 10);
        QCOMPARE(roi->partAt(w.center()), 1);
        QCOMPARE(roi->partAt(w.topLeft()), 2);
        QCOMPARE(roi->partAt(QPointF(w.center().x(), w.bottom())), 7);
        QCOMPARE(roi->partAt(QPointF(w.left() - 40, w.top() - 40)), 0);
        // A drag on the inside moves it, on a corner resizes it, outside draws a new one.
        auto send = [roi](QEvent::Type type, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs) {
            QMouseEvent e(type, p, roi->mapToGlobal(p), b, bs, Qt::NoModifier);
            QApplication::sendEvent(roi, &e);
        };
        auto drag = [&](QPointF from, QPointF to) {
            send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= 4; ++i)
                send(QEvent::MouseMove, from + (to - from) * i / 4.0, Qt::NoButton, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        };
        const QRectF before = roi->region();
        drag(w.center(), w.center() + QPointF(20, 10));
        QVERIFY(roi->region().x() > before.x() && roi->region().y() > before.y());
        QCOMPARE(roi->region().size(), before.size());
        const QRectF moved = roi->region();
        drag(roi->regionInWidget().bottomRight(), roi->regionInWidget().bottomRight() - QPointF(25, 25));
        QVERIFY(roi->region().width() < moved.width() && roi->region().height() < moved.height());
        QCOMPARE(roi->region().topLeft(), moved.topLeft());
        // Double-click: the whole picture again; the region never gets smaller than a sliver.
        QMouseEvent dbl(QEvent::MouseButtonDblClick, roi->regionInWidget().center(),
            roi->mapToGlobal(roi->regionInWidget().center()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(roi, &dbl);
        QCOMPARE(roi->region(), QRectF(0, 0, 1, 1));
        roi->setRegion(QRectF(0.5, 0.5, 0.0001, 0.0001));
        QVERIFY(roi->region().width() >= ViewerRegion::kMinSide - 1e-9);
        v->toolAction(QStringLiteral("roi"))->trigger();
        QVERIFY(!roi->isVisible());
        QSettings().clear();
        ViewerOverlaySettings::instance().load();
    }

    void viewerProxyCacheAndTransportButtons()
    {
        // Spec 121.5 / 121.6: the transport's HiQ / MB / Prx / APrx / Cache buttons drive the viewers: a
        // proxy renders a smaller picture, Auto Proxy only while playing or changing something, and the
        // disk cache serves a picture again without rendering it.
        QSettings().clear();
        viewercache::clear();
        CompViewOptions::instance().load();
        Fixture f(true, true);
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        const quint64 bg = f.session()->addNode(nodetypes::Background);
        f.session()->setViewerNode(0, bg);
        CompViewer* v = f.page()->viewer(0);
        QVERIFY(f.session()->isStored());
        // The buttons exist, have tooltips and write the options.
        const auto button = [&f](const char* name) {
            auto* b = f.page()->findChild<QToolButton*>(QLatin1String(name));
            return b;
        };
        for (const char* name : {"CompHiQ", "CompMotionBlur", "CompProxy", "CompProxyRatio", "CompAutoProxy", "CompDiskCache"}) {
            QToolButton* b = button(name);
            QVERIFY2(b && !b->toolTip().isEmpty(), name);
        }
        QVERIFY(button("CompHiQ")->isChecked() && button("CompMotionBlur")->isChecked());
        QVERIFY(!button("CompProxy")->isChecked() && !button("CompAutoProxy")->isChecked() && !button("CompDiskCache")->isChecked());
        QCOMPARE(button("CompProxyRatio")->text(), QStringLiteral("1:2"));
        // Normal: the viewer renders at its own picture size.
        v->setZoom(0.0);
        QCoreApplication::processEvents();
        button("CompMotionBlur")->click(); // off: the viewer renders itself with a draft
        QVERIFY(!CompViewOptions::instance().options().motionBlur);
        QTRY_VERIFY_WITH_TIMEOUT(!v->lastRenderSize().isEmpty(), 10000);
        const QSize full = v->lastRenderSize();
        // Proxy 1:2.
        button("CompProxy")->click();
        QVERIFY(CompViewOptions::instance().options().proxy);
        QTRY_COMPARE_WITH_TIMEOUT(v->lastRenderSize(), CompViewOptions::instance().renderSize(full, false), 10000);
        QVERIFY(v->lastRenderSize().width() <= (full.width() + 1) / 2);
        button("CompProxyRatio")->click();
        QCOMPARE(CompViewOptions::instance().options().proxyRatio, 4);
        QCOMPARE(button("CompProxyRatio")->text(), QStringLiteral("1:4"));
        QTRY_VERIFY_WITH_TIMEOUT(v->lastRenderSize().width() <= (full.width() + 3) / 4, 10000);
        button("CompProxy")->click(); // off
        // Auto Proxy: a proxy while something changes, the full size again when it stops.
        button("CompAutoProxy")->click();
        QVERIFY(CompViewOptions::instance().options().autoProxy);
        f.project.timeline()->setPlayhead(22);
        QTRY_VERIFY_WITH_TIMEOUT(v->lastRenderSize().width() < full.width(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(v->lastRenderSize(), full, 10000);
        button("CompAutoProxy")->click();
        // Disk cache: the second request for a picture comes from the cache folder.
        button("CompDiskCache")->click();
        QVERIFY(CompViewOptions::instance().options().diskCache);
        f.project.timeline()->setPlayhead(25);
        // cacheMisses() describes only the viewer's latest render: a repeat render of the same state
        // (queued while the first one ran) finds the picture already stored and reports 0 misses, so
        // a fast machine can skip past the miss. A stored file is the lasting proof of the miss.
        QTRY_VERIFY_WITH_TIMEOUT(viewercache::usage().files > 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!f.page()->viewer(0)->lastRenderSize().isEmpty(), 10000);
        f.project.timeline()->setPlayhead(26);
        f.project.timeline()->setPlayhead(25); // back: the same picture
        QTRY_VERIFY_WITH_TIMEOUT(v->cacheHits() > 0, 10000);
        // A change of the graph makes another key: no hit for the new picture.
        const int files = viewercache::usage().files;
        f.session()->edit(QStringLiteral("Colour"), [bg](Composition& c) {
            c.node(bg)->params.setStatic(nodekeys::Color, QColor(10, 200, 30));
            return true;
        });
        // The new picture was a miss (a new file was stored). cacheHits() is the count of the
        // viewer's latest render, which may be a repeat render of the same, now cached, state.
        QTRY_VERIFY_WITH_TIMEOUT(viewercache::usage().files > files, 10000);
        CompViewOptions::instance().setOptions({});
        viewercache::clear();
        QSettings().clear();
    }

    void viewerShowsOnlyTheRegionAndTheOnionFrames()
    {
        // Spec 121.1 / 121.2 on the real viewer: with the region on, the picture outside it is cut
        // away; with Onion Skin on, the moving shape also shows where it was and will be.
        QSettings().clear();
        CompViewOptions::instance().load();
        Fixture f(true, true);
        f.project.timeline()->setPlayhead(20); // comp frame 10
        f.showPage();
        const quint64 bg = f.session()->addNode(nodetypes::Background);
        f.session()->edit(QStringLiteral("Colour"), [bg](Composition& c) {
            c.node(bg)->params.setStatic(nodekeys::Color, QColor(230, 20, 20));
            return true;
        });
        f.session()->setViewerNode(0, bg);
        CompViewer* v = f.page()->viewer(0);
        const auto shown = [v]() -> QImage {
            const QImage all = v->surface()->widget()->grab().toImage();
            return all.copy(v->surface()->pictureRect());
        };
        const auto isRed = [](QRgb p) { return qRed(p) > 150 && qGreen(p) < 90 && qBlue(p) < 90; };
        QTRY_VERIFY_WITH_TIMEOUT(isRed(shown().pixel(shown().width() / 2, shown().height() / 2)), 10000);
        QVERIFY(isRed(shown().pixel(3, 3)));
        v->setRegionShown(true);
        v->setRegion(QRectF(0.25, 0.25, 0.5, 0.5));
        QTRY_VERIFY_WITH_TIMEOUT(!isRed(shown().pixel(3, 3)), 10000);
        const QImage img = shown();
        QVERIFY(isRed(img.pixel(img.width() / 2, img.height() / 2)));
        QVERIFY(!isRed(img.pixel(img.width() - 4, img.height() - 4)));
        v->setRegionShown(false);
        QTRY_VERIFY_WITH_TIMEOUT(isRed(shown().pixel(3, 3)), 10000);
        // Onion Skin: a rectangle that moves; the neighbours' places show as tinted ghosts.
        const quint64 box = f.session()->addNode(nodetypes::RectangleMask);
        f.session()->edit(QStringLiteral("Move"), [box](Composition& c) {
            CompNode* n = c.node(box);
            n->params.setBase(nodekeys::Width, 0.1);
            n->params.setBase(nodekeys::Height, 0.4);
            AnimatedValue& x = n->params.animated(nodekeys::CenterX, 0.5);
            x.setKey(9, 0.3, Interp::Linear);
            x.setKey(10, 0.5, Interp::Linear);
            x.setKey(11, 0.7, Interp::Linear);
            return true;
        });
        f.session()->setViewerNode(0, box);
        QCOMPARE(f.session()->compFrame(), int64_t(10));
        const auto at = [&](double fx) {
            const QImage i = shown();
            return i.pixel(int(i.width() * fx), i.height() / 2);
        };
        QTRY_VERIFY_WITH_TIMEOUT(qRed(at(0.5)) > 200 && qBlue(at(0.5)) > 200, 10000); // the current frame: white
        QTRY_VERIFY_WITH_TIMEOUT(qRed(at(0.3)) < 30 && qBlue(at(0.3)) < 30, 10000); // nothing there yet
        v->setOnion(true);
        // Frame 9 (blue) and frame 11 (orange) are laid faintly over the black background.
        QTRY_VERIFY_WITH_TIMEOUT(qBlue(at(0.3)) > 60 && qBlue(at(0.3)) > qRed(at(0.3)) + 20, 10000);
        QVERIFY(qRed(at(0.7)) > 60 && qRed(at(0.7)) > qBlue(at(0.7)) + 20);
        QVERIFY(qRed(at(0.5)) > 200 && qBlue(at(0.5)) > 200); // the current one stays white
        v->setOnion(false);
        QTRY_VERIFY_WITH_TIMEOUT(qBlue(at(0.3)) < 30, 10000);
        QSettings().clear();
    }

    void viewerBezierAndTextPathHandles()
    {
        // Spec 116.3 / 117.1: the Bezier mask and the Text node's path show points with tangent
        // handles; every edit is one undo step.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        CompViewerOverlay* overlay = f.page()->viewer(1)->controls();
        auto send = [overlay](QEvent::Type type, QPointF p, Qt::MouseButton b,
                        Qt::MouseButtons bs, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            QMouseEvent e(type, p, overlay->mapToGlobal(p), b, bs, mods);
            QApplication::sendEvent(overlay, &e);
        };
        auto drag = [&](QPointF from, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, mods);
            for (int i = 1; i <= 4; ++i)
                send(QEvent::MouseMove, from + (to - from) * i / 4.0, Qt::NoButton,
                    Qt::LeftButton, mods);
            send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, mods);
        };
        auto doubleClick = [&](QPointF at) {
            send(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
            QMouseEvent e(QEvent::MouseButtonDblClick, at, overlay->mapToGlobal(at),
                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(overlay, &e);
        };
        const int64_t frame = f.session()->compFrame();
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.session()->setSelection({in});
        f.session()->addNode(nodetypes::BezierMask);
        const quint64 bz = f.nodeOfType(nodetypes::BezierMask);
        QVERIFY(bz);
        f.session()->setSelection({bz});
        QVERIFY(overlay->hasControls());
        const MaskPath path0 = f.comp().node(bz)->path.at(double(frame));
        QCOMPARE(path0.size(), 4);
        QVERIFY(path0[0].smooth && path0[0].hasTangents());
        // The centre cross moves the whole mask.
        QVERIFY(overlay->handlePos(CompViewerOverlay::Handle::Centre));
        // A point: drag it.
        const QPointF p1 = *overlay->handlePos(CompViewerOverlay::Handle::Point, 1);
        int index = -1;
        QCOMPARE(overlay->handleAt(p1, &index), CompViewerOverlay::Handle::Point);
        QCOMPARE(index, 1);
        int before = f.steps();
        drag(p1, p1 + QPointF(10, 8));
        QCOMPARE(f.steps(), before + 1);
        MaskPath path1 = f.comp().node(bz)->path.at(double(frame));
        QVERIFY(path1[1].x > path0[1].x + 1 && path1[1].y > path0[1].y + 1);
        QCOMPARE(path1[1].outX, path0[1].outX); // the tangents travel with the point
        QCOMPARE(overlay->selectedPoint(), 1);
        // A tangent handle: dragging the outgoing one bends the curve and turns the incoming
        // one of a smooth point the opposite way, keeping its length.
        const QPointF t = *overlay->handlePos(CompViewerOverlay::Handle::TanOut, 1);
        QCOMPARE(overlay->handleAt(t, &index), CompViewerOverlay::Handle::TanOut);
        const double inLen = std::hypot(path1[1].inX, path1[1].inY);
        before = f.steps();
        drag(t, t + QPointF(0, -14));
        QCOMPARE(f.steps(), before + 1);
        const MaskPath path2 = f.comp().node(bz)->path.at(double(frame));
        QVERIFY(std::abs(path2[1].outY - path1[1].outY) > 1.0);
        QVERIFY(std::abs(std::hypot(path2[1].inX, path2[1].inY) - inLen) < 1e-6);
        QVERIFY(std::abs(path2[1].inX * path2[1].outY - path2[1].inY * path2[1].outX) < 1e-3); // collinear
        QVERIFY(path2[1].inX * path2[1].outX + path2[1].inY * path2[1].outY < 0); // opposite
        // Alt breaks the point: the other handle stays, the point becomes a corner.
        const QPointF t2 = *overlay->handlePos(CompViewerOverlay::Handle::TanIn, 1);
        drag(t2, t2 + QPointF(9, 9), Qt::AltModifier);
        const MaskPath path3 = f.comp().node(bz)->path.at(double(frame));
        QVERIFY(!path3[1].smooth);
        QCOMPARE(path3[1].outX, path2[1].outX);
        // Double-click on a point turns a corner into a smooth one (and back).
        const QPointF p1b = *overlay->handlePos(CompViewerOverlay::Handle::Point, 1);
        before = f.steps();
        doubleClick(p1b);
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(f.comp().node(bz)->path.at(double(frame))[1].smooth);
        doubleClick(*overlay->handlePos(CompViewerOverlay::Handle::Point, 1));
        const MaskPoint corner = f.comp().node(bz)->path.at(double(frame))[1];
        QVERIFY(!corner.smooth && !corner.hasTangents());
        // Double-click on the curve adds a point; Delete removes the selected one.
        const QPointF a = *overlay->handlePos(CompViewerOverlay::Handle::Point, 2);
        const QPointF b = *overlay->handlePos(CompViewerOverlay::Handle::Point, 3);
        QPointF onCurve;
        {
            // The curve's middle: sample the widget path through the Segment hit test.
            bool found = false;
            for (int i = 1; i < 40 && !found; ++i) {
                const QPointF probe = a + (b - a) * (i / 40.0);
                for (int dy = -30; dy <= 30 && !found; dy += 2) {
                    for (int dx = -30; dx <= 30 && !found; dx += 2) {
                        const QPointF q = probe + QPointF(dx, dy);
                        int seg = -1;
                        if (overlay->handleAt(q, &seg) == CompViewerOverlay::Handle::Segment
                            && seg == 2) {
                            onCurve = q;
                            found = true;
                        }
                    }
                }
            }
            QVERIFY(found);
        }
        before = f.steps();
        doubleClick(onCurve);
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(f.comp().node(bz)->path.at(double(frame)).size(), 5);
        QCOMPARE(overlay->selectedPoint(), 3);
        overlay->setFocus();
        before = f.steps();
        QTest::keyClick(overlay, Qt::Key_Delete);
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(f.comp().node(bz)->path.at(double(frame)).size(), 4);
        f.project.undo();
        QCOMPARE(f.comp().node(bz)->path.at(double(frame)).size(), 5);

        // The Text node: path handles appear only with the Path layout.
        f.session()->setSelection({in});
        f.session()->addNode(nodetypes::Text);
        const quint64 tx = f.nodeOfType(nodetypes::Text);
        QVERIFY(tx);
        f.session()->setSelection({tx});
        QVERIFY(overlay->hasControls());
        QVERIFY(!overlay->handlePos(CompViewerOverlay::Handle::Point, 0));
        f.session()->edit(QStringLiteral("Layout"), [tx](Composition& c) {
            c.node(tx)->params.statics[nodekeys::LayoutType] = textlayouts::Path;
            return true;
        });
        QVERIFY(overlay->handlePos(CompViewerOverlay::Handle::Point, 0));
        const MaskPath tp0 = f.comp().node(tx)->path.at(double(frame));
        QVERIFY(tp0.size() >= 2);
        const QPointF q = *overlay->handlePos(CompViewerOverlay::Handle::Point, 1);
        QCOMPARE(overlay->handleAt(q, &index), CompViewerOverlay::Handle::Point);
        before = f.steps();
        drag(q, q + QPointF(0, -20));
        QCOMPARE(f.steps(), before + 1);
        const MaskPath tp1 = f.comp().node(tx)->path.at(double(frame));
        QVERIFY(tp1[1].y < tp0[1].y - 2); // up on the screen = smaller y
        // The centre / angle box still works (a drag inside, away from the path).
        QVERIFY(overlay->handlePos(CompViewerOverlay::Handle::Centre));
    }

    void followTrackRowOffersSizeAngleAndPoints()
    {
        // Spec 117.4: the Follow Track row has Position / Size / Angle / Points as the node allows.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.session()->setSelection({in});
        f.session()->addNode(nodetypes::PlanarTracker);
        const quint64 pt = f.nodeOfType(nodetypes::PlanarTracker);
        f.session()->setSelection({in});
        f.session()->addNode(nodetypes::PolygonMask);
        const quint64 poly = f.nodeOfType(nodetypes::PolygonMask);
        const int64_t frame = f.session()->compFrame();
        f.session()->edit(QStringLiteral("Track"), [&](Composition& c) {
            // A plane that turns 30 degrees counter-clockwise and grows by half over 10 frames.
            for (int i = 0; i <= 10; ++i) {
                const double a = 30.0 * i / 10 * M_PI / 180.0, s = 1.0 + 0.05 * i;
                const double xs[4] = {-1, 1, 1, -1}, ys[4] = {1, 1, -1, -1};
                for (int k = 0; k < 4; ++k) {
                    // A square of 200 px turned on the picture (not in normalised units).
                    const double x = 100 * s * (xs[k] * std::cos(a) - ys[k] * std::sin(a))
                        / f.session()->frameSize().width();
                    const double y = 100 * s * (xs[k] * std::sin(a) + ys[k] * std::cos(a))
                        / f.session()->frameSize().height();
                    c.node(pt)->params.animated(nodekeys::planarCorner(k, false), 0.5)
                        .setKey(frame + i, 0.5 + x, Interp::Linear);
                    c.node(pt)->params.animated(nodekeys::planarCorner(k, true), 0.5)
                        .setKey(frame + i, 0.5 + y, Interp::Linear);
                }
            }
            return true;
        });
        f.session()->setSelection({poly});
        CompInspector* inspector = f.page()->inspector();
        QVERIFY(inspector->followPicker() && inspector->followButton());
        QVERIFY(inspector->followPosition() && inspector->followPosition()->isChecked());
        QVERIFY(inspector->followSize() && inspector->followAngle() && inspector->followPoints());
        QVERIFY(!inspector->followSize()->toolTip().isEmpty());
        inspector->followSize()->setChecked(true);
        inspector->followAngle()->setChecked(true);
        const int before = f.steps();
        inspector->followButton()->click();
        QCOMPARE(f.steps(), before + 1);
        const ParamSet& p = f.comp().node(poly)->params;
        QVERIFY(std::abs(p.value(nodekeys::Size, frame + 10, 0) - 1.5) < 0.02);
        QVERIFY(std::abs(p.value(nodekeys::Angle, frame + 10, 0) - 30.0) < 0.5);
        // A Merge has no points; a Text without the Path layout neither.
        f.session()->setSelection({in});
        f.session()->addNode(nodetypes::Text);
        f.session()->setSelection({f.nodeOfType(nodetypes::Text)});
        QVERIFY(inspector->followPicker());
        QVERIFY(!inspector->followPoints() && !inspector->followSize() && inspector->followAngle());
    }

    void trackerPatternBoxes()
    {
        // Spec 118.2: a Tracker with several patterns shows a box for each; the others can be
        // picked and dragged, which makes them the active one.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        CompViewerOverlay* overlay = f.page()->viewer(1)->controls();
        auto send = [overlay](QEvent::Type type, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs) {
            QMouseEvent e(type, p, overlay->mapToGlobal(p), b, bs, Qt::NoModifier);
            QApplication::sendEvent(overlay, &e);
        };
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.session()->setSelection({in});
        f.session()->addNode(nodetypes::Tracker);
        const quint64 tr = f.nodeOfType(nodetypes::Tracker);
        f.session()->setSelection({tr});
        QVERIFY(!overlay->handlePos(CompViewerOverlay::Handle::Point, 1)); // one pattern only
        f.session()->edit(QStringLiteral("Patterns"), [tr](Composition& c) {
            c.node(tr)->params.setBase(nodekeys::Patterns, 2);
            return true;
        });
        const QPointF second = *overlay->handlePos(CompViewerOverlay::Handle::Point, 1);
        int index = -1;
        QCOMPARE(overlay->handleAt(second, &index), CompViewerOverlay::Handle::Point);
        QCOMPARE(index, 1);
        const int before = f.steps();
        const QPointF to = second + QPointF(25, 15);
        send(QEvent::MouseButtonPress, second, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(f.steps(), before + 1);
        const CompNode* n = f.comp().node(tr);
        const int64_t frame = f.session()->compFrame();
        QCOMPARE(int(std::lround(n->params.value(nodekeys::ActivePattern, 0, 1))), 2);
        QVERIFY(n->params.value(nodekeys::patternKey(2, false), frame, 0) > 0.6 + 0.01);
        QVERIFY(n->params.value(nodekeys::patternKey(2, true), frame, 1) < 0.5 - 0.01);
        QCOMPARE(n->params.value(nodekeys::CenterX, frame, 0), 0.5); // pattern 1 stays
        // Now pattern 1 is the other box.
        QCOMPARE(overlay->handleAt(*overlay->handlePos(CompViewerOverlay::Handle::Point, 0)),
            CompViewerOverlay::Handle::Point);
        f.project.undo();
        QCOMPARE(int(std::lround(f.comp().node(tr)->params.value(nodekeys::ActivePattern, 0, 1))), 1);
    }

    void pointFollowerFromTheViewerMenu()
    {
        // Spec 117.5: a point of a Bezier mask follows a track from the viewer.
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        CompViewerOverlay* overlay = f.page()->viewer(1)->controls();
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.session()->setSelection({in});
        f.session()->addNode(nodetypes::Tracker);
        const quint64 tracker = f.nodeOfType(nodetypes::Tracker);
        f.session()->addNode(nodetypes::BezierMask);
        const quint64 bz = f.nodeOfType(nodetypes::BezierMask);
        QVERIFY(tracker && bz);
        const int64_t frame = f.session()->compFrame();
        f.session()->edit(QStringLiteral("Track"), [&](Composition& c) {
            AnimatedValue& x = c.node(tracker)->params.animated(nodekeys::CenterX, 0.5);
            AnimatedValue& y = c.node(tracker)->params.animated(nodekeys::CenterY, 0.5);
            for (int i = 0; i <= 10; ++i) {
                x.setKey(frame + i, 0.5 + 0.01 * i, Interp::Linear);
                y.setKey(frame + i, 0.5, Interp::Linear);
            }
            return true;
        });
        f.session()->setSelection({bz});
        const MaskPath before = f.comp().node(bz)->path.at(double(frame));
        const int steps = f.steps();
        QVERIFY(overlay->followPoint(2, tracker));
        QCOMPARE(f.steps(), steps + 1);
        const MaskPath after = f.comp().node(bz)->path.at(double(frame + 10));
        QVERIFY(f.comp().node(bz)->path.isAnimated());
        QVERIFY(after[2].x > before[2].x + 1);
        QCOMPARE(after[0].x, before[0].x);
        f.project.undo();
        QVERIFY(!f.comp().node(bz)->path.isAnimated());
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

    void openTransitionOnThePage()
    {
        Fixture f;
        // A second clip right after the first one, with a Fusion Composition transition.
        ClipData b;
        b.kind = ClipKind::Solid;
        b.start = 60;
        b.duration = 40;
        TransitionData t;
        t.type = kCompositionTransition;
        t.duration = 10;
        b.transitionIn = t;
        f.project.beginEdit(QStringLiteral("Add Clip"));
        const quint64 second = editops::overwriteClip(
            f.project.timeline()->mutableData(), TrackRef {TrackType::Video, 0}, b);
        f.project.endEdit();
        f.showPage();
        f.session()->openTransition(second, false);
        QCOMPARE(f.session()->transition(), 1);
        QCOMPARE(f.session()->clipId(), second);
        QVERIFY(!f.session()->isStored()); // the default graph is only shown
        int sides[3] = {0, 0, 0};
        for (const CompNode& n : f.comp().nodes)
            ++sides[n.transitionSide];
        QCOMPARE(sides[1], 1);
        QCOMPARE(sides[2], 1);
        QCOMPARE(f.session()->compFrameCount(), 10);
        // Centred on the cut at 60: the window is frames 55 .. 64.
        f.session()->seekCompFrame(3);
        QCOMPARE(f.project.timeline()->playhead(), 58);
        QCOMPARE(f.session()->compFrame(), 3);
        // The first edit stores the graph in the transition, as one undo step.
        QVERIFY(f.session()->addNode(nodetypes::Transform) != 0);
        const ClipData* stored = editops::clipById(f.project.timeline()->data(), second);
        QVERIFY(stored->transitionIn->composition);
        QVERIFY(f.session()->isStored());
        QVERIFY(!f.clip()->composition); // the first clip is not touched
        f.project.undo();
        QVERIFY(!editops::clipById(f.project.timeline()->data(), second)->transitionIn->composition);
        // Opening a clip again leaves the transition.
        f.session()->openClip(f.clipId);
        QCOMPARE(f.session()->transition(), 0);
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
        // Spec 89.13: the spline keys are on here.
        const auto splineKeys = f.page()->splinePanel()->findChildren<QShortcut*>();
        QVERIFY(!splineKeys.isEmpty());
        for (QShortcut* sc : splineKeys)
            QVERIFY(sc->isEnabled());
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

    // ---- Spec 90: viewer features (A/B, channels, checker, controls, lock, clear, 100 %, loop) ----

    void spec90_channelCheckerAndWipeHelpers()
    {
        using namespace viewerfx;
        QImage img(8, 4, QImage::Format_ARGB32);
        img.fill(qRgba(200, 100, 50, 255));
        img.setPixel(0, 0, qRgba(10, 20, 30, 0));
        QCOMPARE(qRed(showChannel(img, Channel::Red).pixel(3, 1)), 200);
        QCOMPARE(qGreen(showChannel(img, Channel::Green).pixel(3, 1)), 100);
        QCOMPARE(qBlue(showChannel(img, Channel::Blue).pixel(3, 1)), 50);
        const QImage alpha = showChannel(img, Channel::Alpha);
        QCOMPARE(qRed(alpha.pixel(3, 1)), 255);
        QCOMPARE(qRed(alpha.pixel(0, 0)), 0);
        QCOMPARE(qAlpha(alpha.pixel(0, 0)), 255); // the channel view is opaque
        QCOMPARE(showChannel(img, Channel::Color), img);
        // Checker: a transparent picture shows two different greys; an opaque pixel stays.
        QImage clear(64, 32, QImage::Format_ARGB32_Premultiplied);
        clear.fill(Qt::transparent);
        clear.setPixelColor(40, 20, QColor(255, 0, 0));
        const QImage c = overChecker(clear, 16);
        QVERIFY(c.pixel(2, 2) != c.pixel(20, 2));
        QCOMPARE(c.pixel(2, 2), c.pixel(36, 2));
        QCOMPARE(c.pixelColor(40, 20), QColor(255, 0, 0));
        QCOMPARE(qAlpha(c.pixel(2, 2)), 255);
        // Wipe: A on the left of a vertical line, B on the right; 0 degrees swaps to the top.
        QImage a(20, 10, QImage::Format_ARGB32_Premultiplied), b = a;
        a.fill(Qt::red);
        b.fill(Qt::blue);
        const QImage w = splitWipe(a, b, QPointF(0.5, 0.5), 90);
        QCOMPARE(w.pixelColor(2, 5), QColor(Qt::red));
        QCOMPARE(w.pixelColor(17, 5), QColor(Qt::blue));
        const QImage h = splitWipe(a, b, QPointF(0.5, 0.5), 0);
        QVERIFY(h.pixelColor(10, 1) != h.pixelColor(10, 8));
    }

    void spec90_buffersAAndB()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        CompViewer* v = f.page()->viewer(0);
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const quint64 out = f.nodeNamed(QStringLiteral("MediaOut1"));
        f.session()->setViewerNode(0, in);
        QCOMPARE(v->activeBuffer(), 0);
        QTest::keyClick(v, Qt::Key_Period); // buffer B: starts as a copy of A
        QCOMPARE(v->activeBuffer(), 1);
        QCOMPARE(v->bufferNode(1), in);
        QVERIFY(v->toolAction(QStringLiteral("b"))->isChecked());
        // A node assigned while B is shown goes to B; A keeps its node.
        f.session()->setViewerNode(0, out);
        QCOMPARE(v->bufferNode(1), out);
        QCOMPARE(v->bufferNode(0), in);
        QCOMPARE(f.session()->viewerNode(0), in);
        QCOMPARE(v->node(), out);
        QTest::keyClick(v, Qt::Key_Comma);
        QCOMPARE(v->activeBuffer(), 0);
        QCOMPARE(v->node(), in);
        QVERIFY(v->headerText().contains(QStringLiteral("MediaIn1")));
        // Split display with / (and the A|B button), a movable line.
        QVERIFY(!v->splitShown());
        QTest::keyClick(v, Qt::Key_Slash);
        QVERIFY(v->splitShown());
        QVERIFY(v->toolAction(QStringLiteral("split"))->isChecked());
        v->setSplitLine(QPointF(0.25, 0.5), 45);
        QCOMPARE(v->splitPos(), QPointF(0.25, 0.5));
        QCOMPARE(v->splitAngle(), 45.0);
        v->toolAction(QStringLiteral("split"))->trigger();
        QVERIFY(!v->splitShown());
        // Buffers are per viewer.
        QCOMPARE(f.page()->viewer(1)->activeBuffer(), 0);
    }

    void spec90_channelKeys()
    {
        Fixture f;
        f.showPage();
        CompViewer* v = f.page()->viewer(1);
        using viewerfx::Channel;
        QCOMPARE(v->channel(), Channel::Color);
        const std::pair<Qt::Key, Channel> keys[] = {{Qt::Key_R, Channel::Red},
            {Qt::Key_G, Channel::Green}, {Qt::Key_B, Channel::Blue}, {Qt::Key_A, Channel::Alpha},
            {Qt::Key_C, Channel::Color}};
        const char* ids[] = {"red", "green", "blue", "alpha", "color"};
        int i = 0;
        for (const auto& [key, ch] : keys) {
            QTest::keyClick(v, key);
            QCOMPARE(v->channel(), ch);
            QVERIFY(v->toolAction(QLatin1String(ids[i++]))->isChecked());
        }
        // The same key again goes back to colour (button).
        v->toolAction(QStringLiteral("red"))->trigger();
        QCOMPARE(v->channel(), Channel::Red);
        v->toolAction(QStringLiteral("red"))->trigger();
        QCOMPARE(v->channel(), Channel::Color);
    }

    void spec90_checkerControlsLockClearAndZoom()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        CompViewer* v = f.page()->viewer(0);
        CompViewer* r = f.page()->viewer(1);
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.session()->setViewerNode(0, in);
        // Checker button.
        QVERIFY(!v->checkerShown());
        v->toolAction(QStringLiteral("checker"))->trigger();
        QVERIFY(v->checkerShown());
        // Ctrl+K hides and shows the on-screen controls.
        QVERIFY(v->controls()->isVisible());
        QTest::keyClick(v, Qt::Key_K, Qt::ControlModifier);
        QVERIFY(!v->controlsShown());
        QVERIFY(!v->controls()->isVisible());
        QTest::keyClick(v, Qt::Key_K, Qt::ControlModifier);
        QVERIFY(v->controls()->isVisible());
        // Ctrl+L locks: 1 / 2 and drops no longer change what the viewer shows.
        QTest::keyClick(v, Qt::Key_L, Qt::ControlModifier);
        QVERIFY(v->locked());
        const quint64 out = f.nodeNamed(QStringLiteral("MediaOut1"));
        f.session()->setViewerNode(0, out);
        QCOMPARE(v->node(), in);
        QVERIFY(v->headerText().contains(QStringLiteral("MediaIn1")));
        QTest::keyClick(v, Qt::Key_L, Qt::ControlModifier);
        QVERIFY(!v->locked());
        QCOMPARE(v->node(), out);
        // Ctrl+1: 100 %.
        v->setZoom(0.0);
        QTest::keyClick(v, Qt::Key_1, Qt::ControlModifier);
        QCOMPARE(v->surface()->picture().zoom(), 1.0);
        // ~ clears both viewers (and the lock).
        QTest::keyClick(v, Qt::Key_L, Qt::ControlModifier);
        QCOMPARE(r->node(), f.nodeNamed(QStringLiteral("MediaOut1")));
        QTest::keyClick(v, Qt::Key_AsciiTilde, Qt::ShiftModifier);
        QVERIFY(!v->locked());
        QCOMPARE(v->node(), quint64(0));
        QCOMPARE(r->node(), quint64(0));
        QCOMPARE(f.session()->viewerNode(0), quint64(0));
        QVERIFY(v->headerText().contains(QStringLiteral("No Node")));
    }

    // The keys are viewer-local: with the focus in the node editor they do nothing, and the
    // global keys (I = Mark In, ...) are not touched.
    void spec90_keysActOnlyWithViewerFocus()
    {
        Fixture f;
        f.showPage();
        CompViewer* v = f.page()->viewer(1);
        f.editor()->setFocus();
        for (Qt::Key k : {Qt::Key_R, Qt::Key_G, Qt::Key_A, Qt::Key_Comma, Qt::Key_Slash})
            QTest::keyClick(f.editor(), k);
        QTest::keyClick(f.editor(), Qt::Key_K, Qt::ControlModifier);
        QTest::keyClick(f.editor(), Qt::Key_L, Qt::ControlModifier);
        QCoreApplication::processEvents();
        QCOMPARE(v->channel(), viewerfx::Channel::Color);
        QCOMPARE(v->activeBuffer(), 0);
        QVERIFY(!v->splitShown());
        QVERIFY(v->controlsShown());
        QVERIFY(!v->locked());
        // The viewer itself ignores keys it does not own (I stays free for Mark In).
        QKeyEvent over(QEvent::ShortcutOverride, Qt::Key_I, Qt::NoModifier);
        QCoreApplication::sendEvent(v, &over);
        QVERIFY(!over.isAccepted());
        QKeyEvent mine(QEvent::ShortcutOverride, Qt::Key_R, Qt::NoModifier);
        QCoreApplication::sendEvent(v, &mine);
        QVERIFY(mine.isAccepted());
    }

    void spec90_viewerToolBarFitsA1366Window()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.window->resize(1366, 768);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        f.action("view.page_compositing")->trigger();
        QCoreApplication::processEvents();
        for (int i = 0; i < 2; ++i) {
            QToolBar* bar = f.page()->viewer(i)->toolBar();
            QVERIFY(bar->isVisible());
            const QRect barRect(bar->mapTo(f.window.get(), QPoint(0, 0)), bar->size());
            QVERIFY(f.window->rect().contains(barRect));
            QList<QRect> rects;
            for (QToolButton* b : bar->findChildren<QToolButton*>()) {
                if (!b->isVisible())
                    continue;
                const QRect r(b->mapTo(f.window.get(), QPoint(0, 0)), b->size());
                QVERIFY2(barRect.contains(r), qPrintable(b->text())); // nothing cut off
                rects << r;
            }
            QVERIFY(rects.size() >= 2);
            for (int a = 0; a < rects.size(); ++a)
                for (int b = a + 1; b < rects.size(); ++b)
                    QVERIFY2(!rects[a].intersects(rects[b]), "viewer tool buttons overlap");
        }
    }

    void spec90_loopModeHasThreeStates()
    {
        Fixture f(true, true);
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        CompTransport* t = f.page()->transport();
        QCOMPARE(t->loopMode(), CompLoopMode::Loop);
        auto* button = t->findChild<QToolButton*>(QStringLiteral("CompLoop"));
        QVERIFY(button);
        button->click();
        QCOMPARE(t->loopMode(), CompLoopMode::PingPong);
        QCOMPARE(button->text(), QStringLiteral("Ping-pong"));
        button->click();
        QCOMPARE(t->loopMode(), CompLoopMode::Once);
        button->click();
        QCOMPARE(t->loopMode(), CompLoopMode::Loop);
        // Once: playing from the last frame stops at it.
        t->setLoopMode(CompLoopMode::Once);
        f.project.timeline()->setPlayhead(58);
        t->play();
        QTRY_VERIFY_WITH_TIMEOUT(!t->isPlaying(), 5000);
        QVERIFY(f.project.timeline()->playhead() >= 58);
        QVERIFY(f.project.timeline()->playhead() <= 59);
        // Ping-pong: at the end the engine turns round.
        t->setLoopMode(CompLoopMode::PingPong);
        f.project.timeline()->setPlayhead(57);
        t->play();
        QTRY_VERIFY_WITH_TIMEOUT(f.playback->rate() < 0.0, 5000);
        t->stop();
    }

    // ---- issue #7 (spec 99) ----
    void spec99_loopModeWrapsAtTheClipEnd()
    {
        // The clip (10..59) ends the timeline: the engine never plays past it, so Loop has to
        // wrap at the last frame.
        Fixture f(true, true);
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        CompTransport* t = f.page()->transport();
        QCOMPARE(t->loopMode(), CompLoopMode::Loop);
        f.project.timeline()->setPlayhead(54);
        t->play();
        QTRY_VERIFY_WITH_TIMEOUT(f.project.timeline()->playhead() < 40, 5000);
        QVERIFY(f.playback->isPlaying()); // still playing after the wrap
        QTest::qWait(300);
        QVERIFY(f.playback->isPlaying());
        t->stop();
    }

    void spec99_viewerIsServedByTheProgramEngine()
    {
        // A clip on V2 over the open clip: the engine renders the viewer's node (not the
        // tracks above), with and without a stored graph, and statistics count its frames.
        Fixture f(true, true);
        ClipData over;
        over.kind = ClipKind::Solid;
        over.name = QStringLiteral("Over");
        over.start = 20;
        over.duration = 30;
        f.project.beginEdit(QStringLiteral("Add Clip"));
        editops::overwriteClip(
            f.project.timeline()->mutableData(), TrackRef {TrackType::Video, 1}, over);
        f.project.endEdit();
        f.project.timeline()->setPlayhead(30);
        QVERIFY(!f.playback->compositionViewOwner());
        f.showPage();
        QVERIFY(f.session()->clipId() == f.clipId);
        QVERIFY(!f.session()->isStored());
        QVERIFY(f.playback->compositionViewOwner()); // empty graph
        f.session()->addNode(nodetypes::Transform);
        QVERIFY(f.session()->isStored());
        QVERIFY(f.playback->compositionViewOwner()); // same with a node added
        f.playback->play();
        QTest::qWait(500);
        f.playback->resetStats();
        QTest::qWait(1000);
        const PlaybackEngine::Stats s = f.playback->stats();
        f.playback->pause();
        qInfo("compositing page playback: %.1f fps shown, %lld dropped", s.fps(),
            qlonglong(s.dropped));
        QVERIFY(s.presented > 0);
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QVERIFY(!f.playback->compositionViewOwner()); // the Edit page shows the timeline again
    }

    void spec101_compositingPageSound()
    {
        // V1 clip (linked to A1) with a V2 clip over it (linked to A2): the page plays the
        // timeline mix by default; Solo keeps only the clip's own sound; the Audio button
        // silences the page; Sound Offset moves the clip's sound; the Edit page is untouched.
        Fixture f(true, true);
        ClipData over;
        over.kind = ClipKind::Solid;
        over.start = 20;
        over.duration = 30;
        ClipData a1;
        a1.kind = ClipKind::Solid;
        a1.start = 10;
        a1.duration = 50;
        ClipData a2 = a1;
        a2.start = 20;
        a2.duration = 30;
        f.project.beginEdit(QStringLiteral("Add Clips"));
        TimelineData& td = f.project.timeline()->mutableData();
        editops::addTrack(td, TrackType::Video);
        while (td.audioTracks.size() < 2)
            editops::addTrack(td, TrackType::Audio);
        const quint64 v2 = editops::overwriteClip(td, TrackRef {TrackType::Video, 1}, over);
        QVERIFY(v2);
        const quint64 a1id = editops::overwriteClip(td, TrackRef {TrackType::Audio, 0}, a1);
        const quint64 a2id = editops::overwriteClip(td, TrackRef {TrackType::Audio, 1}, a2);
        QVERIFY(a1id && a2id);
        // Clips are told apart by their link group below (7 = the open clip's, 8 = V2's).
        for (TrackData& t : td.videoTracks)
            for (ClipData& c : t.clips)
                c.linkGroup = c.id == f.clipId ? 7 : c.id == v2 ? 8 : 0;
        for (TrackData& t : td.audioTracks)
            for (ClipData& c : t.clips)
                c.linkGroup = c.id == a1id ? 7 : c.id == a2id ? 8 : 0;
        f.project.endEdit();
        auto audioClip = [&f](quint64 id) {
            const auto sound = f.playback->soundTimeline(); // keep it alive for the loop
            for (const TrackData& t : sound->audioTracks)
                for (const ClipData& c : t.clips)
                    if (c.linkGroup == id)
                        return c;
            return ClipData();
        };
        auto enabledAudio = [&audioClip](quint64 id) {
            const ClipData c = audioClip(id);
            return c.linkGroup == id && c.enabled;
        };
        f.project.timeline()->setPlayhead(15); // only the V1 clip is under the playhead
        // Edit page: nothing changes.
        QVERIFY(enabledAudio(7) && enabledAudio(8));
        f.showPage();
        // Default: the timeline mix, V2's sound included.
        QVERIFY(enabledAudio(7));
        QVERIFY(enabledAudio(8));
        // Solo this clip through the Inspector (one undo step).
        f.session()->setSelection({f.nodeNamed(QStringLiteral("MediaIn1"))});
        QComboBox* mode = f.page()->inspector()->audioPicker();
        QVERIFY(mode);
        QVERIFY(f.page()->inspector()->soundOffset());
        const int before = f.steps();
        mode->setCurrentIndex(1);
        Q_EMIT mode->activated(1);
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(f.comp().soloAudio);
        QVERIFY(enabledAudio(7));
        QVERIFY(!enabledAudio(8));
        // Saved and loaded; the default writes nothing.
        QVERIFY(serialization::compositionToJson(f.comp(), false).contains(QLatin1String("audio")));
        QVERIFY(serialization::compositionFromJson(serialization::compositionToJson(f.comp(), false))
                    .soloAudio);
        QVERIFY(!serialization::compositionToJson(Composition(), false)
                     .contains(QLatin1String("audio")));
        // Sound Offset: 2.5 frames later = the source position of the linked clip moves back.
        QDoubleSpinBox* offset = f.page()->inspector()->soundOffset();
        offset->setValue(2.5);
        Q_EMIT offset->editingFinished();
        QCOMPARE(f.comp().soundOffset, 2.5);
        const double fps = f.project.timeline()->data().fps.toDouble();
        QVERIFY(std::abs(double(audioClip(7).sourceInUs) + 2.5 / fps * 1e6) < 2.0);
        QCOMPARE(audioClip(8).sourceInUs, int64_t(0));
        QCOMPARE(serialization::compositionFromJson(serialization::compositionToJson(f.comp(), false))
                     .soundOffset,
            2.5);
        // Undo both steps.
        f.project.undoStack()->undo();
        QCOMPARE(f.comp().soundOffset, 0.0);
        f.project.undoStack()->undo();
        QVERIFY(!f.comp().soloAudio);
        QVERIFY(enabledAudio(8));
        QCOMPARE(audioClip(7).sourceInUs, int64_t(0));
        // The Audio button: off = silent, remembered; on = the mix again.
        QToolButton* button = f.page()->transport()->audioButton();
        QVERIFY(button->isChecked());
        button->click();
        QVERIFY(!f.page()->transport()->audioOn());
        QVERIFY(!QSettings().value(QStringLiteral("compositing/audio")).toBool());
        QVERIFY(!enabledAudio(7) && !enabledAudio(8));
        // The Edit page plays the timeline whatever the button says.
        f.action("view.page_edit")->trigger();
        QCoreApplication::processEvents();
        QVERIFY(enabledAudio(7) && enabledAudio(8));
        f.action("view.page_compositing")->trigger();
        QCoreApplication::processEvents();
        QVERIFY(!enabledAudio(7));
        button->click();
        QVERIFY(enabledAudio(7) && enabledAudio(8));
    }

    void spec90_channelViewRendersAnImage()
    {
        Fixture f(true, true);
        f.project.timeline()->setPlayhead(20);
        f.showPage();
        CompViewer* v = f.page()->viewer(1);
        f.session()->renameNode(f.nodeNamed(QStringLiteral("MediaIn1")), QStringLiteral("Plate"));
        v->setChannel(viewerfx::Channel::Alpha);
        v->setChecker(true);
        v->setSplit(true);
        QTRY_VERIFY_WITH_TIMEOUT(v->surface()->picture().hasFrame(), 5000);
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

    void spec43_toolRowNeverOverlapsAndTreeNamesAreReadable()
    {
        Fixture f;
        f.project.timeline()->setPlayhead(20);
        f.window->resize(1366, 768);
        f.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(f.window.get()));
        f.action("view.page_compositing")->trigger();
        const quint64 t = animatedTransform(f);
        Q_UNUSED(t);
        f.action("view.toggle_spline")->trigger();
        QTest::qWait(100);
        QCoreApplication::processEvents();
        // Node editor tool row: no two visible buttons intersect, and some moved into the "»" menu.
        const auto buttons = f.window->findChildren<QToolButton*>();
        QList<QRect> rects;
        for (QToolButton* b : buttons) {
            if (!b->objectName().startsWith(QStringLiteral("CompTool:")) || !b->isVisible())
                continue;
            rects << QRect(b->mapTo(f.window.get(), QPoint(0, 0)), b->size());
        }
        QVERIFY(!rects.isEmpty());
        for (int i = 0; i < rects.size(); ++i)
            for (int j = i + 1; j < rects.size(); ++j)
                QVERIFY2(!rects[i].intersects(rects[j]), "tool buttons overlap");
        // The Spline panel's tree shows whole names.
        KeyframesPanel* panel = f.page()->splinePanel();
        int checked = 0;
        for (QLabel* l : panel->tree()->findChildren<QLabel*>()) {
            if (l->text() != f.comp().node(f.session()->currentNode())->name
                && l->text() != QStringLiteral("Angle") && l->text() != QStringLiteral("Center X"))
                continue;
            QVERIFY2(l->isVisible(), qPrintable(l->text()));
            QVERIFY2(
                l->width() >= l->fontMetrics().horizontalAdvance(l->text()), qPrintable(l->text()));
            QVERIFY(l->toolTip() == l->text());
            ++checked;
        }
        QCOMPARE(checked, 3);
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
