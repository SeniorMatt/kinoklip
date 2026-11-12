// SPDX-License-Identifier: GPL-3.0-or-later
// Vim keys in the node graphs (spec section 83, key context "nodes"): selecting nodes by where
// they are and along the signal, adding, deleting, copying, bypassing, renaming and connecting,
// in the Compositing page's NodeEditor and the Color page's ColorNodeEditor.
#include "PanelTestUtils.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/VimController.h"
#include "app/pages/ColorPage.h"
#include "core/Grade.h"
#include "core/NodeRegistry.h"
#include "core/Timeline.h"
#include "ui/color/ColorNodeEditor.h"
#include "ui/color/ColorSession.h"
#include "ui/common/Resources.h"
#include "ui/common/VimInput.h"
#include "ui/common/VimNodeHost.h"
#include "ui/common/VimOptions.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/NodeEditor.h"

#include <QApplication>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QUndoStack>

using namespace kk;

namespace {

VimNodeInfo tile(quint64 id, double x, double y)
{
    return {id, QRectF(x, y, 100, 60), QString::number(id)};
}

/// The Compositing editor with the Vim layer and the node host, MediaIn -> MediaOut shown.
struct CompFixture : test::PanelFixture
{
    std::unique_ptr<CompSession> session;
    std::unique_ptr<NodeEditor> editor;
    struct NullHost final : VimHost
    {
        bool trigger(const QString&) override { return false; }
        int64_t playhead() const override { return 0; }
        void setPlayhead(int64_t) override { }
        void clearSelection() override { }
        void openCommandLine() override { }
    } nullHost;
    VimInput input {nullptr};
    VimNodeHost host {nullptr};
    QStringList messages;

    CompFixture()
    {
        const quint64 clip = addClip(TrackRef {TrackType::Video, 0}, 0, 100);
        session = std::make_unique<CompSession>(ctx);
        session->openClip(clip);
        editor = std::make_unique<NodeEditor>(session.get(), &actions);
        editor->resize(1000, 500);
        editor->show();
        editor->setView(QPointF(4, 0.5), 1.0);
        input.setHost(&nullHost);
        input.setContextHost(QStringLiteral("nodes"), &host);
        host.onMessage = [this](const QString& text) { messages.append(text); };
        input.setActive(true);
        editor->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents();
    }
    const Composition& comp() const { return session->composition(); }
    int steps() const { return project.undoStack()->count(); }
    quint64 nodeNamed(const QString& name) const
    {
        for (const CompNode& n : comp().nodes) {
            if (n.name == name)
                return n.id;
        }
        return 0;
    }
    void type(const QString& keys)
    {
        for (const QChar c : keys) {
            QWidget* focus = QApplication::focusWidget();
            if (c.isUpper())
                QTest::keyClick(focus, c.toLatin1(), Qt::ShiftModifier);
            else
                QTest::keyClick(focus, c.toLatin1());
        }
    }
};

} // namespace

