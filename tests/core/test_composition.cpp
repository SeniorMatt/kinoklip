// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the Compositing page core (spec section 42a): the node registry, CompOps,
// the composition in ClipData (edit operations, serialization format 15, migration from
// format 14) and the undoable Project::setClipComposition().
#include "CoreTestUtils.h"
#include "core/CompOps.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/MediaUsage.h"
#include "core/NodeRegistry.h"
#include "core/Project.h"
#include "core/Serialization.h"
#include "core/Timeline.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

using namespace kk;
using namespace kk::test;
namespace co = kk::compops;

namespace {

/// Id of the first node of type @p type, or 0.
quint64 nodeOfType(const Composition& c, const QString& type)
{
    for (const CompNode& n : c.nodes) {
        if (n.type == type)
            return n.id;
    }
    return 0;
}

quint64 mediaInOf(const Composition& c)
{
    return nodeOfType(c, nodetypes::MediaIn);
}

/// The node feeding input @p port of @p node (0 = none).
quint64 source(const Composition& c, quint64 node, const QString& port)
{
    const CompNode* n = c.node(node);
    return n ? n->inputs.value(port).nodeId : 0;
}

/// A composition with every kind of content: Background + Text merged over MediaIn,
/// a keyframed blur with a mask, a note and viewer settings.
Composition richComposition()
{
    Composition c = co::defaultComposition();
    const quint64 in = mediaInOf(c);
    const co::AddResult bg = co::addNodeAfter(c, nodetypes::Background, in);
    const co::AddResult text = co::addNodeAfter(c, nodetypes::Text, bg.merge);
    const co::AddResult blur = co::addNodeAfter(
        c, NodeRegistry::effectNodeType(QStringLiteral("gaussian_blur")), text.merge);
    const quint64 rect = co::addNodeAfter(c, nodetypes::RectangleMask, blur.node).node;
    Q_UNUSED(rect);
    co::addNode(c, nodetypes::PolygonMask, QPointF(3, 3));
    CompNode* b = c.node(blur.node);
    b->params.values[QStringLiteral("radius")].setKey(0, 0.0);
    b->params.values[QStringLiteral("radius")].setKey(20, 30.0);
    c.node(text.node)->params.setStatic(nodekeys::Text, QStringLiteral("Hello"));
    c.node(bg.node)->params.setStatic(nodekeys::Color, QColor(10, 20, 30, 200));
    CompNote note;
    note.id = c.allocateId();
    note.position = QPointF(-1, 2);
    note.size = QSizeF(4, 2);
    note.text = QStringLiteral("a note");
    note.color = QColor(200, 180, 0);
    c.notes.append(note);
    c.viewCentre = QPointF(2.5, -1);
    c.viewZoom = 1.5;
    co::setViewerNodes(c, blur.node, 0);
    return c;
}

} // namespace

class TestComposition : public QObject
{
    Q_OBJECT
private slots:
    // ---- Registry ----

    void registryHasBuiltins()
    {
        const NodeRegistry& r = NodeRegistry::instance();
        for (const QString& id : {nodetypes::MediaIn, nodetypes::MediaOut, nodetypes::Merge,
                 nodetypes::Dissolve, nodetypes::Transform, nodetypes::Crop, nodetypes::Resize,
                 nodetypes::Letterbox, nodetypes::Background, nodetypes::Text, nodetypes::FastNoise,
                 nodetypes::RectangleMask, nodetypes::EllipseMask, nodetypes::PolygonMask,
                 nodetypes::ColorCorrector, nodetypes::ChannelBooleans})
            QVERIFY2(r.find(id), qPrintable(id));
        const NodeDescriptor* merge = r.find(nodetypes::Merge);
        QCOMPARE(merge->mainInput, nodeports::Background);
        QCOMPARE(merge->input(nodeports::Foreground)->kind, PortKind::Image);
        QCOMPARE(merge->input(nodeports::Mask)->kind, PortKind::Mask);
        QCOMPARE(merge->param(nodekeys::BlendMode)->choices.size(), 9);
        QCOMPARE(merge->controls, ViewerControls::Transform);
        QCOMPARE(r.find(nodetypes::MediaOut)->outputs, 0);
        QCOMPARE(r.find(nodetypes::EllipseMask)->outputKind, PortKind::Mask);
        QCOMPARE(r.find(nodetypes::PolygonMask)->controls, ViewerControls::Polygon);
        QCOMPARE(r.find(nodetypes::RectangleMask)->controls, ViewerControls::Rect);
        QVERIFY(r.find(nodetypes::Background)->isSource());
        QVERIFY(!r.find(nodetypes::Transform)->isSource());
        QVERIFY(r.find(QStringLiteral("nope")) == nullptr);
        // Defaults come from the parameter descriptors.
        const ParamSet p = r.defaultParams(nodetypes::Transform);
        QCOMPARE(p.value(nodekeys::CenterX, 0), 0.5);
        QCOMPARE(p.value(nodekeys::Size, 0), 1.0);
        QCOMPARE(p.staticValue(nodekeys::FlipH).toBool(), false);
        QVERIFY(!NodeRegistry::categoryName(NodeCategory::Mask).isEmpty());
        QCOMPARE(blendmodes::index(blendmodes::Subtract), 8);
        QCOMPARE(blendmodes::index(QStringLiteral("bogus")), 0);
    }

