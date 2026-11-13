// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen tests of the Compositing page node editor (docs/behavior-spec.md 42b): mouse
// gestures (connect, disconnect, insert into a link, box select, move as one undo step), the
// session commands behind the keys (delete, copy / paste, rename, pass-through, swap) and the
// paint time of a 200-node graph.
#include "PanelTestUtils.h"
#include "core/CompOps.h"
#include "core/NodeRegistry.h"
#include "core/Serialization.h"
#include "core/Timeline.h"
#include "ui/common/Resources.h"
#include "ui/comp/CompAdapter.h"
#include "ui/comp/CompSession.h"
#include "ui/comp/NodeEditor.h"
#include "ui/comp/NodeLibrary.h"

#include <QApplication>
#include <QJsonDocument>
#include <QMenu>
#include <QElapsedTimer>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QTest>
#include <QUndoStack>

#include <ctime>
#include <future>
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

    void sideInputsFaceTheirSource()
    {
        // Spec 42b.24: inputs face their source. A side input (foreground, mask) sits on the
        // top edge and moves to the bottom when its source is clearly below the tile; the main
        // input leaves the left edge for a source to the right. Links never loop round.
        Fixture f;
        const quint64 merge = f.session->addNodeAt(nodetypes::Merge, QPointF(6, 0));
        const quint64 above = f.session->addNodeAt(nodetypes::Background, QPointF(0, -3));
        const quint64 below = f.session->addNodeAt(nodetypes::Text, QPointF(0, 3));
        const QRectF r = f.editor->tileRect(*f.node(merge));
        QCOMPARE(f.editor->inputPos(*f.node(merge), nodeports::Mask).y(), r.top()); // free
        QVERIFY(f.session->connectNodes(above, merge, nodeports::Foreground));
        QVERIFY(f.session->connectNodes(below, merge, nodeports::Mask));
        QCOMPARE(f.editor->inputSide(*f.node(merge), nodeports::Foreground), -1);
        QCOMPARE(f.editor->inputSide(*f.node(merge), nodeports::Mask), 1);
        QCOMPARE(f.editor->inputSide(*f.node(merge), nodeports::Background), 0);
        const QPointF mask = f.editor->inputPos(*f.node(merge), nodeports::Mask);
        QCOMPARE(mask.y(), r.bottom());
        QCOMPARE(f.editor->inputPos(*f.node(merge), nodeports::Foreground).y(), r.top());
        // A main input fed from above and to the right comes in across the top, next to the
        // foreground (the two spread along the edge in the order of their sources).
        QCOMPARE(f.editor->inputSide(*f.node(merge), nodeports::Background), 0);
        const quint64 right = f.session->addNodeAt(nodetypes::Background, QPointF(8, -3));
        QVERIFY(f.session->connectNodes(right, merge, nodeports::Background));
        QCOMPARE(f.editor->inputSide(*f.node(merge), nodeports::Background), -1);
        const QPointF bgPort = f.editor->inputPos(*f.node(merge), nodeports::Background);
        const QPointF fgPort = f.editor->inputPos(*f.node(merge), nodeports::Foreground);
        QCOMPARE(bgPort.y(), r.top());
        QVERIFY(std::abs(bgPort.x() - fgPort.x()) > r.width() / 4);
        QVERIFY(fgPort.x() < bgPort.x()); // ordered like their sources: no crossing links
        // The port is grabbed where it is drawn: dragging it away disconnects.
        f.drag(mask, QPointF(500, 450));
        QVERIFY(!f.node(merge)->inputs.value(nodeports::Mask).isConnected());
        QCOMPARE(f.editor->inputPos(*f.node(merge), nodeports::Mask).y(), r.top());
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

    void dropOutputOnOutputCreatesMerge()
    {
        Fixture f;
        // Background dragged onto MediaIn1's output: Merge1 (background MediaIn1, foreground
        // Background1) takes over what MediaIn1 fed. One undo step.
        const quint64 bg = f.session->addNodeAt(nodetypes::Background, QPointF(0, 3));
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const quint64 out = f.comp().mediaOutId;
        const int before = f.steps();
        const int nodes = f.comp().nodes.size();
        f.drag(f.editor->outputPos(*f.node(bg)), f.editor->outputPos(*f.node(in)));
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(f.comp().nodes.size(), nodes + 1);
        const quint64 merge = f.nodeNamed(QStringLiteral("Merge1"));
        QVERIFY(merge);
        QCOMPARE(f.node(merge)->inputs.value(nodeports::Background).nodeId, in);
        QCOMPARE(f.node(merge)->inputs.value(nodeports::Foreground).nodeId, bg);
        QCOMPARE(f.node(out)->inputs.value(nodeports::Input).nodeId, merge);
        QCOMPARE(f.session->currentNode(), merge);
        f.project.undoStack()->undo();
        QCOMPARE(f.comp().nodes.size(), nodes);
        QCOMPARE(f.node(out)->inputs.value(nodeports::Input).nodeId, in);
    }

    void dropOutputOnOwnDownstreamIsRefusedWithoutLoop()
    {
        Fixture f;
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const quint64 out = f.comp().mediaOutId;
        f.session->setSelection({in});
        const quint64 a = f.session->addNode(nodetypes::Transform);
        f.session->setSelection({a});
        const quint64 b = f.session->addNode(nodetypes::Transform);
        QVERIFY(a && b);
        const int before = f.steps();
        // MediaIn1 -> A -> B -> MediaOut: B dropped on MediaIn1's output would make A read the
        // Merge, which reads B, which reads A. Refused, nothing changes.
        f.drag(f.editor->outputPos(*f.node(b)), f.editor->outputPos(*f.node(in)));
        QCOMPARE(f.steps(), before);
        QCOMPARE(f.node(out)->inputs.value(nodeports::Input).nodeId, b);
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


    // ---- Spec 93: node editor extras ----

    void spec93_shiftDragInsertsAConnectedNode()
    {
        Fixture f;
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const quint64 out = f.comp().mediaOutId;
        const quint64 bg = f.session->addNodeAt(nodetypes::Background, QPointF(0, 4));
        const quint64 t = f.session->addNodeAt(nodetypes::Transform, QPointF(4, 4));
        f.session->connectNodes(bg, t, nodeports::Input);
        const QPointF onLink = (f.editor->outputPos(*f.node(in))
                                   + f.editor->inputPos(*f.node(out), nodeports::Input))
            / 2;
        // Without Shift a connected tile only moves.
        f.session->setSelection({t});
        f.drag(f.centreOf(t), onLink);
        QCOMPARE(f.node(out)->inputs.value(nodeports::Input).nodeId, in);
        // With Shift (the tile was already selected) it leaves its place and joins the link.
        f.session->edit(QStringLiteral("Place"), [t](Composition& c) {
            c.node(t)->position = QPointF(4, 4);
            return true;
        });
        const QPointF onLink2 = (f.editor->outputPos(*f.node(in))
                                    + f.editor->inputPos(*f.node(out), nodeports::Input))
            / 2;
        const int before = f.steps();
        f.session->setSelection({t});
        f.drag(f.centreOf(t), onLink2, Qt::ShiftModifier);
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(f.node(out)->inputs.value(nodeports::Input).nodeId, t);
        QCOMPARE(f.node(t)->inputs.value(nodeports::Input).nodeId, in);
        QVERIFY(f.comp().node(bg));
        // Undo restores the old connection of the tile in one step.
        f.project.undoStack()->undo();
        QCOMPARE(f.node(out)->inputs.value(nodeports::Input).nodeId, in);
        QCOMPARE(f.node(t)->inputs.value(nodeports::Input).nodeId, bg);
    }

    void spec93_shiftClickStillTogglesTheSelection()
    {
        Fixture f;
        const quint64 a = f.session->addNodeAt(nodetypes::Transform, QPointF(4, 4));
        const quint64 b = f.session->addNodeAt(nodetypes::Transform, QPointF(4, 6));
        f.session->setSelection({a, b}, {}, b);
        f.click(f.centreOf(b), Qt::ShiftModifier);
        QCOMPARE(f.session->selectedNodes(), QVector<quint64>({a}));
        f.click(f.centreOf(b), Qt::ShiftModifier);
        QCOMPARE(f.session->selectedNodes().size(), 2);
    }

    void spec93_moveIntoLinkBridgesAndRefusesCycles()
    {
        Composition c = compadapter::defaultComposition();
        const quint64 in = c.nodes.first().id;
        const quint64 out = c.mediaOutId;
        const quint64 a = compadapter::addNode(c, nodetypes::Transform, QPointF(2, 0));
        const quint64 b = compadapter::addNode(c, nodetypes::Transform, QPointF(4, 0));
        compadapter::connectNodes(c, in, 0, a, nodeports::Input);
        compadapter::connectNodes(c, a, 0, b, nodeports::Input);
        compadapter::connectNodes(c, b, 0, out, nodeports::Input);
        // Move b (in the middle of in -> a -> b -> out) onto the in -> a link: out now reads a.
        QVERIFY(compops::moveIntoLink(c, b, a, nodeports::Input));
        QCOMPARE(c.node(out)->inputs.value(nodeports::Input).nodeId, a);
        QCOMPARE(c.node(a)->inputs.value(nodeports::Input).nodeId, b);
        QCOMPARE(c.node(b)->inputs.value(nodeports::Input).nodeId, in);
        // A refused move changes nothing.
        const Composition before = c;
        QVERIFY(!compops::moveIntoLink(c, a, a, nodeports::Input));
        QVERIFY(c == before);
    }

    void spec93_swapInputsIsOnMerge()
    {
        Fixture f;
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const quint64 bg = f.session->addNodeAt(nodetypes::Background, QPointF(0, 3));
        const quint64 merge = f.session->addNodeAt(nodetypes::Merge, QPointF(5, 3));
        f.session->connectNodes(in, merge, nodeports::Background);
        f.session->connectNodes(bg, merge, nodeports::Foreground);
        f.session->setSelection({merge});
        QVERIFY(f.session->swapInputs());
        QCOMPARE(f.node(merge)->inputs.value(nodeports::Background).nodeId, bg);
    }

    void spec93_tilePicturesAreRenderedLazilyAndSaved()
    {
        Fixture f;
        int requests = 0;
        f.editor->setThumbnailSource([&requests](quint64, QSize px) {
            ++requests;
            auto frame = std::make_shared<RenderedFrame>();
            frame->ok = true;
            frame->image = QImage(px, QImage::Format_ARGB32_Premultiplied);
            frame->image.fill(Qt::red);
            std::promise<RenderedFramePtr> promise;
            promise.set_value(frame);
            return promise.get_future();
        });
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        // Off by default: nothing is rendered and nothing is written.
        QCOMPARE(f.editor->refreshTilePictures(), 0);
        QCOMPARE(requests, 0);
        QVERIFY(!QJsonDocument(serialization::compositionToJson(f.comp())).toJson().contains("thumb"));
        f.session->setSelection({in});
        QVERIFY(f.session->setNodeThumbnails(true));
        QVERIFY(f.node(in)->thumbnail);
        QTRY_VERIFY_WITH_TIMEOUT(!f.editor->tilePicture(in).isNull(), 3000);
        QCOMPARE(requests, 1);
        QImage target(f.editor->size(), QImage::Format_ARGB32_Premultiplied);
        f.editor->render(&target); // paints the picture on the tile
        const Composition back = serialization::compositionFromJson(serialization::compositionToJson(f.comp()));
        QVERIFY(back.node(in)->thumbnail);
        // The context menu offers the toggle, checked.
        QMenu* menu = f.editor->buildContextMenu(f.centreOf(in), f.editor.get());
        QAction* toggle = menu->findChild<QAction*>(QStringLiteral("TilePictureAction"));
        QVERIFY(toggle && toggle->isChecked());
        delete menu;
        QVERIFY(f.session->setNodeThumbnails(false));
        QVERIFY(!QJsonDocument(serialization::compositionToJson(f.comp())).toJson().contains("thumb"));
    }

    void spec93_tooltipNamesTheNodeAndItsSettings()
    {
        Fixture f;
        const quint64 t = f.session->addNodeAt(nodetypes::Transform, QPointF(4, 4));
        const QString tip = f.editor->tooltipAt(f.centreOf(t));
        QVERIFY2(tip.contains(f.node(t)->name), qPrintable(tip));
        QVERIFY2(tip.contains(compadapter::nodeType(nodetypes::Transform)->name), qPrintable(tip));
        QVERIFY(tip.contains(QLatin1Char(':'))); // at least one setting
        QVERIFY(f.editor->tooltipAt(QPointF(2, 2)).isEmpty());
    }

    void spec93_replaceToolKeepsLinksAndSharedSettings()
    {
        Fixture f;
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const quint64 out = f.comp().mediaOutId;
        const quint64 t
            = f.session->addNodeAt(nodetypes::Transform, QPointF(4, 4), out, nodeports::Input);
        QCOMPARE(f.node(t)->inputs.value(nodeports::Input).nodeId, in);
        const QStringList types = compops::replacementTypes(nodetypes::Transform);
        QVERIFY(!types.isEmpty());
        QVERIFY(compops::replacementTypes(nodetypes::MediaOut).isEmpty());
        f.session->edit(QStringLiteral("Blend"), [t](Composition& c) {
            c.node(t)->params.setBase(QStringLiteral("ccBlend"), 0.25);
            return true;
        });
        // The context menu lists them.
        QMenu* menu = f.editor->buildContextMenu(f.centreOf(t), f.editor.get());
        QVERIFY(menu->findChild<QMenu*>(QStringLiteral("ReplaceToolMenu")));
        delete menu;
        const int before = f.steps();
        QVERIFY(f.session->replaceNode(t, types.first()));
        QCOMPARE(f.steps(), before + 1);
        QCOMPARE(f.node(t)->type, types.first());
        QCOMPARE(f.node(t)->inputs.value(nodeports::Input).nodeId, in);
        QCOMPARE(f.node(out)->inputs.value(nodeports::Input).nodeId, t);
        QCOMPARE(f.node(t)->params.value(QStringLiteral("ccBlend"), 0), 0.25);
        QVERIFY(!f.session->replaceNode(t, nodetypes::MediaOut));
    }

    void spec93_findSelectsAndFramesOnlyWithEditorFocus()
    {
        Fixture f;
        const quint64 a = f.session->addNodeAt(nodetypes::Transform, QPointF(40, 30));
        f.session->addNodeAt(nodetypes::Background, QPointF(0, 6));
        QCOMPARE(f.editor->findNodes(QStringLiteral("transform")), 1);
        QCOMPARE(f.session->selectedNodes(), QVector<quint64>({a}));
        QVERIFY(f.editor->rect().contains(f.editor->tileRect(*f.node(a)).center().toPoint()));
        QCOMPARE(f.editor->findNodes(QStringLiteral("nothing like this")), 0);
        QVERIFY(f.session->selectedNodes().isEmpty());
        // Ctrl+F opens the field; typing selects; Esc closes it.
        QVERIFY(!f.editor->findField());
        QTest::keyClick(f.editor.get(), Qt::Key_F, Qt::ControlModifier);
        QVERIFY(f.editor->findField());
        QTest::keyClicks(f.editor->findField(), QStringLiteral("backg"));
        QCOMPARE(f.session->selectedNodes().size(), 1);
        QTest::keyClick(f.editor->findField(), Qt::Key_Escape);
        QVERIFY(!f.editor->findField());
        // The key is claimed through the shortcut override only for the editor itself.
        QKeyEvent claim(QEvent::ShortcutOverride, Qt::Key_F, Qt::ControlModifier);
        claim.ignore();
        QApplication::sendEvent(f.editor.get(), &claim);
        QVERIFY(claim.isAccepted());
        QKeyEvent other(QEvent::ShortcutOverride, Qt::Key_F, Qt::NoModifier);
        other.ignore();
        QApplication::sendEvent(f.editor.get(), &other);
        QVERIFY(!other.isAccepted());
    }

    void spec93_tileColourIsWrittenOnlyWhenSet()
    {
        Fixture f;
        const quint64 t = f.session->addNodeAt(nodetypes::Transform, QPointF(4, 4));
        QVERIFY(!QJsonDocument(serialization::compositionToJson(f.comp())).toJson().contains("color"));
        f.session->setSelection({t});
        QVERIFY(f.session->setNodeColor(QColor(QStringLiteral("#3a62a8"))));
        const Composition back = serialization::compositionFromJson(serialization::compositionToJson(f.comp()));
        QCOMPARE(back.node(t)->color, QColor(QStringLiteral("#3a62a8")));
        QMenu* menu = f.editor->buildContextMenu(f.centreOf(t), f.editor.get());
        QVERIFY(menu->findChild<QMenu*>(QStringLiteral("SetColorMenu")));
        delete menu;
        QImage target(f.editor->size(), QImage::Format_ARGB32_Premultiplied);
        f.editor->render(&target);
        QVERIFY(f.session->setNodeColor(QColor()));
        QVERIFY(!QJsonDocument(serialization::compositionToJson(f.comp())).toJson().contains("color"));
    }

    void spec93_pasteSettingsCopiesParamsOfTheSameType()
    {
        Fixture f;
        const quint64 a = f.session->addNodeAt(nodetypes::Transform, QPointF(4, 4));
        const quint64 b = f.session->addNodeAt(nodetypes::Transform, QPointF(4, 6));
        const quint64 m = f.session->addNodeAt(nodetypes::Merge, QPointF(8, 6));
        f.session->edit(QStringLiteral("Size"), [a](Composition& c) {
            c.node(a)->params.setBase(nodekeys::Size, 2.5);
            return true;
        });
        f.session->setSelection({a});
        f.session->copySelection();
        f.session->setSelection({m});
        QVERIFY(!f.session->canPasteSettings());
        QVERIFY(!f.session->pasteSettings());
        f.session->setSelection({b});
        QVERIFY(f.session->canPasteSettings());
        f.editor->setFocus();
        QTest::keyClick(f.editor.get(), Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(f.node(b)->params.value(nodekeys::Size, 0), 2.5);
        QCOMPARE(f.project.undoStack()->undoText(), QStringLiteral("Paste Settings"));
        QCOMPARE(f.node(b)->position, QPointF(4, 6)); // only the settings, not the place
    }

    void spec93_shiftDragOnALinkDisconnectsIt()
    {
        Fixture f;
        const quint64 in = f.nodeNamed(QStringLiteral("MediaIn1"));
        const quint64 out = f.comp().mediaOutId;
        const QPointF onLink = (f.editor->outputPos(*f.node(in))
                                   + f.editor->inputPos(*f.node(out), nodeports::Input))
            / 2;
        QCOMPARE(f.editor->hitTest(onLink).kind, NodeEditor::Hit::Kind::Link);
        // A Shift click without dragging leaves it.
        f.click(onLink, Qt::ShiftModifier);
        QVERIFY(f.node(out)->inputs.value(nodeports::Input).isConnected());
        f.drag(onLink, onLink + QPointF(0, 80), Qt::ShiftModifier);
        QVERIFY(!f.node(out)->inputs.value(nodeports::Input).isConnected());
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
