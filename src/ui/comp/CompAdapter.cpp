// SPDX-License-Identifier: GPL-3.0-or-later
// CompAdapter implementation. STAND-IN until the core commit (core/NodeRegistry, core/CompOps,
// ClipData::composition) is merged: a small node catalogue, the graph operations and a
// per-project composition store with its own undo commands. Replace the bodies with calls into
// the core; the header stays.
#include "ui/comp/CompAdapter.h"

#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"

#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QUndoCommand>
#include <QUndoStack>

#include <utility>

namespace kk::compadapter {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::compadapter", text);
}

/// Grid units between a node and the one added after it.
constexpr double kAfterStep = 4.0;

ParamDescriptor scalar(const char* key, const QString& label, double min, double max, double def,
    double step, const QString& unit = {})
{
    ParamDescriptor d;
    d.key = QLatin1String(key);
    d.label = label;
    d.type = ParamType::Double;
    d.min = min;
    d.max = max;
    d.def = def;
    d.step = step;
    d.unit = unit;
    return d;
}

ParamDescriptor fixed(const char* key, const QString& label, ParamType type, const QVariant& def)
{
    ParamDescriptor d;
    d.key = QLatin1String(key);
    d.label = label;
    d.type = type;
    d.defStatic = def;
    d.animatable = false;
    return d;
}

ParamDescriptor choice(const char* key, const QString& label, const QStringList& values,
    const QStringList& labels)
{
    ParamDescriptor d = fixed(key, label, ParamType::Choice, values.value(0));
    d.choices = values;
    d.choiceLabels = labels;
    return d;
}

PortInfo port(const char* id, const QString& label, PortKind kind, LinkRole role, bool required)
{
    return PortInfo {QLatin1String(id), label, kind, role, required};
}

QVector<ParamDescriptor> transformParams()
{
    return {scalar("centerX", tr("Center X"), -2, 3, 0.5, 0.001),
        scalar("centerY", tr("Center Y"), -2, 3, 0.5, 0.001),
        scalar("size", tr("Size"), 0, 10, 1, 0.01),
        scalar("angle", tr("Angle"), -3600, 3600, 0, 0.1, QStringLiteral("°"))};
}

QVector<ParamDescriptor> maskParams()
{
    QVector<ParamDescriptor> p {scalar("centerX", tr("Center X"), -2, 3, 0.5, 0.001),
        scalar("centerY", tr("Center Y"), -2, 3, 0.5, 0.001),
        scalar("width", tr("Width"), 0, 4, 0.5, 0.001),
        scalar("height", tr("Height"), 0, 4, 0.5, 0.001),
        scalar("angle", tr("Angle"), -3600, 3600, 0, 0.1, QStringLiteral("°")),
        scalar("softEdge", tr("Soft Edge"), 0, 1, 0, 0.001)};
    p.append(fixed("invert", tr("Invert"), ParamType::Bool, false));
    return p;
}