    void everyEffectIsANode()
    {
        const auto effects = EffectRegistry::instance().byCategory(EffectCategory::VideoEffect);
        QVERIFY(effects.size() >= 60);
        const NodeRegistry& r = NodeRegistry::instance();
        QCOMPARE(r.byCategory(NodeCategory::Effect).size(), effects.size());
        for (const EffectDescriptor* e : effects) {
            const NodeDescriptor* d = r.find(QStringLiteral("fx:") + e->id);
            QVERIFY2(d, qPrintable(e->id));
            QCOMPARE(d->effectId, e->id);
            QCOMPARE(d->name, e->name);
            QCOMPARE(d->group, e->group);
            QCOMPARE(d->mainInput, nodeports::Input);
            QVERIFY(d->input(nodeports::Mask));
            QCOMPARE(d->params.size(), e->params.size());
            QVERIFY(!d->shortName.isEmpty() && d->shortName.size() <= 4);
            QVERIFY(!d->nameBase.contains(QLatin1Char(' ')));
        }
    }

    // ---- CompOps ----

    void defaultCompositionPassesThrough()
    {
        Composition c = co::defaultComposition();
        QCOMPARE(c.nodes.size(), 2);
        const quint64 in = mediaInOf(c);
        QCOMPARE(c.node(in)->name, QStringLiteral("MediaIn1"));
        QCOMPARE(c.node(c.mediaOutId)->name, QStringLiteral("MediaOut1"));
        QCOMPARE(source(c, c.mediaOutId, nodeports::Input), in);
        QVERIFY(co::isPassThrough(c));
        QCOMPARE(co::evaluationOrder(c, c.mediaOutId), (QVector<quint64> {in, c.mediaOutId}));
        // A disabled node in between still passes through; an enabled one does not.
        const quint64 blur = co::addNodeAfter(c, QStringLiteral("fx:gaussian_blur"), in).node;
        QVERIFY(!co::isPassThrough(c));
        co::setEnabled(c, blur, false);
        QVERIFY(co::isPassThrough(c));
        c.node(in)->mediaId = QStringLiteral("other");
        QVERIFY(!co::isPassThrough(c));
    }

    void uniqueNames()
    {
        Composition c = co::defaultComposition();
        const quint64 a = co::addNode(c, nodetypes::Merge, {});
        const quint64 b = co::addNode(c, nodetypes::Merge, {});
        QCOMPARE(c.node(a)->name, QStringLiteral("Merge1"));
        QCOMPARE(c.node(b)->name, QStringLiteral("Merge2"));
        co::removeNodes(c, {a});
        QCOMPARE(c.node(co::addNode(c, nodetypes::Merge, {}))->name, QStringLiteral("Merge1"));
        QCOMPARE(co::addNode(c, QStringLiteral("unknown"), {}), quint64(0));
        // Ids are never reused.
        QVERIFY(c.nextId > b);
        QVERIFY(!co::renameNode(c, b, QStringLiteral("Merge1"))); // taken
        QVERIFY(!co::renameNode(c, b, QStringLiteral("  "))); // empty
        QVERIFY(co::renameNode(c, b, QStringLiteral(" Comp ")));
        QCOMPARE(c.node(b)->name, QStringLiteral("Comp"));
    }

