// SPDX-License-Identifier: GPL-3.0-or-later
// CompOps implementation (pure functions on Composition values).
#include "core/CompOps.h"

#include <QHash>
#include <QSet>

#include <cmath>
#include <functional>

namespace kk::compops {

namespace {

/// Spacing of nodes placed automatically (grid units: 1 = one tile height). The node editor
/// draws tiles 3.4 units wide (NodeEditor::kTileWidth), so a step of 4.5 leaves a gap for the
/// link between neighbours.
constexpr double kStepX = 4.5;
constexpr double kStepY = 1.5;

const NodeDescriptor* descriptorOf(const CompNode& n)
{
    return NodeRegistry::instance().find(n.type);
}

PortKind outputKindOf(const CompNode& n)
{
    const NodeDescriptor* d = descriptorOf(n);
    return d ? d->outputKind : PortKind::Image;
}

/// Kind of input @p port of @p n (Image if unknown).
PortKind inputKindOf(const CompNode& n, const QString& port)
{
    const NodeDescriptor* d = descriptorOf(n);
    const NodePort* p = d ? d->input(port) : nullptr;
    return p ? p->kind : PortKind::Image;
}

/// Moves every node downstream of @p from (not @p from itself) right by @p dx.
void shiftDownstream(Composition& comp, quint64 from, double dx)
{
    QVector<quint64> ids;
    for (const CompNode& n : comp.nodes) {
        if (n.id != from && dependsOn(comp, n.id, from))
            ids.append(n.id);
    }
    moveNodes(comp, ids, QPointF(dx, 0));
}

/// Makes every input that reads output 0 of @p from read @p to instead (except @p to's own).
void rewireConsumers(Composition& comp, quint64 from, quint64 to)
{
    for (CompNode& n : comp.nodes) {
        if (n.id == to)
            continue;
        for (auto it = n.inputs.begin(); it != n.inputs.end(); ++it) {
            if (it->nodeId == from && it->outputIndex == 0)
                *it = NodeLink {to, 0};
        }
    }
}

/// The base of a node name: "Merge12" -> "Merge".
QString nameBase(const QString& name)
{
    qsizetype end = name.size();
    while (end > 0 && name.at(end - 1).isDigit())
        --end;
    return end > 0 ? name.left(end) : QStringLiteral("Node");
}

bool nameTaken(const Composition& comp, const QString& name, quint64 except = 0)
{
    for (const CompNode& n : comp.nodes) {
        if (n.id != except && n.name == name)
            return true;
    }
    return false;
}

/// A regular pentagon of radius @p r (mask-local px): the starting shape of a Polygon node.
AnimatedPath defaultPolygon(double r)
{
    AnimatedPath path;
    MaskPath shape;
    for (int i = 0; i < 5; ++i) {
        const double a = -M_PI / 2 + i * 2 * M_PI / 5;
        MaskPoint p;
        p.x = r * std::cos(a);
        p.y = r * std::sin(a);
        shape.append(p);
    }
    path.shapes = {shape};
    return path;
}

} // namespace

Composition defaultComposition()
{
    Composition comp;
    const quint64 in = addNode(comp, nodetypes::MediaIn, QPointF(0, 0));
    const quint64 out = addNode(comp, nodetypes::MediaOut, QPointF(2 * kStepX, 0));
    comp.mediaOutId = out;
    connect(comp, in, 0, out, nodeports::Input);
    return comp;
}

bool isPassThrough(const Composition& comp)
{
    const CompNode* out = comp.node(comp.mediaOutId);
    if (!out)
        return false;
    const auto it = out->inputs.constFind(nodeports::Input);
    if (it == out->inputs.constEnd() || it->outputIndex != 0)
        return false;
    const CompNode* n = comp.node(it->nodeId);
    for (int guard = 0; n && guard < comp.nodes.size(); ++guard) {
        if (n->enabled)
            return n->type == nodetypes::MediaIn && n->mediaId.isEmpty();
        const NodeLink* main = mainInputLink(*n);
        if (!main || main->outputIndex != 0)
            return false;
        n = comp.node(main->nodeId);
    }
    return false;
}

QString uniqueName(const Composition& comp, const QString& base)
{
    for (int i = 1;; ++i) {
        const QString name = base + QString::number(i);
        if (!nameTaken(comp, name))
            return name;
    }
}

std::optional<CompNode> makeNode(Composition& comp, const QString& type, QPointF position)
{
    const NodeDescriptor* d = NodeRegistry::instance().find(type);
    if (!d)
        return std::nullopt;
    CompNode n;
    n.id = comp.allocateId();
    n.type = type;
    n.name = uniqueName(comp, d->nameBase);
    n.position = position;
    n.params = NodeRegistry::instance().defaultParams(type);
    if (type == nodetypes::PolygonMask)
        n.path = defaultPolygon(216.0); // a fifth of a 1080 px frame height
    return n;
}

quint64 addNode(Composition& comp, const QString& type, QPointF position)
{
    std::optional<CompNode> n = makeNode(comp, type, position);
    if (!n)
        return 0;
    const quint64 id = n->id;
    comp.nodes.append(std::move(*n));
    return id;
}

AddResult addNodeAfter(Composition& comp, const QString& type, quint64 selected)
{
    AddResult result;
    const NodeDescriptor* d = NodeRegistry::instance().find(type);
    if (!d)
        return result;
    const CompNode* sel = comp.node(selected);
    if (sel && sel->type == nodetypes::MediaOut) {
        // "After" MediaOut means into its input: after whatever feeds it.
        const auto it = sel->inputs.constFind(nodeports::Input);
        if (it != sel->inputs.constEnd() && comp.node(it->nodeId))
            return addNodeAfter(comp, type, it->nodeId);
        const QPointF pos = sel->position - QPointF(kStepX, 0);
        result.node = addNode(comp, type, pos);
        if (!d->mainInput.isEmpty() || d->outputKind == PortKind::Image)
            connect(comp, result.node, 0, selected, nodeports::Input);
        return result;
    }
    if (!sel) {
        double maxX = -kStepX;
        double y = 0.0;
        for (const CompNode& n : comp.nodes) {
            if (n.position.x() > maxX) {
                maxX = n.position.x();
                y = n.position.y();
            }
        }
        result.node = addNode(comp, type, QPointF(maxX + kStepX, y));
        return result;
    }
    const QPointF selPos = sel->position;
    const quint64 selId = sel->id;
    const bool selHasFreeMask = [&] {
        const NodeDescriptor* sd = descriptorOf(*sel);
        const NodePort* mp = sd ? sd->input(nodeports::Mask) : nullptr;
        return mp && mp->kind == PortKind::Mask && !sel->inputs.contains(nodeports::Mask);
    }();
    // `sel` may dangle from here on (nodes are appended).
    if (d->outputKind == PortKind::Mask) {
        result.node = addNode(comp, type, selPos - QPointF(0, kStepY));
        if (selHasFreeMask)
            connect(comp, result.node, 0, selId, nodeports::Mask);
        return result;
    }
    if (d->isSource()) {
        // Background / Text / noise / another MediaIn: merged over the selected node.
        shiftDownstream(comp, selId, kStepX);
        result.merge = addNode(comp, nodetypes::Merge, selPos + QPointF(kStepX, 0));
        result.node = addNode(comp, type, selPos + QPointF(kStepX, -kStepY));
        rewireConsumers(comp, selId, result.merge);
        if (kindsCompatible(outputKindOf(*comp.node(selId)), PortKind::Image))
            connect(comp, selId, 0, result.merge, nodeports::Background);
        connect(comp, result.node, 0, result.merge, nodeports::Foreground);
        return result;
    }
    // A node with an image input: between the selected node and what it feeds.
    shiftDownstream(comp, selId, kStepX);
    result.node = addNode(comp, type, selPos + QPointF(kStepX, 0));
    if (kindsCompatible(
            outputKindOf(*comp.node(selId)), inputKindOf(*comp.node(result.node), d->mainInput))) {
        rewireConsumers(comp, selId, result.node);
        connect(comp, selId, 0, result.node, d->mainInput);
    }
    return result;
}

int removeNodes(Composition& comp, const QVector<quint64>& ids)
{
    QSet<quint64> removed;
    for (quint64 id : ids) {
        if (id != comp.mediaOutId && comp.node(id))
            removed.insert(id);
    }
    if (removed.isEmpty())
        return 0;
    // What an input reading a removed node reads instead: that node's main input, following
    // further removed nodes (a removed chain collapses to what fed it).
    const auto replacement = [&](NodeLink link) -> std::optional<NodeLink> {
        for (int guard = 0; guard <= comp.nodes.size(); ++guard) {
            if (!removed.contains(link.nodeId))
                return link;
            const CompNode* n = comp.node(link.nodeId);
            const NodeLink* main = n ? mainInputLink(*n) : nullptr;
            if (!main || link.outputIndex != 0)
                return std::nullopt;
            link = *main;
        }
        return std::nullopt;
    };
    for (CompNode& n : comp.nodes) {
        if (removed.contains(n.id))
            continue;
        for (auto it = n.inputs.begin(); it != n.inputs.end();) {
            if (!removed.contains(it->nodeId)) {
                ++it;
                continue;
            }
            const std::optional<NodeLink> to = replacement(*it);
            const CompNode* src = to ? comp.node(to->nodeId) : nullptr;
            if (src && kindsCompatible(outputKindOf(*src), inputKindOf(n, it.key()))) {
                *it = *to;
                ++it;
            } else {
                it = n.inputs.erase(it);
            }
        }
    }
    comp.nodes.removeIf([&removed](const CompNode& n) { return removed.contains(n.id); });
    if (removed.contains(comp.leftViewerNode))
        comp.leftViewerNode = 0;
    if (removed.contains(comp.rightViewerNode))
        comp.rightViewerNode = 0;
    return int(removed.size());
}

bool kindsCompatible(PortKind output, PortKind input)
{
    return output == input || (output == PortKind::Image && input == PortKind::Mask);
}

bool dependsOn(const Composition& comp, quint64 node, quint64 on)
{
    QSet<quint64> seen;
    QVector<quint64> stack {node};
    while (!stack.isEmpty()) {
        const quint64 id = stack.takeLast();
        if (id == on)
            return true;
        if (seen.contains(id))
            continue;
        seen.insert(id);
        if (const CompNode* n = comp.node(id)) {
            for (const NodeLink& l : n->inputs) {
                if (l.isConnected())
                    stack.append(l.nodeId);
            }
        }
    }
    return false;
}

ConnectError connect(Composition& comp, quint64 from, int output, quint64 to, const QString& port)
{
    const CompNode* src = comp.node(from);
    CompNode* dst = comp.node(to);
    const NodeDescriptor* sd = src ? descriptorOf(*src) : nullptr;
    const NodeDescriptor* dd = dst ? descriptorOf(*dst) : nullptr;
    if (!sd || !dd)
        return ConnectError::UnknownNode;
    const NodePort* p = dd->input(port);
    if (!p || output < 0 || output >= sd->outputs)
        return ConnectError::UnknownPort;
    if (!kindsCompatible(sd->outputKind, p->kind))
        return ConnectError::WrongKind;
    if (dependsOn(comp, from, to))
        return ConnectError::Cycle;
    dst->inputs.insert(port, NodeLink {from, output});
    return ConnectError::None;
}

bool disconnect(Composition& comp, quint64 to, const QString& port)
{
    CompNode* n = comp.node(to);
    return n && n->inputs.remove(port) > 0;
}

bool insertIntoLink(Composition& comp, quint64 node, quint64 to, const QString& port)
{
    CompNode* target = comp.node(to);
    CompNode* inserted = comp.node(node);
    if (!target || !inserted || node == to)
        return false;
    const auto it = target->inputs.constFind(port);
    if (it == target->inputs.constEnd() || it->nodeId == node)
        return false;
    const NodeLink link = *it;
    const CompNode* src = comp.node(link.nodeId);
    const NodeDescriptor* d = descriptorOf(*inserted);
    if (!src || !d || d->mainInput.isEmpty() || d->outputs < 1)
        return false;
    if (!kindsCompatible(outputKindOf(*src), inputKindOf(*inserted, d->mainInput))
        || !kindsCompatible(d->outputKind, inputKindOf(*target, port)))
        return false;
    // After the insert `node` reads the link's source and `to` reads `node`.
    if (dependsOn(comp, link.nodeId, node) || dependsOn(comp, node, to))
        return false;
    inserted->inputs.insert(d->mainInput, link);
    comp.node(to)->inputs.insert(port, NodeLink {node, 0});
    return true;
}

void moveNodes(Composition& comp, const QVector<quint64>& ids, QPointF delta)
{
    for (CompNode& n : comp.nodes) {
        if (ids.contains(n.id))
            n.position += delta;
    }
}

bool renameNode(Composition& comp, quint64 node, const QString& name)
{
    CompNode* n = comp.node(node);
    const QString trimmed = name.trimmed();
    if (!n || trimmed.isEmpty() || nameTaken(comp, trimmed, node))
        return false;
    n->name = trimmed;
    return true;
}

bool setEnabled(Composition& comp, quint64 node, bool enabled)
{
    CompNode* n = comp.node(node);
    if (!n)
        return false;
    n->enabled = enabled;
    return true;
}

QVector<CompNode> copyNodes(const Composition& comp, const QVector<quint64>& ids)
{
    QSet<quint64> set;
    for (quint64 id : ids) {
        if (id != comp.mediaOutId)
            set.insert(id);
    }
    QVector<CompNode> out;
    for (const CompNode& n : comp.nodes) {
        if (!set.contains(n.id) || n.type == nodetypes::MediaOut)
            continue;
        CompNode copy = n;
        copy.inputs.removeIf(
            [&set](QMap<QString, NodeLink>::iterator it) { return !set.contains(it->nodeId); });
        out.append(std::move(copy));
    }
    return out;
}

QVector<quint64> pasteNodes(Composition& comp, const QVector<CompNode>& nodes, QPointF offset)
{
    QHash<quint64, quint64> idMap;
    QVector<quint64> created;
    const qsizetype first = comp.nodes.size();
    for (const CompNode& n : nodes) {
        if (n.type == nodetypes::MediaOut || !NodeRegistry::instance().find(n.type))
            continue;
        CompNode copy = n;
        copy.id = comp.allocateId();
        if (n.id != 0)
            idMap.insert(n.id, copy.id);
        if (copy.name.trimmed().isEmpty() || nameTaken(comp, copy.name))
            copy.name = uniqueName(comp, nameBase(copy.name.trimmed()));
        copy.position += offset;
        created.append(copy.id);
        comp.nodes.append(std::move(copy));
    }
    for (qsizetype i = first; i < comp.nodes.size(); ++i) {
        CompNode& n = comp.nodes[i];
        for (auto it = n.inputs.begin(); it != n.inputs.end();) {
            const auto mapped = idMap.constFind(it->nodeId);
            if (mapped == idMap.constEnd()) {
                it = n.inputs.erase(it);
            } else {
                it->nodeId = *mapped;
                ++it;
            }
        }
    }
    return created;
}

void setViewerNodes(Composition& comp, quint64 left, quint64 right)
{
    comp.leftViewerNode = comp.node(left) ? left : 0;
    comp.rightViewerNode = comp.node(right) ? right : 0;
}

QVector<quint64> evaluationOrder(const Composition& comp, quint64 target)
{
    QVector<quint64> order;
    if (!comp.node(target))
        return order;
    QSet<quint64> done;
    QSet<quint64> active; // on the current path: guards against cycles in damaged data
    const std::function<void(quint64)> visit = [&](quint64 id) {
        if (done.contains(id) || active.contains(id))
            return;
        const CompNode* n = comp.node(id);
        if (!n)
            return;
        active.insert(id);
        if (!n->enabled) {
            if (const NodeLink* main = mainInputLink(*n))
                visit(main->nodeId);
        } else {
            // Descriptor order first (deterministic, matches the tiles), then any other port.
            const NodeDescriptor* d = descriptorOf(*n);
            if (d) {
                for (const NodePort& p : d->inputs) {
                    if (p.analysisOnly)
                        continue; // only looked at by the UI (tracking), never drawn
                    const auto it = n->inputs.constFind(p.id);
                    if (it != n->inputs.constEnd())
                        visit(it->nodeId);
                }
            }
        }
        active.remove(id);
        done.insert(id);
        order.append(id);
    };
    visit(target);
    return order;
}

const NodeLink* mainInputLink(const CompNode& node)
{
    const NodeDescriptor* d = descriptorOf(node);
    if (!d || d->mainInput.isEmpty())
        return nullptr;
    const auto it = node.inputs.constFind(d->mainInput);
    return it == node.inputs.constEnd() || !it->isConnected() ? nullptr : &*it;
}

} // namespace kk::compops