QVector<NodeTypeInfo> buildCatalogue()
{
    const QString io = tr("I/O"), composite = tr("Composite"), transform = tr("Transform"),
                  generate = tr("Generate"), masks = tr("Masks"), color = tr("Color"),
                  effects = tr("Effects");
    const PortInfo input = port("input", tr("Input"), PortKind::Image, LinkRole::Background, true);
    const PortInfo effectMask
        = port("mask", tr("Effect Mask"), PortKind::Mask, LinkRole::Mask, false);
    QVector<NodeTypeInfo> all;
    auto add = [&all](NodeTypeInfo info) { all.append(std::move(info)); };

    add({QStringLiteral("media_in"), tr("MediaIn"), io, QStringLiteral("MediaIn"), {}, 1, {}});
    add({QStringLiteral("media_out"), tr("MediaOut"), io, QStringLiteral("MediaOut"),
        {port("input", tr("Input"), PortKind::Image, LinkRole::Background, true)}, 0, {}, true});

    QVector<ParamDescriptor> merge
        = {choice("blend", tr("Blend Mode"),
               {QStringLiteral("normal"), QStringLiteral("screen"), QStringLiteral("multiply"),
                   QStringLiteral("add"), QStringLiteral("overlay"), QStringLiteral("darken"),
                   QStringLiteral("lighten"), QStringLiteral("difference"),
                   QStringLiteral("subtract")},
               {tr("Normal"), tr("Screen"), tr("Multiply"), tr("Add"), tr("Overlay"),
                   tr("Darken"), tr("Lighten"), tr("Difference"), tr("Subtract")}),
            scalar("opacity", tr("Opacity"), 0, 1, 1, 0.01)};
    merge += transformParams();
    add({QStringLiteral("merge"), tr("Merge"), composite, QStringLiteral("Merge"),
        {port("background", tr("Background"), PortKind::Image, LinkRole::Background, true),
            port("foreground", tr("Foreground"), PortKind::Image, LinkRole::Foreground, false),
            port("mask", tr("Mask"), PortKind::Mask, LinkRole::Mask, false)},
        1, merge});
    add({QStringLiteral("dissolve"), tr("Dissolve"), composite, QStringLiteral("Dissolve"),
        {port("background", tr("Background"), PortKind::Image, LinkRole::Background, true),
            port("foreground", tr("Foreground"), PortKind::Image, LinkRole::Foreground, false)},
        1, {scalar("mix", tr("Mix"), 0, 1, 0.5, 0.01)}});

    QVector<ParamDescriptor> xf = transformParams();
    xf += {scalar("pivotX", tr("Pivot X"), -2, 3, 0.5, 0.001),
        scalar("pivotY", tr("Pivot Y"), -2, 3, 0.5, 0.001),
        fixed("flipH", tr("Flip Horizontal"), ParamType::Bool, false),
        fixed("flipV", tr("Flip Vertical"), ParamType::Bool, false)};
    add({QStringLiteral("transform"), tr("Transform"), transform, QStringLiteral("Transform"),
        {input, effectMask}, 1, xf});
    add({QStringLiteral("crop"), tr("Crop"), transform, QStringLiteral("Crop"), {input}, 1,
        {scalar("left", tr("Left"), 0, 1, 0, 0.001), scalar("right", tr("Right"), 0, 1, 0, 0.001),
            scalar("top", tr("Top"), 0, 1, 0, 0.001),
            scalar("bottom", tr("Bottom"), 0, 1, 0, 0.001)}});

    add({QStringLiteral("background"), tr("Background"), generate, QStringLiteral("Background"),
        {effectMask}, 1, {fixed("color", tr("Color"), ParamType::Color, QColor(Qt::black))}});
    add({QStringLiteral("text"), tr("Text"), generate, QStringLiteral("Text"), {effectMask}, 1,
        {fixed("text", tr("Text"), ParamType::String, tr("Title")),
            fixed("font", tr("Font"), ParamType::String, QString()),
            scalar("size", tr("Size"), 1, 1000, 80, 1, QStringLiteral("px")),
            fixed("color", tr("Color"), ParamType::Color, QColor(Qt::white)),
            scalar("centerX", tr("Center X"), -2, 3, 0.5, 0.001),
            scalar("centerY", tr("Center Y"), -2, 3, 0.5, 0.001)}});

    for (const auto& [id, name] : {std::pair {"rectangle", tr("Rectangle")},
             std::pair {"ellipse", tr("Ellipse")}, std::pair {"polygon", tr("Polygon")}}) {
        QVector<ParamDescriptor> p = maskParams();
        if (QLatin1String(id) == QLatin1String("polygon"))
            p.append(fixed("points", tr("Points"), ParamType::String, QString()));
        add({QLatin1String(id), name, masks, name,
            {port("mask", tr("Mask"), PortKind::Mask, LinkRole::Mask, false)}, 1, p});
    }

    add({QStringLiteral("color_corrector"), tr("Color Corrector"), color,
        QStringLiteral("ColorCorrector"), {input, effectMask}, 1,
        {scalar("gain", tr("Gain"), 0, 4, 1, 0.01), scalar("lift", tr("Lift"), -1, 1, 0, 0.01),
            scalar("gamma", tr("Gamma"), 0.1, 4, 1, 0.01),
            scalar("saturation", tr("Saturation"), 0, 4, 1, 0.01)}});

    // Every Edit page video effect as a node (type fx:<id>), as the core will generate them.
    for (const EffectDescriptor* fx :
        EffectRegistry::instance().byCategory(EffectCategory::VideoEffect)) {
        QString base = fx->name;
        base.remove(QLatin1Char(' '));
        add({QStringLiteral("fx:") + fx->id, fx->name, effects, base, {input, effectMask}, 1,
            fx->params});
    }
    return all;
}