    void addAfterInsertsIntoChain()
    {
        Composition c = co::defaultComposition();
        const quint64 in = mediaInOf(c);
        const co::AddResult blur = co::addNodeAfter(c, QStringLiteral("fx:gaussian_blur"), in);
        QCOMPARE(blur.merge, quint64(0));
        QCOMPARE(source(c, blur.node, nodeports::Input), in);
        QCOMPARE(source(c, c.mediaOutId, nodeports::Input), blur.node);
        // Placed right of the selected node; MediaOut was pushed further right.
        QVERIFY(c.node(blur.node)->position.x() > c.node(in)->position.x());
        QVERIFY(c.node(c.mediaOutId)->position.x() > c.node(blur.node)->position.x());
        // Selecting MediaOut adds before it.
        const quint64 cc = co::addNodeAfter(c, nodetypes::ColorCorrector, c.mediaOutId).node;
        QCOMPARE(source(c, cc, nodeports::Input), blur.node);
        QCOMPARE(source(c, c.mediaOutId, nodeports::Input), cc);
        // No selection: unconnected.
        const quint64 lone = co::addNodeAfter(c, nodetypes::Transform, 0).node;
        QVERIFY(lone != 0);
        QVERIFY(c.node(lone)->inputs.isEmpty());
    }

    void addGeneratorAutoMerges()
    {
        Composition c = co::defaultComposition();
        const quint64 in = mediaInOf(c);
        const co::AddResult text = co::addNodeAfter(c, nodetypes::Text, in);
        QVERIFY(text.merge != 0);
        QCOMPARE(c.node(text.merge)->type, nodetypes::Merge);
        QCOMPARE(source(c, text.merge, nodeports::Background), in);
        QCOMPARE(source(c, text.merge, nodeports::Foreground), text.node);
        QCOMPARE(source(c, c.mediaOutId, nodeports::Input), text.merge);
        QVERIFY(c.node(text.node)->inputs.isEmpty());
        // A mask goes into the selected node's free mask port.
        const quint64 mask = co::addNodeAfter(c, nodetypes::EllipseMask, text.merge).node;
        QCOMPARE(source(c, text.merge, nodeports::Mask), mask);
        // ... and not into a taken one.
        const quint64 mask2 = co::addNodeAfter(c, nodetypes::RectangleMask, text.merge).node;
        QCOMPARE(source(c, text.merge, nodeports::Mask), mask);
        QVERIFY(c.node(mask2)->inputs.isEmpty());
        // A polygon starts with a closed default shape.
        const quint64 poly = co::addNode(c, nodetypes::PolygonMask, {});
        QCOMPARE(c.node(poly)->path.pointCount(), 5);
    }

    void connectRefusesCyclesAndKinds()
    {
        Composition c = co::defaultComposition();
        const quint64 in = mediaInOf(c);
        const quint64 a = co::addNodeAfter(c, nodetypes::Transform, in).node;
        const quint64 b = co::addNodeAfter(c, nodetypes::ColorCorrector, a).node;
        const quint64 mask = co::addNode(c, nodetypes::RectangleMask, {});
        const quint64 merge = co::addNode(c, nodetypes::Merge, {});
        QCOMPARE(co::connect(c, b, 0, a, nodeports::Input), co::ConnectError::Cycle);
        QCOMPARE(co::connect(c, a, 0, a, nodeports::Mask), co::ConnectError::Cycle);
        QCOMPARE(
            co::connect(c, mask, 0, merge, nodeports::Foreground), co::ConnectError::WrongKind);
        QCOMPARE(
            co::connect(c, mask, 0, merge, QStringLiteral("nope")), co::ConnectError::UnknownPort);
        QCOMPARE(co::connect(c, c.mediaOutId, 0, merge, nodeports::Foreground),
            co::ConnectError::UnknownPort); // MediaOut has no output
        QCOMPARE(
            co::connect(c, 999, 0, merge, nodeports::Foreground), co::ConnectError::UnknownNode);
        // Images may feed a mask port (their alpha is the matte), masks a mask port.
        QCOMPARE(co::connect(c, b, 0, merge, nodeports::Mask), co::ConnectError::None);
        QCOMPARE(co::connect(c, mask, 0, merge, nodeports::Mask), co::ConnectError::None);
        QCOMPARE(source(c, merge, nodeports::Mask), mask); // replaced
        QVERIFY(co::dependsOn(c, b, in));
        QVERIFY(!co::dependsOn(c, in, b));
        QVERIFY(co::disconnect(c, merge, nodeports::Mask));
        QVERIFY(!co::disconnect(c, merge, nodeports::Mask));
    }

