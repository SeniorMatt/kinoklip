// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the Compositing page node editor (docs/behavior-spec.md 42b): mouse
// gestures (connect, disconnect, insert into a link, box select, move as one undo step), the
// session commands behind the keys (delete, copy / paste, rename, pass-through, swap) and the
// paint time of a 200-node graph.
#include "PanelTestUtils.h"
#include "core/CompOps.h"
#include "core/NodeRegistry.h"
#include "core/Timeline.h"
#include "ui/common/Resources.h"
#include "ui/comp/CompAdapter.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/NodeEditor.h"
#include "ui/comp/NodeLibrary.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QTest>
#include <QUndoStack>

#include <ctime>
#include <memory>

using namespace kk;

namespace {

struct Fixture : test::PanelFixture
{
    std::unique_ptr<CompSession> session;
    std::unique_ptr<NodeEditor> editor;
    quint64 clipId = 0;

    Fixture()
    {
        clipId = addClip(TrackRef {TrackType::Video, 0}, 0, 100);
        session = std::make_unique<CompSession>(ctx);
        session->openClip(clipId);
        editor = std::make_unique<NodeEditor>(session.get(), &actions);
        editor->resize(1000, 500);
        editor->show();
        editor->setView(QPointF(4, 0.5), 1.0);
    }

    const Composition& comp() const { return session->composition(); }
    int steps() const { return project.undoStack()->count(); }
    quint64 nodeNamed(const QString& name) const
    {
        for (const CompNode& n : comp().nodes)
            if (n.name == name)
                return n.id;
        return 0;
    }
    const CompNode* node(quint64 id) const { return comp().node(id); }
    QPointF centreOf(quint64 id) const { return editor->tileRect(*node(id)).center(); }

    void send(QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
        Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        QMouseEvent e(type, pos, editor->mapToGlobal(pos), button, buttons, mods);
        QApplication::sendEvent(editor.get(), &e);
    }
    /// Left drag from @p from to @p to in steps.
    void drag(QPointF from, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        send(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, mods);
        for (int i = 1; i <= 5; ++i)
            send(QEvent::MouseMove, from + (to - from) * (i / 5.0), Qt::NoButton, Qt::LeftButton,
                mods);
        send(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, mods);
    }
    void click(QPointF at, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        send(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, mods);
        send(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, mods);
    }
};

} // namespace