// ---- Stand-in store (until ClipData::composition) ----

using ClipMap = QHash<quint64, Composition>;

struct Store
{
    QHash<quint64, ClipMap> timelines; ///< timeline uid -> clip id -> composition
    int gestureDepth = 0;
    QString gestureLabel;
    QHash<quint64, std::optional<Composition>> gestureBefore; ///< clip -> state at begin
};

QHash<const Project*, Store>& stores()
{
    static QHash<const Project*, Store> s;
    return s;
}

Store& storeFor(Project& project)
{
    auto& all = stores();
    if (!all.contains(&project)) {
        QObject::connect(&project, &QObject::destroyed, [p = &project] { stores().remove(p); });
        QObject::connect(&project, &Project::projectReset, &project,
            [p = &project] { stores()[p].timelines.clear(); });
    }
    return all[&project];
}

std::optional<Composition> stored(const Project& project, quint64 clipId)
{
    const auto it = stores().constFind(&project);
    if (it == stores().cend())
        return std::nullopt;
    const ClipMap clips = it->timelines.value(project.activeTimelineUid());
    const auto c = clips.constFind(clipId);
    return c == clips.cend() ? std::nullopt : std::optional<Composition>(*c);
}

void notify(Project& project)
{
    Timeline* tl = project.timeline();
    (void)tl->mutableData(); // forces the changed() emission below
    tl->notifyChanged();
}

void writeStore(Project& project, quint64 timelineUid, quint64 clipId,
    const std::optional<Composition>& comp)
{
    ClipMap& clips = storeFor(project).timelines[timelineUid];
    if (comp)
        clips.insert(clipId, *comp);
    else
        clips.remove(clipId);
    notify(project);
}

/// Undo step of the stand-in store: before / after of one clip's composition.
class CompositionCommand final : public QUndoCommand
{
public:
    CompositionCommand(Project& project, quint64 timelineUid, quint64 clipId,
        std::optional<Composition> before, std::optional<Composition> after, const QString& label)
        : QUndoCommand(label)
        , m_project(&project)
        , m_timeline(timelineUid)
        , m_clip(clipId)
        , m_before(std::move(before))
        , m_after(std::move(after))
    { }
    void undo() override
    {
        if (m_project)
            writeStore(*m_project, m_timeline, m_clip, m_before);
    }
    void redo() override
    {
        if (m_project)
            writeStore(*m_project, m_timeline, m_clip, m_after);
    }

private:
    QPointer<Project> m_project;
    quint64 m_timeline;
    quint64 m_clip;
    std::optional<Composition> m_before, m_after;
};

bool clipExists(const Project& project, quint64 clipId)
{
    return editops::clipById(project.timeline()->data(), clipId) != nullptr;
}

/// True if @p target is @p from or upstream of it (walking the inputs).
bool isUpstream(const Composition& comp, quint64 from, quint64 target)
{
    QVector<quint64> stack {from};
    QSet<quint64> seen;
    while (!stack.isEmpty()) {
        const quint64 id = stack.takeLast();
        if (id == target)
            return true;
        if (seen.contains(id))
            continue;
        seen.insert(id);
        if (const CompNode* n = comp.node(id)) {
            for (const NodeLink& l : n->inputs)
                if (l.isConnected())
                    stack.append(l.nodeId);
        }
    }
    return false;
}

} // namespace

int NodeTypeInfo::inputIndex(const QString& id) const
{
    for (int i = 0; i < inputs.size(); ++i)
        if (inputs[i].id == id)
            return i;
    return -1;
}

QString NodeTypeInfo::mainInput() const
{
    for (const PortInfo& p : inputs)
        if (p.kind == PortKind::Image)
            return p.id;
    return {};
}

const QVector<NodeTypeInfo>& nodeTypes()
{
    static const QVector<NodeTypeInfo> all = buildCatalogue();
    return all;
}