    void removeKeepsChain()
    {
        Composition c = co::defaultComposition();
        const quint64 in = mediaInOf(c);
        const quint64 a = co::addNodeAfter(c, nodetypes::Transform, in).node;
        const quint64 b = co::addNodeAfter(c, nodetypes::ColorCorrector, a).node;
        const quint64 mask = co::addNodeAfter(c, nodetypes::EllipseMask, b).node;
        co::setViewerNodes(c, a, b);
        QCOMPARE(co::removeNodes(c, {a, b, c.mediaOutId}), 2); // MediaOut stays
        QCOMPARE(source(c, c.mediaOutId, nodeports::Input), in);
        QCOMPARE(c.leftViewerNode, quint64(0));
        QCOMPARE(c.rightViewerNode, quint64(0));
        QVERIFY(c.node(mask)); // the mask node itself is kept (its consumer was removed)
        // Removing a mask node: the masked node's mask port is simply disconnected.
        const quint64 x = co::addNodeAfter(c, nodetypes::Transform, in).node;
        co::connect(c, mask, 0, x, nodeports::Mask);
        co::removeNodes(c, {mask});
        QVERIFY(!c.node(x)->inputs.contains(nodeports::Mask));
        QCOMPARE(source(c, c.mediaOutId, nodeports::Input), x);
    }

    void insertIntoLink()
    {
        Composition c = co::defaultComposition();
        const quint64 in = mediaInOf(c);
        const quint64 t = co::addNode(c, nodetypes::Transform, {});
        QVERIFY(co::insertIntoLink(c, t, c.mediaOutId, nodeports::Input));
        QCOMPARE(source(c, t, nodeports::Input), in);
        QCOMPARE(source(c, c.mediaOutId, nodeports::Input), t);
        // A generator has no main input; a mask's output cannot feed an image port.
        const quint64 bg = co::addNode(c, nodetypes::Background, {});
        QVERIFY(!co::insertIntoLink(c, bg, c.mediaOutId, nodeports::Input));
        const quint64 mask = co::addNode(c, nodetypes::RectangleMask, {});
        QVERIFY(!co::insertIntoLink(c, mask, c.mediaOutId, nodeports::Input));
        // Not into its own input link, not when it makes a cycle, not into a free port.
        QVERIFY(!co::insertIntoLink(c, t, t, nodeports::Input));
        QVERIFY(!co::insertIntoLink(c, in, t, nodeports::Input));
        QVERIFY(!co::insertIntoLink(c, t, c.mediaOutId, QStringLiteral("nope")));
    }

    void moveAndEnable()
    {
        Composition c = co::defaultComposition();
        const quint64 in = mediaInOf(c);
        const QPointF before = c.node(in)->position;
        co::moveNodes(c, {in}, QPointF(1, -2));
        QCOMPARE(c.node(in)->position, before + QPointF(1, -2));
        QVERIFY(co::setEnabled(c, in, false));
        QVERIFY(!c.node(in)->enabled);
        QVERIFY(!co::setEnabled(c, 999, false));
        co::setViewerNodes(c, in, 999);
        QCOMPARE(c.leftViewerNode, in);
        QCOMPARE(c.rightViewerNode, quint64(0));
    }