class TestVimNodes : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_vimnodes"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // ---- The geometry, without widgets ----

    void neighbourIsTheNearestInThatDirection()
    {
        //  1   2   3
        //      4
        const QVector<VimNodeInfo> nodes {
            tile(1, 0, 0), tile(2, 200, 0), tile(3, 400, 0), tile(4, 200, 150)};
        using D = VimNodeHost::Dir;
        QCOMPARE(VimNodeHost::neighbour(nodes, 1, D::Right), quint64(2));
        QCOMPARE(VimNodeHost::neighbour(nodes, 2, D::Right), quint64(3));
        QCOMPARE(VimNodeHost::neighbour(nodes, 2, D::Left), quint64(1));
        QCOMPARE(VimNodeHost::neighbour(nodes, 2, D::Down), quint64(4));
        QCOMPARE(VimNodeHost::neighbour(nodes, 4, D::Up), quint64(2));
        QCOMPARE(VimNodeHost::neighbour(nodes, 3, D::Right), quint64(0)); // nothing there
        QCOMPARE(VimNodeHost::neighbour(nodes, 1, D::Up), quint64(0));
        // Down from 1: no node below in the cone, so the nearest one that lies below at all.
        QCOMPARE(VimNodeHost::neighbour(nodes, 1, D::Down), quint64(4));
    }

    void signalOrderFollowsTheLinks()
    {
        const QVector<VimNodeInfo> nodes {tile(1, 400, 0), tile(2, 0, 0), tile(3, 200, 0)};
        const QVector<std::pair<quint64, quint64>> links {{2, 3}, {3, 1}};
        QCOMPARE(VimNodeHost::signalOrder(nodes, links), (QVector<quint64> {2, 3, 1}));
        // No links: top to bottom.
        QCOMPARE(VimNodeHost::signalOrder({tile(5, 0, 90), tile(6, 0, 10)}, {}),
            (QVector<quint64> {6, 5}));
    }

    // ---- The Compositing editor ----

    void hjklSelectByPositionInTheCompositingGraph()
    {
        CompFixture f;
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const quint64 out = f.nodeNamed(QStringLiteral("MediaOut1"));
        const quint64 lone = f.session->addNodeAt(
            NodeRegistry::effectNodeType(QStringLiteral("gaussian_blur")), QPointF(4.0, 3.0));
        QVERIFY(lone);
        f.session->setSelection({in}, {}, in);
        f.type(QStringLiteral("l"));
        QVERIFY(f.session->currentNode() != in);
        const quint64 first = f.session->currentNode();
        // l/h move along the row; the editor's current node is the cursor.
        f.type(QStringLiteral("h"));
        QCOMPARE(f.session->currentNode(), in);
        f.session->setSelection({in}, {}, in);
        f.type(QStringLiteral("9l"));
        QVERIFY(f.session->currentNode() != in);
        f.type(QStringLiteral("j"));
        QVERIFY(f.session->selectedNodes().size() == 1);
        Q_UNUSED(first);
        Q_UNUSED(out);
    }

    void wAndBWalkTheSignalAndGgGJumpToTheEnds()
    {
        CompFixture f;
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const quint64 out = f.nodeNamed(QStringLiteral("MediaOut1"));
        const quint64 blur
            = f.session->addNode(NodeRegistry::effectNodeType(QStringLiteral("gaussian_blur")));
        f.session->setSelection({in}, {}, in);
        f.type(QStringLiteral("gg"));
        QCOMPARE(f.session->currentNode(), in);
        f.type(QStringLiteral("w"));
        QCOMPARE(f.session->currentNode(), blur);
        f.type(QStringLiteral("w"));
        QCOMPARE(f.session->currentNode(), out);
        f.type(QStringLiteral("b"));
        QCOMPARE(f.session->currentNode(), blur);
        f.type(QStringLiteral("G"));
        QCOMPARE(f.session->currentNode(), out);
        f.type(QStringLiteral("1G"));
        QCOMPARE(f.session->currentNode(), in);
    }

    void gcConnectsInTwoPressesAndGxDisconnects()
    {
        CompFixture f;
        const quint64 a = f.session->addNodeAt(
            NodeRegistry::effectNodeType(QStringLiteral("gaussian_blur")), QPointF(4.0, 3.0));
        const quint64 b = f.session->addNodeAt(
            NodeRegistry::effectNodeType(QStringLiteral("gaussian_blur")), QPointF(9.0, 3.0));
        f.session->setSelection({a}, {}, a);
        const int before = f.steps();
        f.type(QStringLiteral("gc"));
        QCOMPARE(f.host.connectSource(), a);
        f.type(QStringLiteral("l")); // to the node on the right
        QCOMPARE(f.session->currentNode(), b);
        f.type(QStringLiteral("gc"));
        QCOMPARE(f.host.connectSource(), quint64(0));
        QCOMPARE(f.steps(), before + 1);
        const CompNode* target = f.comp().node(b);
        bool linked = false;
        for (const NodeLink& link : target->inputs)
            linked = linked || link.nodeId == a;
        QVERIFY(linked);
        // gx removes the input again.
        f.type(QStringLiteral("gx"));
        linked = false;
        for (const NodeLink& link : f.comp().node(b)->inputs)
            linked = linked || link.nodeId == a;
        QVERIFY(!linked);
        QCOMPARE(f.steps(), before + 2);
        // gc twice on the same node cancels.
        f.type(QStringLiteral("gcgc"));
        QCOMPARE(f.host.connectSource(), quint64(0));
        QCOMPARE(f.steps(), before + 2);
    }

    void ddGbYyPAndRInTheCompositingGraph()
    {
        CompFixture f;
        const quint64 blur
            = f.session->addNode(NodeRegistry::effectNodeType(QStringLiteral("gaussian_blur")));
        f.session->setSelection({blur}, {}, blur);
        // gb: pass-through on and off.
        f.type(QStringLiteral("gb"));
        QVERIFY(!f.comp().node(blur)->enabled);
        f.type(QStringLiteral("gb"));
        QVERIFY(f.comp().node(blur)->enabled);
        // yy p: a copy.
        const int nodes = int(f.comp().nodes.size());
        f.type(QStringLiteral("yy"));
        f.type(QStringLiteral("p"));
        QCOMPARE(int(f.comp().nodes.size()), nodes + 1);
        // r: the name field opens.
        f.session->setSelection({blur}, {}, blur);
        f.type(QStringLiteral("r"));
        QVERIFY(f.editor->renameField());
        QTest::keyClick(f.editor->renameField(), Qt::Key_Escape);
        f.editor->setFocus(Qt::OtherFocusReason);
        // dd: deleted, one undo step.
        f.session->setSelection({blur}, {}, blur);
        const int before = f.steps();
        f.type(QStringLiteral("dd"));
        QVERIFY(!f.comp().node(blur));
        QCOMPARE(f.steps(), before + 1);
    }

    void aOpensTheNodeLibraryAndZfFits()
    {
        CompFixture f;
        QSignalSpy library(
            f.actions.action(QStringLiteral("comp.select_node")), &QAction::triggered);
        f.type(QStringLiteral("a"));
        QCOMPARE(library.count(), 1);
        f.editor->setView(QPointF(40, 40), 2.0);
        f.type(QStringLiteral("zf"));
        QVERIFY(f.editor->zoom() <= 1.0);
    }

    // ---- The Color page ----

    void colorPageKeys()
    {
        QSettings().clear();
        Project project;
        SelectionModel selection;
        ActionRegistry actions;
        AppContext ctx;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        actions.action(QLatin1String(vimopts::kActionId))->setChecked(true);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        project.beginEdit(QStringLiteral("Add Clip"));
        TimelineData& tl = project.timeline()->mutableData();
        tl.width = 320;
        tl.height = 180;
        ClipData clip;
        clip.kind = ClipKind::Solid;
        clip.solidColor = Qt::darkCyan;
        clip.start = 0;
        clip.duration = 50;
        const quint64 clipId = editops::overwriteClip(tl, TrackRef {TrackType::Video, 0}, clip);
        project.endEdit();
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        MainWindow window(ctx, options);
        window.resize(1600, 900);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        actions.action("view.page_color")->trigger();
        QCoreApplication::processEvents();
        ColorSession* session = window.colorPage()->session();
        ColorNodeEditor* editor = window.colorPage()->nodeEditor();
        selection.selectClip(clipId);
        session->openClip(clipId);
        const quint64 a = session->addSerialNode();
        const quint64 b = session->addSerialNode();
        QVERIFY(a && b && a != b);
        editor->setFocus(Qt::OtherFocusReason);
        QCOMPARE(VimInput::contextOf(editor), QStringLiteral("nodes"));
        const auto type = [](const QString& keys) {
            for (const QChar c : keys) {
                QWidget* focus = QApplication::focusWidget();
                if (c.isUpper())
                    QTest::keyClick(focus, c.toLatin1(), Qt::ShiftModifier);
                else
                    QTest::keyClick(focus, c.toLatin1());
            }
        };
        session->selectNode(a);
        type(QStringLiteral("l"));
        QCOMPARE(session->selectedNode(), b);
        type(QStringLiteral("h"));
        QCOMPARE(session->selectedNode(), a);
        type(QStringLiteral("w"));
        QCOMPARE(session->selectedNode(), b);
        // o: a serial node after, P: a parallel node, L: a layer node.
        int nodes = int(session->grade().nodes.size());
        type(QStringLiteral("o"));
        QCOMPARE(int(session->grade().nodes.size()), nodes + 1);
        type(QStringLiteral("P"));
        QVERIFY(int(session->grade().nodes.size()) > nodes + 1); // a mixer and its corrector
        const int afterParallel = int(session->grade().nodes.size());
        type(QStringLiteral("L"));
        QVERIFY(int(session->grade().nodes.size()) > afterParallel);
        // gb: bypass; dd: delete.
        const quint64 current = session->selectedNode();
        type(QStringLiteral("gb"));
        QVERIFY(!session->grade().node(current)->enabled);
        type(QStringLiteral("gb"));
        QVERIFY(session->grade().node(current)->enabled);
        nodes = int(session->grade().nodes.size());
        type(QStringLiteral("dd"));
        QVERIFY(int(session->grade().nodes.size()) < nodes);
        // gc: the source node feeds the target.
        session->selectNode(a);
        type(QStringLiteral("gc"));
        session->selectNode(b);
        type(QStringLiteral("gc"));
        QVERIFY(session->grade().node(b)->inputs.contains(a));
        type(QStringLiteral("gx"));
        QVERIFY(!session->grade().node(b)->inputs.contains(a));
    }
};

QTEST_MAIN(TestVimNodes)
#include "test_vimnodes.moc"