const NodeTypeInfo* nodeType(const QString& type)
{
    for (const NodeTypeInfo& info : nodeTypes())
        if (info.type == type)
            return &info;
    return nullptr;
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
    QStringList blur = {QStringLiteral("fx:gaussian_blur")};
    if (!nodeType(blur.first())) {
        // The effect id of the blur may differ; take the first effect named like a blur.
        for (const NodeTypeInfo& info : nodeTypes()) {
            if (info.type.startsWith(QLatin1String("fx:"))
                && info.name.contains(QLatin1String("Blur"), Qt::CaseInsensitive)) {
                blur = {info.type};
                break;
            }
        }
    }
    QString chroma;
    for (const NodeTypeInfo& info : nodeTypes()) {
        if (info.type.startsWith(QLatin1String("fx:"))
            && info.name.contains(QLatin1String("Chroma"), Qt::CaseInsensitive)) {
            chroma = info.type;
            break;
        }
    }
    QStringList out {QStringLiteral("background"), QStringLiteral("text"),
        QStringLiteral("merge"), QStringLiteral("transform"), QStringLiteral("rectangle"),
        QStringLiteral("ellipse"), QStringLiteral("polygon")};
    out += blur;
    out.append(QStringLiteral("color_corrector"));
    if (!chroma.isEmpty())
        out.append(chroma);
    out.erase(std::remove_if(out.begin(), out.end(),
                  [](const QString& t) { return nodeType(t) == nullptr; }),
        out.end());
    return out;
}

ViewerControlKeys viewerControls(const QString& type)
{
    ViewerControlKeys k;
    k.centerX = QStringLiteral("centerX");
    k.centerY = QStringLiteral("centerY");
    k.angle = QStringLiteral("angle");
    if (type == QLatin1String("transform") || type == QLatin1String("merge")) {
        k.kind = ViewerControl::Transform;
        k.size = QStringLiteral("size");
    } else if (type == QLatin1String("rectangle") || type == QLatin1String("ellipse")) {
        k.kind = type == QLatin1String("rectangle") ? ViewerControl::Rectangle
                                                     : ViewerControl::Ellipse;
        k.width = QStringLiteral("width");
        k.height = QStringLiteral("height");
    } else if (type == QLatin1String("polygon")) {
        k.kind = ViewerControl::Polygon;
        k.points = QStringLiteral("points");
    } else {
        return {};
    }
    return k;
}