    void copyPasteRemapsIds()
    {
        Composition c = co::defaultComposition();
        const quint64 in = mediaInOf(c);
        const co::AddResult text = co::addNodeAfter(c, nodetypes::Text, in);
        const QVector<CompNode> clip = co::copyNodes(c, {text.node, text.merge, c.mediaOutId});
        QCOMPARE(clip.size(), 2); // MediaOut is never copied
        // Through the clipboard JSON, like the page does.
        const QVector<CompNode> fromJson
            = serialization::compNodesFromJson(serialization::compNodesToJson(clip));
        QCOMPARE(fromJson, clip);
        const qsizetype count = c.nodes.size();
        const QVector<quint64> pasted = co::pasteNodes(c, fromJson, QPointF(0, 3));
        QCOMPARE(pasted.size(), 2);
        QCOMPARE(c.nodes.size(), count + 2);
        // copyNodes() keeps the composition's order (the Merge was created first).
        const bool textFirst = c.node(pasted[0])->type == nodetypes::Text;
        const CompNode* newText = c.node(pasted[textFirst ? 0 : 1]);
        const CompNode* newMerge = c.node(pasted[textFirst ? 1 : 0]);
        QVERIFY(newText->id != text.node && newMerge->id != text.merge);
        QCOMPARE(newText->name, QStringLiteral("Text2"));
        QCOMPARE(newMerge->name, QStringLiteral("Merge2"));
        QCOMPARE(newText->position, c.node(text.node)->position + QPointF(0, 3));
        // The internal link is remapped, the link to MediaIn (outside the set) dropped.
        QCOMPARE(source(c, newMerge->id, nodeports::Foreground), newText->id);
        QVERIFY(!newMerge->inputs.contains(nodeports::Background));
        // Pasting into another composition keeps the names when they are free.
        Composition other = co::defaultComposition();
        const QVector<quint64> p2 = co::pasteNodes(other, clip, {});
        QCOMPARE(other.node(p2[textFirst ? 0 : 1])->name, QStringLiteral("Text1"));
    }

    void evaluationOrderIsTopological()
    {
        Composition c = richComposition();
        const QVector<quint64> order = co::evaluationOrder(c, c.mediaOutId);
        QCOMPARE(order.last(), c.mediaOutId);
        QCOMPARE(order.first(), mediaInOf(c));
        for (int i = 0; i < order.size(); ++i) {
            for (const NodeLink& l : c.node(order[i])->inputs)
                QVERIFY(order.indexOf(l.nodeId) < i);
        }
        // The unconnected polygon is not part of the pull.
        QVERIFY(!order.contains(nodeOfType(c, nodetypes::PolygonMask)));
        QVERIFY(co::evaluationOrder(c, 999).isEmpty());
        // A cycle in damaged data does not hang.
        c.node(mediaInOf(c))->inputs.insert(nodeports::Input, NodeLink {c.mediaOutId, 0});
        QVERIFY(!co::evaluationOrder(c, c.mediaOutId).isEmpty());
    }

    // ---- Model, edit operations, serialization ----

    void serializationRoundTrip()
    {
        const Composition c = richComposition();
        const QJsonObject json = serialization::compositionToJson(c);
        QCOMPARE(serialization::compositionFromJson(json), c);
        // pictureOnly leaves out names, positions, notes and the view.
        const QJsonObject picture = serialization::compositionToJson(c, true);
        QVERIFY(!picture.contains(QLatin1String("notes")));
        QVERIFY(!picture.contains(QLatin1String("view")));
        Composition moved = c;
        co::moveNodes(moved, {mediaInOf(moved)}, QPointF(5, 5));
        moved.viewZoom = 3;
        QCOMPARE(serialization::compositionToJson(moved, true), picture);

        // Through the clip JSON (copy / paste of clips, the project file).
        ClipData clip = mediaClip(0, 50);
        clip.composition = c;
        QCOMPARE(serialization::clipFromJson(serialization::clipToJson(clip)), clip);
        ClipData plain = mediaClip(0, 50);
        QVERIFY(!serialization::clipToJson(plain).contains(QLatin1String("composition")));
        QVERIFY(!serialization::clipFromJson(serialization::clipToJson(plain)).composition);
    }