class TestNodeEditor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { initResources(); }

    void viewingDoesNotStore()
    {
        Fixture f;
        QVERIFY(!f.session->isStored());
        QCOMPARE(f.comp().nodes.size(), 2); // MediaIn1 -> MediaOut1 shown
        QCOMPARE(f.steps(), 1); // only the clip
        QVERIFY(!f.clip(f.clipId)->composition);
    }

    void addNodeAfterSelectionIsOneStep()
    {
        Fixture f;
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.session->setSelection({in});
        const int before = f.steps();
        const quint64 blur
            = f.session->addNode(NodeRegistry::effectNodeType(QStringLiteral("gaussian_blur")));
        QVERIFY(blur);
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(f.clip(f.clipId)->composition); // created on the first edit
        QCOMPARE(f.node(blur)->inputs.value(nodeports::Input).nodeId, in);
        QCOMPARE(f.session->currentNode(), blur);
        f.project.undo();
        QVERIFY(!f.clip(f.clipId)->composition);
        QCOMPARE(f.comp().nodes.size(), 2);
    }

    void selectNodePopupAddsAfterSelected()
    {
        Fixture f;
        f.session->setSelection({f.nodeNamed(QStringLiteral("MediaIn1"))});
        SelectNodePopup popup(f.session.get());
        popup.popup(QPoint(100, 100));
        QTest::keyClicks(popup.searchField(), QStringLiteral("transf"));
        QVERIFY(popup.list()->count() >= 1);
        QCOMPARE(
            popup.list()->currentItem()->data(Qt::UserRole + 1).toString(), nodetypes::Transform);
        QTest::keyClick(popup.searchField(), Qt::Key_Return);
        QVERIFY(!popup.isVisible());
        const quint64 t = f.nodeNamed(QStringLiteral("Transform1"));
        QVERIFY(t);
        QVERIFY(f.node(t)->inputs.value(nodeports::Input).isConnected());
        QVERIFY(SelectNodePopup::matches(QStringLiteral("merge")).first() == nodetypes::Merge);
    }

    void connectByDraggingFromOutput()
    {
        Fixture f;
        const quint64 bg = f.session->addNodeAt(nodetypes::Background, QPointF(0, 3));
        const quint64 merge = f.session->addNodeAt(nodetypes::Merge, QPointF(5, 3));
        const int before = f.steps();
        f.drag(f.editor->outputPos(*f.node(bg)),
            f.editor->inputPos(*f.node(merge), nodeports::Foreground));
        QCOMPARE(f.node(merge)->inputs.value(nodeports::Foreground).nodeId, bg);
        QCOMPARE(f.steps(), before + 1);
        // Dropping on a tile takes its main input when free.
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.drag(f.editor->outputPos(*f.node(in)), f.centreOf(merge));
        QCOMPARE(f.node(merge)->inputs.value(nodeports::Background).nodeId, in);
    }

    void dragLinkEndAwayDisconnects()
    {
        Fixture f;
        const quint64 out = f.comp().mediaOutId;
        QVERIFY(f.node(out)->inputs.value(nodeports::Input).isConnected());
        const int before = f.steps();
        f.drag(f.editor->inputPos(*f.node(out), nodeports::Input), QPointF(500, 450));
        QVERIFY(!f.node(out)->inputs.value(nodeports::Input).isConnected());
        QCOMPARE(f.steps(), before + 1);
        f.project.undo();
        QVERIFY(!f.clip(f.clipId)->composition); // back to "no composition"
    }

    void dropNodeOnLinkInserts()
    {
        Fixture f;
        // A Transform below the MediaIn -> MediaOut link, dragged onto it.
        const quint64 t = f.session->addNodeAt(nodetypes::Transform, QPointF(4, 4));
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const quint64 out = f.comp().mediaOutId;
        const QPointF a = f.editor->outputPos(*f.node(in));
        const QPointF b = f.editor->inputPos(*f.node(out), nodeports::Input);
        const QPointF onLink = (a + b) / 2;
        QCOMPARE(f.editor->hitTest(onLink).kind, NodeEditor::Hit::Kind::Link);
        const int before = f.steps();
        f.drag(f.centreOf(t), onLink);
        QCOMPARE(f.steps(), before + 1); // move + insert: one step
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Insert Node"));
        QCOMPARE(f.node(out)->inputs.value(nodeports::Input).nodeId, t);
        QCOMPARE(f.node(t)->inputs.value(nodeports::Input).nodeId, in);
    }

    void boxSelectAndMoveIsOneStep()
    {
        Fixture f;
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const quint64 out = f.comp().mediaOutId;
        const QRectF a = f.editor->tileRect(*f.node(in));
        const QRectF b = f.editor->tileRect(*f.node(out));
        f.drag(a.topLeft() - QPointF(20, 20), b.bottomRight() + QPointF(20, 20));
        QCOMPARE(f.session->selectedNodes().size(), 2);
        // Ctrl+click toggles one out.
        f.click(a.center(), Qt::ControlModifier);
        QCOMPARE(f.session->selectedNodes(), QVector<quint64> {out});
        f.click(a.center(), Qt::ShiftModifier);
        QCOMPARE(f.session->selectedNodes().size(), 2);

        const QPointF inPos = f.node(in)->position, outPos = f.node(out)->position;
        const int before = f.steps();
        f.drag(a.center(), a.center() + QPointF(NodeEditor::kUnitPx * 2, NodeEditor::kUnitPx));
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Move Nodes"));
        QCOMPARE(f.node(in)->position, inPos + QPointF(2, 1));
        QCOMPARE(f.node(out)->position, outPos + QPointF(2, 1));
    }

    void deleteCopyPasteDuplicate()
    {
        Fixture f;
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.session->setSelection({in});
        const quint64 blur
            = f.session->addNode(NodeRegistry::effectNodeType(QStringLiteral("gaussian_blur")));
        f.session->setSelection({blur});
        f.session->copySelection();
        QVERIFY(CompSession::canPaste());
        int before = f.steps();
        QVERIFY(f.session->paste(QPointF(2, 4)));
        QCOMPARE(f.steps(), before + 1);
        QVERIFY(f.nodeNamed(QStringLiteral("GaussianBlur2")) || f.comp().nodes.size() == 4);
        QCOMPARE(f.comp().nodes.size(), 4);
        QVERIFY(f.session->duplicateSelection());
        QCOMPARE(f.comp().nodes.size(), 5);
        before = f.steps();
        QVERIFY(f.session->deleteSelection());
        QCOMPARE(f.comp().nodes.size(), 4);
        QCOMPARE(f.steps(), before + 1);
        // MediaOut is never deleted.
        f.session->setSelection({f.comp().mediaOutId});
        QVERIFY(!f.session->deleteSelection());
    }

    void renameInline()
    {
        Fixture f;
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        f.editor->startRename(in);
        QVERIFY(f.editor->renameField());
        f.editor->renameField()->setText(QStringLiteral("Plate"));
        QTest::keyClick(f.editor->renameField(), Qt::Key_Return);
        QCOMPARE(f.node(in)->name, QStringLiteral("Plate"));
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Rename Node"));
        // Esc cancels.
        f.editor->startRename(in);
        f.editor->renameField()->setText(QStringLiteral("Other"));
        QTest::keyClick(f.editor->renameField(), Qt::Key_Escape);
        QCOMPARE(f.node(in)->name, QStringLiteral("Plate"));
    }

    void passThroughLockAndSwap()
    {
        Fixture f;
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const quint64 bg = f.session->addNodeAt(nodetypes::Background, QPointF(0, 3));
        const quint64 merge = f.session->addNodeAt(nodetypes::Merge, QPointF(5, 3));
        f.session->connectNodes(in, merge, nodeports::Background);
        f.session->connectNodes(bg, merge, nodeports::Foreground);
        f.session->setSelection({merge});
        QVERIFY(f.session->togglePassThrough());
        QVERIFY(!f.node(merge)->enabled);
        QVERIFY(f.session->togglePassThrough());
        QVERIFY(f.node(merge)->enabled);
        QVERIFY(f.session->swapInputs());
        QCOMPARE(f.node(merge)->inputs.value(nodeports::Background).nodeId, bg);
        QCOMPARE(f.node(merge)->inputs.value(nodeports::Foreground).nodeId, in);
        QVERIFY(f.session->toggleLock());
        QVERIFY(f.node(merge)->locked);
        f.editor->repaint(); // locked / disabled looks paint without trouble
    }

    void panAndZoom()
    {
        Fixture f;
        const QPointF anchor(300, 200);
        const QPointF before = f.editor->toGrid(anchor);
        f.editor->zoomAt(anchor, 2.0);
        QCOMPARE(f.editor->zoom(), 2.0);
        QVERIFY((f.editor->toGrid(anchor) - before).manhattanLength() < 1e-9);
        const QPointF centre = f.editor->centre();
        f.send(QEvent::MouseButtonPress, QPointF(500, 250), Qt::MiddleButton, Qt::MiddleButton);
        f.send(QEvent::MouseMove, QPointF(560, 250), Qt::NoButton, Qt::MiddleButton);
        f.send(QEvent::MouseButtonRelease, QPointF(560, 250), Qt::MiddleButton, Qt::NoButton);
        QVERIFY(f.editor->centre().x() < centre.x());
        f.editor->fitToView();
        for (const CompNode& n : f.comp().nodes)
            QVERIFY(f.editor->rect().contains(f.editor->tileRect(n).toRect()));
    }

    void notes()
    {
        Fixture f;
        const quint64 note = f.session->addNote(QPointF(0, 3), QStringLiteral("Key the plate"));
        QVERIFY(note);
        QCOMPARE(f.comp().notes.size(), 1);
        QVERIFY(f.session->setNoteText(note, QStringLiteral("Done")));
        QCOMPARE(f.comp().notes.first().text, QStringLiteral("Done"));
        QCOMPARE(f.session->selectedNotes(), QVector<quint64> {note});
        QVERIFY(f.session->deleteSelection());
        QVERIFY(f.comp().notes.isEmpty());
    }

    void twoHundredNodesPaintFast()
    {
        Fixture f;
        f.session->edit(QStringLiteral("Big Graph"), [](Composition& c) {
            quint64 prev = c.nodes.first().id;
            for (int i = 0; i < 200; ++i) {
                const quint64 id = compadapter::addNode(c,
                    i % 2 ? nodetypes::Transform
                          : NodeRegistry::effectNodeType(QStringLiteral("gaussian_blur")),
                    QPointF((i % 20) * 4.0, (i / 20) * 2.0));
                compadapter::connectNodes(c, prev, 0, id, nodeports::Input);
                prev = id;
            }
            return true;
        });
        QCOMPARE(f.comp().nodes.size(), 202);
        f.editor->resize(1600, 900);
        f.editor->fitToView();
        // Thread CPU time, not wall time: the test machine may be busy with other builds.
        auto cpuMs = [] {
            timespec t {};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
            return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
        };
        QImage target(f.editor->size(), QImage::Format_ARGB32_Premultiplied);
        QVector<double> times;
        for (int i = 0; i < 10; ++i) {
            const double start = cpuMs();
            f.editor->render(&target); // runs paintEvent() whether or not on screen
            times.append(cpuMs() - start);
        }
        std::sort(times.begin(), times.end());
        qInfo("200 nodes (all visible, zoom %.2f): paint CPU median %.2f ms, max %.2f ms",
            f.editor->zoom(), times[5], times.last());
        QVERIFY(f.editor->lastPaintMs() > 0.0);
        // Generous bound for slow CI machines (an idle laptop core needs about 15 ms).
        QVERIFY2(times[5] < 60.0, qPrintable(QString::number(times[5])));
        // A hit test over the whole graph stays cheap too.
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 100; ++i)
            (void)f.editor->hitTest(QPointF(i * 13 % 1600, i * 7 % 900));
        qInfo("200 nodes: 100 hit tests %.2f ms", timer.nsecsElapsed() / 1e6);
    }
};

QTEST_MAIN(TestNodeEditor)
#include "test_nodeeditor.moc"