QString uniqueName(const Composition& comp, const QString& base)
{
    for (int n = 1;; ++n) {
        const QString name = base + QString::number(n);
        bool taken = false;
        for (const CompNode& node : comp.nodes) {
            if (node.name.compare(name, Qt::CaseInsensitive) == 0) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return name;
    }
}

Composition defaultComposition()
{
    Composition comp;
    const quint64 in = addNode(comp, QStringLiteral("media_in"), QPointF(0, 0));
    const quint64 out = addNode(comp, QStringLiteral("media_out"), QPointF(kAfterStep * 2, 0));
    comp.mediaOutId = out;
    connectNodes(comp, in, 0, out, QStringLiteral("input"));
    comp.rightViewerNode = 0; // = MediaOut
    return comp;
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
    CompNode node;
    node.id = comp.allocateId();
    node.type = type;
    node.name = uniqueName(comp, info->baseName);
    node.position = position;
    for (const ParamDescriptor& p : info->params) {
        if (p.isScalar())
            node.params.setBase(p.key, p.def);
        else
            node.params.setStatic(p.key, p.defStatic);
    }
    comp.nodes.append(node);
    if (type == QLatin1String("media_out"))
        comp.mediaOutId = node.id;
    return node.id;
}

quint64 addNodeAfter(Composition& comp, const QString& type, quint64 afterId)
{
    const CompNode* after = comp.node(afterId);
    const QPointF pos = after ? after->position + QPointF(kAfterStep, 0) : comp.viewCentre;
    // Make room: nodes downstream on the same row move right so the new one does not cover
    // them (only when they sit exactly where the new node goes).
    const quint64 id = addNode(comp, type, pos);
    if (!id || !after)
        return id;
    const NodeTypeInfo* info = nodeType(type);
    const QString main = info ? info->mainInput() : QString();
    if (main.isEmpty() || !comp.node(afterId)) // generators: not connected after
        return id;
    // Readers of afterId's output read the new node instead.
    for (CompNode& n : comp.nodes) {
        if (n.id == id)
            continue;
        for (auto it = n.inputs.begin(); it != n.inputs.end(); ++it) {
            if (it->nodeId == afterId)
                it->nodeId = id;
        }
    }
    connectNodes(comp, afterId, 0, id, main);
    for (CompNode& n : comp.nodes) {
        if (n.id != id && n.id != afterId && qFuzzyCompare(n.position.y() + 1, pos.y() + 1)
            && n.position.x() >= pos.x() && n.position.x() < pos.x() + kAfterStep)
            n.position.rx() += kAfterStep;
    }
    return id;
}

bool connectNodes(
    Composition& comp, quint64 fromId, int outputIndex, quint64 toId, const QString& port)
{
    const CompNode* from = comp.node(fromId);
    CompNode* to = comp.node(toId);
    if (!from || !to || fromId == toId)
        return false;
    const NodeTypeInfo* fromInfo = nodeType(from->type);
    const NodeTypeInfo* toInfo = nodeType(to->type);
    if (!fromInfo || !toInfo || outputIndex < 0 || outputIndex >= fromInfo->outputCount
        || toInfo->inputIndex(port) < 0)
        return false;
    if (isUpstream(comp, fromId, toId))
        return false; // toId already feeds fromId: a cycle
    const NodeLink link {fromId, outputIndex};
    if (to->inputs.value(port) == link)
        return false;
    to->inputs.insert(port, link);
    return true;
}

bool disconnectInput(Composition& comp, quint64 toId, const QString& port)
{
    CompNode* to = comp.node(toId);
    if (!to || !to->inputs.value(port).isConnected())
        return false;
    to->inputs.remove(port);
    return true;
}

bool insertIntoLink(Composition& comp, quint64 nodeId, quint64 toId, const QString& port)
{
    CompNode* node = comp.node(nodeId);
    const CompNode* to = comp.node(toId);
    if (!node || !to || nodeId == toId)
        return false;
    const NodeLink link = to->inputs.value(port);
    const NodeTypeInfo* info = nodeType(node->type);
    if (!link.isConnected() || !info || info->outputCount == 0 || link.nodeId == nodeId)
        return false;
    const QString main = info->mainInput();
    if (main.isEmpty() || node->inputs.value(main).isConnected())
        return false;
    Composition trial = comp;
    trial.node(toId)->inputs.remove(port);
    if (!connectNodes(trial, link.nodeId, link.outputIndex, nodeId, main)
        || !connectNodes(trial, nodeId, 0, toId, port))
        return false;
    comp = std::move(trial);
    return true;
}

bool removeItems(Composition& comp, const QSet<quint64>& ids, const QSet<quint64>& noteIds)
{
    bool changed = false;
    for (qsizetype i = comp.nodes.size() - 1; i >= 0; --i) {
        if (ids.contains(comp.nodes[i].id) && comp.nodes[i].id != comp.mediaOutId) {
            comp.nodes.removeAt(i);
            changed = true;
        }
    }
    for (qsizetype i = comp.notes.size() - 1; i >= 0; --i) {
        if (noteIds.contains(comp.notes[i].id)) {
            comp.notes.removeAt(i);
            changed = true;
        }
    }
    if (!changed)
        return false;
    for (CompNode& n : comp.nodes) {
        for (auto it = n.inputs.begin(); it != n.inputs.end();) {
            if (!comp.node(it->nodeId))
                it = n.inputs.erase(it);
            else
                ++it;
        }
    }
    if (!comp.node(comp.leftViewerNode))
        comp.leftViewerNode = 0;
    if (!comp.node(comp.rightViewerNode))
        comp.rightViewerNode = 0;
    return true;
}

bool renameNode(Composition& comp, quint64 id, const QString& name)
{
    CompNode* node = comp.node(id);
    const QString wanted = name.trimmed();
    if (!node || wanted.isEmpty() || node->name == wanted)
        return false;
    bool taken = false;
    for (const CompNode& n : comp.nodes)
        if (n.id != id && n.name.compare(wanted, Qt::CaseInsensitive) == 0)
            taken = true;
    node->name = taken ? uniqueName(comp, wanted) : wanted;
    return true;
}

Composition extract(const Composition& comp, const QSet<quint64>& ids)
{
    Composition out;
    for (const CompNode& n : comp.nodes) {
        if (!ids.contains(n.id) || n.id == comp.mediaOutId)
            continue;
        CompNode copy = n;
        for (auto it = copy.inputs.begin(); it != copy.inputs.end();) {
            if (!ids.contains(it->nodeId))
                it = copy.inputs.erase(it);
            else
                ++it;
        }
        out.nodes.append(copy);
    }
    return out;
}

QVector<quint64> paste(Composition& comp, const Composition& fragment, QPointF offset)
{
    QHash<quint64, quint64> map;
    QVector<quint64> added;
    for (const CompNode& n : fragment.nodes) {
        const NodeTypeInfo* info = nodeType(n.type);
        if (!info || info->singleton)
            continue;
        CompNode copy = n;
        copy.id = comp.allocateId();
        copy.name = uniqueName(comp, info->baseName);
        copy.position += offset;
        map.insert(n.id, copy.id);
        comp.nodes.append(copy);
        added.append(copy.id);
    }
    for (quint64 id : std::as_const(added)) {
        CompNode* n = comp.node(id);
        for (auto it = n->inputs.begin(); it != n->inputs.end();) {
            if (map.contains(it->nodeId)) {
                it->nodeId = map.value(it->nodeId);
                ++it;
            } else {
                it = n->inputs.erase(it);
            }
        }
    }
    return added;
}

bool swapMainInputs(Composition& comp, quint64 id)
{
    CompNode* n = comp.node(id);
    const NodeTypeInfo* info = n ? nodeType(n->type) : nullptr;
    const QString bg = QStringLiteral("background"), fg = QStringLiteral("foreground");
    if (!info || info->inputIndex(bg) < 0 || info->inputIndex(fg) < 0)
        return false;
    const NodeLink a = n->inputs.value(bg), b = n->inputs.value(fg);
    if (!a.isConnected() && !b.isConnected())
        return false;
    n->inputs.remove(bg);
    n->inputs.remove(fg);
    if (b.isConnected())
        n->inputs.insert(bg, b);
    if (a.isConnected())
        n->inputs.insert(fg, a);
    return true;
}

bool reachesOutput(const Composition& comp, quint64 id)
{
    return comp.mediaOutId != 0 && isUpstream(comp, comp.mediaOutId, id);
}

// ---- Project access ----

std::optional<Composition> clipComposition(const Project& project, quint64 clipId)
{
    if (!clipExists(project, clipId))
        return std::nullopt;
    return stored(project, clipId);
}

bool hasComposition(const Project& project, quint64 clipId)
{
    return clipComposition(project, clipId).has_value();
}

bool setClipComposition(Project& project, quint64 clipId, const std::optional<Composition>& comp,
    const QString& label)
{
    if (!clipExists(project, clipId))
        return false;
    const std::optional<Composition> before = stored(project, clipId);
    if (before == comp)
        return false;
    project.undoStack()->push(new CompositionCommand(
        project, project.activeTimelineUid(), clipId, before, comp, label));
    return true;
}

int resetCompositions(Project& project, const QVector<quint64>& clipIds)
{
    QVector<quint64> with;
    for (quint64 id : clipIds)
        if (hasComposition(project, id) && !with.contains(id))
            with.append(id);
    if (with.isEmpty())
        return 0;
    const QString label = QCoreApplication::translate("kk::compadapter", "Reset Composition");
    project.undoStack()->beginMacro(label);
    for (quint64 id : with)
        setClipComposition(project, id, std::nullopt, label);
    project.undoStack()->endMacro();
    return int(with.size());
}

void beginCompEdit(Project& project, const QString& label)
{
    Store& s = storeFor(project);
    if (s.gestureDepth++ == 0) {
        s.gestureLabel = label;
        s.gestureBefore.clear();
    }
}

void endCompEdit(Project& project)
{
    Store& s = storeFor(project);
    if (s.gestureDepth <= 0 || --s.gestureDepth > 0)
        return;
    const auto before = std::exchange(s.gestureBefore, {});
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        const std::optional<Composition> now = stored(project, it.key());
        if (now == it.value())
            continue;
        // The store already holds the result; the command's first redo writes it again.
        project.undoStack()->push(new CompositionCommand(project, project.activeTimelineUid(),
            it.key(), it.value(), now, s.gestureLabel));
    }
}

bool modifyClipComposition(Project& project, quint64 clipId, const Composition& comp)
{
    if (!clipExists(project, clipId))
        return false;
    Store& s = storeFor(project);
    const std::optional<Composition> before = stored(project, clipId);
    if (before == comp)
        return false;
    if (s.gestureDepth <= 0)
        return setClipComposition(project, clipId, comp, s.gestureLabel);
    if (!s.gestureBefore.contains(clipId))
        s.gestureBefore.insert(clipId, before);
    writeStore(project, project.activeTimelineUid(), clipId, comp);
    return true;
}

} // namespace kk::compadapter