    void damagedJsonIsRepaired()
    {
        const QByteArray text = R"({"nodes": [
            {"id": 1, "type": "media_in"},
            {"id": 1, "type": "merge"},
            {"id": 0, "type": "merge"},
            {"id": 5, "type": "media_out", "inputs": {"input": [1, 0]}},
            {"id": 6, "type": "transform", "inputs": {"input": [42, 0]}}],
            "mediaOut": 77, "nextId": 2, "view": {"left": 99, "zoom": -1}})";
        const Composition c
            = serialization::compositionFromJson(QJsonDocument::fromJson(text).object());
        QCOMPARE(c.nodes.size(), 3);
        QCOMPARE(c.mediaOutId, quint64(5)); // repaired to the MediaOut node
        QVERIFY(c.nextId > 6);
        QVERIFY(c.node(6)->inputs.isEmpty()); // link to a missing node dropped
        QCOMPARE(c.leftViewerNode, quint64(0));
        QCOMPARE(c.viewZoom, 1.0);
    }

    void splitAndTrimKeepComposition()
    {
        TimelineData d = timeline();
        ClipData clip = mediaClip(10, 100);
        clip.composition = richComposition();
        const quint64 id = editops::overwriteClip(d, kV1, clip);
        QVERIFY(id);
        QVERIFY(!editops::splitAt(d, 50, editops::unlockedTracks()).isEmpty());
        KK_VERIFY_INVARIANTS(d);
        const TrackData& track = *d.track(kV1);
        QCOMPARE(track.clips.size(), size_t(2));
        QVERIFY(track.clips[0].composition && track.clips[1].composition);
        // The keyframes are clip-local: the right part's keys moved by the split offset (40).
        const quint64 blur
            = nodeOfType(*track.clips[1].composition, QStringLiteral("fx:gaussian_blur"));
        const AnimatedValue& v
            = track.clips[1].composition->node(blur)->params.values[QStringLiteral("radius")];
        QCOMPARE(v.keys.first().frame, int64_t(-40));
        QCOMPARE(v.keys.last().frame, int64_t(-20));
        const quint64 blurLeft
            = nodeOfType(*track.clips[0].composition, QStringLiteral("fx:gaussian_blur"));
        QCOMPARE(track.clips[0]
                     .composition->node(blurLeft)
                     ->params.values[QStringLiteral("radius")]
                     .keys.first()
                     .frame,
            int64_t(0));
    }

    void setCompositionEditOp()
    {
        TimelineData d = timeline();
        const quint64 id = editops::overwriteClip(d, kV1, mediaClip(0, 10));
        QVERIFY(editops::setClipComposition(d, id, co::defaultComposition()));
        QVERIFY(editops::clipById(d, id)->composition);
        QVERIFY(!editops::setClipComposition(d, 999, co::defaultComposition()));
        d.track(kV1)->locked = true;
        QVERIFY(!editops::setClipComposition(d, id, std::nullopt));
        QVERIFY(editops::clipById(d, id)->composition);
    }

    void projectUndo()
    {
        Project p;
        const quint64 uid = p.activeTimelineUid();
        p.beginEdit(QStringLiteral("Add"));
        const quint64 id
            = editops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 50));
        p.endEdit();
        const int steps = p.undoStack()->count();
        QVERIFY(!p.clipComposition(uid, id));

        Composition c = co::defaultComposition();
        QVERIFY(p.setClipComposition(uid, id, c, QStringLiteral("Open in Compositing Page")));
        QCOMPARE(p.undoStack()->count(), steps + 1);
        QCOMPARE(p.undoStack()->undoText(), QStringLiteral("Open in Compositing Page"));
        QCOMPARE(*p.clipComposition(uid, id), c);
        // The same value again makes no step.
        QVERIFY(p.setClipComposition(uid, id, c, QStringLiteral("Nothing")));
        QCOMPARE(p.undoStack()->count(), steps + 1);
        co::addNodeAfter(c, nodetypes::Background, mediaInOf(c));
        QVERIFY(p.setClipComposition(uid, id, c, QStringLiteral("Add Node")));
        QCOMPARE(p.undoStack()->count(), steps + 2);
        // A drag: several calls inside one open edit are one step.
        p.beginEdit(QStringLiteral("Change Blend"));
        for (double v : {0.2, 0.4, 0.6}) {
            c.node(nodeOfType(c, nodetypes::Merge))->params.values[nodekeys::Opacity].base = v;
            QVERIFY(p.setClipComposition(uid, id, c, QStringLiteral("Change Blend")));
        }
        p.endEdit();
        QCOMPARE(p.undoStack()->count(), steps + 3);
        p.undo();
        QCOMPARE(p.clipComposition(uid, id)->nodes.size(), 4);
        QCOMPARE(p.clipComposition(uid, id)
                     ->node(nodeOfType(c, nodetypes::Merge))
                     ->params.value(nodekeys::Opacity, 0),
            1.0);
        p.undo();
        QCOMPARE(p.clipComposition(uid, id)->nodes.size(), 2);
        p.undo();
        QVERIFY(!p.clipComposition(uid, id));
        p.redo();
        QCOMPARE(*p.clipComposition(uid, id), co::defaultComposition());
        // Reset Composition.
        QVERIFY(p.setClipComposition(uid, id, std::nullopt, QStringLiteral("Reset Composition")));
        QVERIFY(!p.clipComposition(uid, id));
        QVERIFY(!p.setClipComposition(uid, 999, c, QStringLiteral("x")));
        QVERIFY(!p.setClipComposition(9999, id, c, QStringLiteral("x")));
    }

    void inactiveTimelineUndo()
    {
        Project p;
        const quint64 first = p.activeTimelineUid();
        p.beginEdit(QStringLiteral("Add"));
        const quint64 id
            = editops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 50));
        p.endEdit();
        const quint64 second = p.createTimeline(QStringLiteral("Other"));
        QVERIFY(p.setActiveTimeline(second));
        QVERIFY(p.setClipComposition(first, id, co::defaultComposition(), QStringLiteral("Edit")));
        QVERIFY(p.clipComposition(first, id));
        QCOMPARE(p.activeTimelineUid(), second);
        p.undo();
        QVERIFY(!p.clipComposition(first, id));
    }

    void projectFileRoundTripAndMigration()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("comp.kkproj"));
        Project p;
        const quint64 uid = p.activeTimelineUid();
        p.beginEdit(QStringLiteral("Add"));
        const quint64 id
            = editops::overwriteClip(p.timeline()->mutableData(), kV1, mediaClip(0, 50));
        p.endEdit();
        QVERIFY(p.setClipComposition(uid, id, richComposition(), QStringLiteral("Comp")));
        QString error;
        QVERIFY2(p.save(path, &error), qPrintable(error));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        QCOMPARE(root.value(QLatin1String("formatVersion")).toInt(), serialization::kFormatVersion);
        Project again;
        QVERIFY2(again.load(path, &error), qPrintable(error));
        QCOMPARE(*again.clipComposition(again.activeTimelineUid(), id), richComposition());

        // A format 14 file (no "composition" anywhere) loads with no compositions.
        const QByteArray v14 = R"({"formatVersion": 14, "project": {"name": "old"},
            "mediaPool": {"bins": [], "items": []},
            "timelines": [{"uid": 1, "name": "T", "fps": {"num": 25, "den": 1}, "width": 1920,
              "height": 1080, "nextId": 10, "videoTracks": [{"id": 1, "clips": [
                {"id": 2, "kind": "solid", "start": 0, "duration": 100}]}],
              "audioTracks": []}]})";
        const QString oldPath = dir.filePath(QStringLiteral("v14.kkproj"));
        QFile o(oldPath);
        QVERIFY(o.open(QIODevice::WriteOnly));
        o.write(v14);
        o.close();
        Project old;
        QVERIFY2(old.load(oldPath, &error), qPrintable(error));
        const TimelineData& d = old.timeline()->data();
        QCOMPARE(d.videoTracks[0].clips.size(), size_t(1));
        QVERIFY(!d.videoTracks[0].clips[0].composition);
    }

    void mediaInCountsAsUse()
    {
        TimelineData d = timeline();
        ClipData clip = mediaClip(0, 10);
        Composition c = co::defaultComposition();
        const quint64 in2 = co::addNodeAfter(c, nodetypes::MediaIn, mediaInOf(c)).node;
        c.node(in2)->mediaId = QStringLiteral("media-2");
        clip.composition = c;
        editops::overwriteClip(d, kV1, clip);
        const usage::Counts counts
            = usage::compute({std::make_shared<const TimelineData>(d)}, MediaPoolState {});
        QCOMPARE(counts.value(QStringLiteral("media-1")), 1);
        QCOMPARE(counts.value(QStringLiteral("media-2")), 1);
    }
};

QTEST_GUILESS_MAIN(TestComposition)
#include "test_composition.moc"
