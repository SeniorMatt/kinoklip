// SPDX-License-Identifier: GPL-3.0-or-later
// CompAdapter implementation: thin wrappers over core/NodeRegistry, core/CompOps and
// Project::setClipComposition() (all compositions live in ClipData::composition).
#include "ui/comp/CompAdapter.h"

#include "core/CompOps.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "core/VideoEffectKeys.h"

#include <QCoreApplication>
#include <QHash>

#include <algorithm>

namespace kk::compadapter {

namespace {

/// Library order of the categories.
constexpr NodeCategory kCategoryOrder[]
    = {NodeCategory::InputOutput, NodeCategory::Composite, NodeCategory::Transform,
        NodeCategory::Generator, NodeCategory::Mask, NodeCategory::Color, NodeCategory::Matte,
        NodeCategory::Effect};

LinkRole roleOf(const NodePort& port)
{
    if (port.kind == PortKind::Mask)
        return LinkRole::Mask;
    if (port.id == nodeports::Foreground || port.id == nodeports::Source)
        return LinkRole::Foreground;
    return LinkRole::Background;
}

NodeTypeInfo toInfo(const NodeDescriptor& d)
{
    NodeTypeInfo info;
    info.type = d.id;
    info.name = d.name;
    info.category = NodeRegistry::categoryName(d.category);
    info.baseName = d.nameBase;
    for (const NodePort& p : d.inputs)
        info.inputs.append(PortInfo {p.id, p.label, p.kind, roleOf(p), p.required});
    info.outputCount = d.outputs;
    info.outputKind = d.outputKind;
    info.params = d.params;
    info.singleton = d.id == nodetypes::MediaOut;
    info.main = d.mainInput;
    return info;
}

QVector<NodeTypeInfo> buildCatalogue()
{
    // Library order: by category (kCategoryOrder), registry order inside a category.
    QVector<NodeTypeInfo> all;
    const NodeRegistry& reg = NodeRegistry::instance();
    for (NodeCategory c : kCategoryOrder)
        for (const NodeDescriptor* d : reg.byCategory(c))
            all.append(toInfo(*d));
    return all;
}

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::compadapter", text);
}

quint64 activeUid(const Project& project)
{
    return project.activeTimelineUid();
}

} // namespace

int NodeTypeInfo::inputIndex(const QString& id) const
{
    for (int i = 0; i < inputs.size(); ++i)
        if (inputs[i].id == id)
            return i;
    return -1;
}

const QVector<NodeTypeInfo>& nodeTypes()
{
    static const QVector<NodeTypeInfo> all = buildCatalogue();
    return all;
}

const NodeTypeInfo* nodeType(const QString& type)
{
    // About 80 types, looked up per painted tile: a static hash keeps it O(1).
    static const QHash<QString, qsizetype> index = [] {
        QHash<QString, qsizetype> h;
        const QVector<NodeTypeInfo>& all = nodeTypes();
        for (qsizetype i = 0; i < all.size(); ++i)
            h.insert(all[i].type, i);
        return h;
    }();
    const auto it = index.constFind(type);
    return it == index.cend() ? nullptr : &nodeTypes()[*it];
}

QStringList categories()
{
    QStringList out;
    for (const NodeTypeInfo& info : nodeTypes())
        if (!out.contains(info.category))
            out.append(info.category);
    return out;
}

QStringList toolbarTypes()
{
    QStringList out {nodetypes::Background, nodetypes::Text, nodetypes::Merge, nodetypes::Transform,
        nodetypes::RectangleMask, nodetypes::EllipseMask, nodetypes::PolygonMask,
        nodetypes::BSplineMask, nodetypes::LumaKeyer, nodetypes::DeltaKeyer,
        nodetypes::CornerPositioner, NodeRegistry::effectNodeType(QStringLiteral("gaussian_blur")), nodetypes::ColorCorrector,
        NodeRegistry::effectNodeType(QLatin1String(fxid::ChromaKey))};
    out.erase(std::remove_if(
                  out.begin(), out.end(), [](const QString& t) { return nodeType(t) == nullptr; }),
        out.end());
    return out;
}

ViewerControlKeys viewerControls(const QString& type)
{
    const NodeDescriptor* d = NodeRegistry::instance().find(type);
    ViewerControlKeys k;
    if (!d || d->controls == ViewerControls::None)
        return k;
    auto has = [d](const QString& key) { return d->param(key) ? key : QString(); };
    k.centerX = has(nodekeys::CenterX);
    k.centerY = has(nodekeys::CenterY);
    k.angle = has(nodekeys::Angle);
    switch (d->controls) {
    case ViewerControls::Transform:
        k.kind = ViewerControl::Transform;
        k.size = has(nodekeys::Size);
        break;
    case ViewerControls::Rect:
        k.kind = type == nodetypes::EllipseMask ? ViewerControl::Ellipse : ViewerControl::Rectangle;
        k.width = has(nodekeys::Width);
        k.height = has(nodekeys::Height);
        break;
    case ViewerControls::Polygon:
        k.kind = ViewerControl::Polygon;
        k.size = has(nodekeys::Size);
        break;
    case ViewerControls::Tracker:
        k.kind = ViewerControl::Tracker;
        k.size = has(nodekeys::PatternSize);
        k.width = has(nodekeys::SearchSize);
        break;
    case ViewerControls::Planar:
        k.kind = ViewerControl::Planar;
        break;
    case ViewerControls::None:
        break;
    }
    return k;
}

QString uniqueName(const Composition& comp, const QString& base)
{
    return compops::uniqueName(comp, base);
}

Composition defaultComposition()
{
    return compops::defaultComposition();
}

quint64 addNode(Composition& comp, const QString& type, QPointF position)
{
    const NodeTypeInfo* info = nodeType(type);
    if (!info)
        return 0;
    if (info->singleton) {
        for (const CompNode& n : comp.nodes)
            if (n.type == type)
                return 0;
    }
    return compops::addNode(comp, type, position);
}

quint64 addNodeAfter(Composition& comp, const QString& type, quint64 afterId)
{
    const NodeTypeInfo* info = nodeType(type);
    if (!info || info->singleton)
        return 0;
    return compops::addNodeAfter(comp, type, afterId).node;
}

bool connectNodes(
    Composition& comp, quint64 fromId, int outputIndex, quint64 toId, const QString& port)
{
    const CompNode* to = comp.node(toId);
    if (!to || fromId == toId || to->inputs.value(port) == NodeLink {fromId, outputIndex})
        return false;
    return compops::connect(comp, fromId, outputIndex, toId, port) == compops::ConnectError::None;
}

bool disconnectInput(Composition& comp, quint64 toId, const QString& port)
{
    return compops::disconnect(comp, toId, port);
}

bool insertIntoLink(Composition& comp, quint64 nodeId, quint64 toId, const QString& port)
{
    return compops::insertIntoLink(comp, nodeId, toId, port);
}

bool removeItems(Composition& comp, const QSet<quint64>& ids, const QSet<quint64>& noteIds)
{
    const int removed = compops::removeNodes(comp, QVector<quint64>(ids.cbegin(), ids.cend()));
    const qsizetype notesBefore = comp.notes.size();
    comp.notes.erase(std::remove_if(comp.notes.begin(), comp.notes.end(),
                         [&noteIds](const CompNote& n) { return noteIds.contains(n.id); }),
        comp.notes.end());
    return removed > 0 || comp.notes.size() != notesBefore;
}

bool renameNode(Composition& comp, quint64 id, const QString& name)
{
    const CompNode* node = comp.node(id);
    const QString wanted = name.trimmed();
    if (!node || node->name == wanted)
        return false;
    return compops::renameNode(comp, id, wanted);
}

Composition extract(const Composition& comp, const QSet<quint64>& ids)
{
    Composition out;
    out.nodes = compops::copyNodes(comp, QVector<quint64>(ids.cbegin(), ids.cend()));
    return out;
}

QVector<quint64> paste(Composition& comp, const Composition& fragment, QPointF offset)
{
    return compops::pasteNodes(comp, fragment.nodes, offset);
}

bool swapMainInputs(Composition& comp, quint64 id)
{
    CompNode* n = comp.node(id);
    const NodeDescriptor* d = n ? NodeRegistry::instance().find(n->type) : nullptr;
    if (!d || !d->input(nodeports::Background) || !d->input(nodeports::Foreground))
        return false;
    const NodeLink a = n->inputs.value(nodeports::Background);
    const NodeLink b = n->inputs.value(nodeports::Foreground);
    if (!a.isConnected() && !b.isConnected())
        return false;
    n->inputs.remove(nodeports::Background);
    n->inputs.remove(nodeports::Foreground);
    if (b.isConnected())
        n->inputs.insert(nodeports::Background, b);
    if (a.isConnected())
        n->inputs.insert(nodeports::Foreground, a);
    return true;
}

bool reachesOutput(const Composition& comp, quint64 id)
{
    return comp.mediaOutId != 0 && compops::dependsOn(comp, comp.mediaOutId, id);
}

// ---- Project access ----

std::optional<Composition> clipComposition(const Project& project, quint64 clipId)
{
    return project.clipComposition(activeUid(project), clipId);
}

bool hasComposition(const Project& project, quint64 clipId)
{
    const ClipData* c = editops::clipById(project.timeline()->data(), clipId);
    return c && c->composition.has_value();
}

bool setClipComposition(
    Project& project, quint64 clipId, const std::optional<Composition>& comp, const QString& label)
{
    const ClipData* c = editops::clipById(project.timeline()->data(), clipId);
    if (!c || c->composition == comp)
        return false;
    return project.setClipComposition(activeUid(project), clipId, comp, label);
}

int resetCompositions(Project& project, const QVector<quint64>& clipIds)
{
    QVector<quint64> with;
    for (quint64 id : clipIds)
        if (hasComposition(project, id) && !with.contains(id))
            with.append(id);
    if (with.isEmpty())
        return 0;
    const QString label = tr("Reset Composition");
    project.beginEdit(label); // several clips: one step
    for (quint64 id : with)
        project.setClipComposition(activeUid(project), id, std::nullopt, label);
    project.endEdit();
    return int(with.size());
}

void beginCompEdit(Project& project, const QString& label)
{
    project.beginEdit(label);
}

void endCompEdit(Project& project)
{
    project.endEdit();
}

bool modifyClipComposition(Project& project, quint64 clipId, const Composition& comp)
{
    // Inside the open edit Project::setClipComposition() joins it (one step at endEdit()).
    if (!setClipComposition(project, clipId, comp, QString()))
        return false;
    project.timeline()->notifyChanged(); // live update while the gesture continues
    return true;
}

} // namespace kk::compadapter
