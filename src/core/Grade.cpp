// SPDX-License-Identifier: GPL-3.0-or-later
// Grade — key defaults, node helpers and the graph operations of the Color page node editor.
#include "core/Grade.h"

#include <QCoreApplication>
#include <QHash>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

/// Points closer than this to the neutral line count as neutral (floating point noise of
/// dragged points).
constexpr double kNeutralEpsilon = 1e-6;
/// Horizontal / vertical spacing of automatically placed nodes (node editor units).
constexpr double kNodeSpacingX = 140.0;
constexpr double kNodeSpacingY = 90.0;

} // namespace

bool GradeCurves::isIdentity(GradeCurve c) const
{
    const double neutral = curveNeutral(c);
    for (const CurvePoint& p : points[size_t(c)]) {
        const double want = neutral < 0 ? p.x : neutral;
        if (std::abs(p.y - want) > kNeutralEpsilon)
            return false;
    }
    return true;
}

bool GradeCurves::isIdentity() const
{
    for (int i = 0; i < kGradeCurveCount; ++i) {
        if (!isIdentity(GradeCurve(i)))
            return false;
    }
    return true;
}

namespace gradewindowkeys {

double defaultValue(QStringView key)
{
    if (key == CenterX || key == CenterY || key == Width || key == Height)
        return 0.5;
    if (key == SoftOut)
        return 0.05;
    if (key == Opacity)
        return 1.0;
    return 0.0;
}

const QVector<QString>& allKeys()
{
    static const QVector<QString> keys
        = {CenterX, CenterY, Width, Height, Rotation, SoftIn, SoftOut, Opacity};
    return keys;
}

QString label(const QString& key)
{
    const std::pair<const QString*, const char*> named[] = {
        {&CenterX, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Center X")},
        {&CenterY, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Center Y")},
        {&Width, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Width")},
        {&Height, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Height")},
        {&Rotation, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Rotation")},
        {&SoftIn, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Inside Softness")},
        {&SoftOut, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Outside Softness")},
        {&Opacity, QT_TRANSLATE_NOOP("kk::gradewindowkeys", "Opacity")},
    };
    for (const auto& [k, text] : named) {
        if (key == *k)
            return QCoreApplication::translate("kk::gradewindowkeys", text);
    }
    return key;
}

} // namespace gradewindowkeys

bool GradeWindow::isRenderable() const
{
    return enabled && (shape != GradeWindowShape::Polygon || points.size() >= 3);
}

QString gradeWindowShapeToString(GradeWindowShape shape)
{
    switch (shape) {
    case GradeWindowShape::Circle:
        return QStringLiteral("circle");
    case GradeWindowShape::Linear:
        return QStringLiteral("linear");
    case GradeWindowShape::Polygon:
        return QStringLiteral("polygon");
    case GradeWindowShape::Gradient:
        return QStringLiteral("gradient");
    }
    return QStringLiteral("circle");
}

GradeWindowShape gradeWindowShapeFromString(const QString& text)
{
    if (text == QLatin1String("linear"))
        return GradeWindowShape::Linear;
    if (text == QLatin1String("polygon"))
        return GradeWindowShape::Polygon;
    if (text == QLatin1String("gradient"))
        return GradeWindowShape::Gradient;
    return GradeWindowShape::Circle;
}

namespace gradekeys {

const QString& wheelKey(Wheel wheel, int channel)
{
    static const QString* const table[4][4] = {
        {&LiftR, &LiftG, &LiftB, &LiftY},
        {&GammaR, &GammaG, &GammaB, &GammaY},
        {&GainR, &GainG, &GainB, &GainY},
        {&OffsetR, &OffsetG, &OffsetB, &OffsetY},
    };
    return *table[std::clamp(int(wheel), 0, 3)][std::clamp(channel, 0, 3)];
}

double defaultValue(QStringView key)
{
    if (key.startsWith(u"gain"))
        return 1.0;
    if (key == Contrast || key == Saturation || key == LumMix || key == Mix)
        return 1.0;
    if (key == Pivot)
        return 0.435; // Resolve's default contrast pivot (log-ish mid grey)
    return 0.0;
}

const QVector<QString>& allKeys()
{
    static const QVector<QString> keys = {LiftR, LiftG, LiftB, LiftY, GammaR, GammaG, GammaB,
        GammaY, GainR, GainG, GainB, GainY, OffsetR, OffsetG, OffsetB, OffsetY, Contrast, Pivot,
        Saturation, Hue, Temperature, Tint, LumMix, Mix};
    return keys;
}

QString label(const QString& key)
{
    static const char* const wheelNames[4] = {QT_TRANSLATE_NOOP("kk::gradekeys", "Lift"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "Gamma"), QT_TRANSLATE_NOOP("kk::gradekeys", "Gain"),
        QT_TRANSLATE_NOOP("kk::gradekeys", "Offset")};
    static const char* const channels[4] = {"R", "G", "B", "Y"};
    for (int w = 0; w < 4; ++w) {
        for (int ch = 0; ch < 4; ++ch) {
            if (key == wheelKey(Wheel(w), ch))
                return QCoreApplication::translate("kk::gradekeys", wheelNames[w])
                    + QLatin1Char(' ') + QLatin1String(channels[ch]);
        }
    }
    const std::pair<const QString*, const char*> named[] = {
        {&Contrast, QT_TRANSLATE_NOOP("kk::gradekeys", "Contrast")},
        {&Pivot, QT_TRANSLATE_NOOP("kk::gradekeys", "Pivot")},
        {&Saturation, QT_TRANSLATE_NOOP("kk::gradekeys", "Saturation")},
        {&Hue, QT_TRANSLATE_NOOP("kk::gradekeys", "Hue")},
        {&Temperature, QT_TRANSLATE_NOOP("kk::gradekeys", "Temperature")},
        {&Tint, QT_TRANSLATE_NOOP("kk::gradekeys", "Tint")},
        {&LumMix, QT_TRANSLATE_NOOP("kk::gradekeys", "Lum Mix")},
        {&Mix, QT_TRANSLATE_NOOP("kk::gradekeys", "Key Output Mix")},
    };
    for (const auto& [k, text] : named) {
        if (key == *k)
            return QCoreApplication::translate("kk::gradekeys", text);
    }
    return key;
}

std::pair<double, double> range(const QString& key)
{
    if (key.startsWith(QLatin1String("gain")))
        return {0.0, 4.0};
    if (key.startsWith(QLatin1String("lift")) || key.startsWith(QLatin1String("gamma"))
        || key.startsWith(QLatin1String("offset")))
        return {-1.0, 1.0};
    if (key == Contrast || key == Saturation)
        return {0.0, 4.0};
    if (key == Pivot || key == LumMix || key == Mix)
        return {0.0, 1.0};
    if (key == Hue)
        return {-180.0, 180.0};
    return {-1.0, 1.0}; // temperature, tint
}

} // namespace gradekeys

bool GradeNode::hasKey() const
{
    if (isMixer())
        return false;
    if (qualifier.enabled)
        return true;
    return std::any_of(
        windows.cbegin(), windows.cend(), [](const GradeWindow& w) { return w.isRenderable(); });
}

bool GradeNode::isAnimated() const
{
    return params.isAnimated()
        || std::any_of(windows.cbegin(), windows.cend(),
            [](const GradeWindow& w) { return w.params.isAnimated(); });
}

void GradeNode::shiftKeys(int64_t deltaFrames)
{
    params.shiftKeys(deltaFrames);
    for (GradeWindow& w : windows)
        w.params.shiftKeys(deltaFrames);
}

quint64 ClipGrade::allocateId() const
{
    quint64 id = 0;
    for (const GradeNode& n : nodes) {
        id = std::max(id, n.id);
        for (const GradeWindow& w : n.windows)
            id = std::max(id, w.id);
    }
    return id + 1;
}

const GradeWindow* ClipGrade::window(quint64 id, const GradeNode** owner) const
{
    for (const GradeNode& n : nodes) {
        for (const GradeWindow& w : n.windows) {
            if (w.id == id) {
                if (owner)
                    *owner = &n;
                return &w;
            }
        }
    }
    return nullptr;
}

GradeWindow* ClipGrade::window(quint64 id, GradeNode** owner)
{
    for (GradeNode& n : nodes) {
        for (GradeWindow& w : n.windows) {
            if (w.id == id) {
                if (owner)
                    *owner = &n;
                return &w;
            }
        }
    }
    return nullptr;
}

bool ClipGrade::isAnimated() const
{
    return std::any_of(
        nodes.cbegin(), nodes.cend(), [](const GradeNode& n) { return n.isAnimated(); });
}

void ClipGrade::shiftKeys(int64_t deltaFrames)
{
    for (GradeNode& n : nodes)
        n.shiftKeys(deltaFrames);
}

int ClipGrade::indexOf(quint64 id) const
{
    for (int i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == id)
            return i;
    }
    return -1;
}

const GradeNode* ClipGrade::node(quint64 id) const
{
    const int i = indexOf(id);
    return i < 0 ? nullptr : &nodes[i];
}

GradeNode* ClipGrade::node(quint64 id)
{
    const int i = indexOf(id);
    return i < 0 ? nullptr : &nodes[i];
}

QString gradeNodeName(const ClipGrade& grade, int index)
{
    if (index >= 0 && index < grade.nodes.size() && !grade.nodes[index].label.isEmpty())
        return grade.nodes[index].label;
    return QStringLiteral("%1").arg(index + 1, 2, 10, QLatin1Char('0'));
}

QString gradeNodeTypeToString(GradeNodeType type)
{
    switch (type) {
    case GradeNodeType::Corrector:
        return QStringLiteral("corrector");
    case GradeNodeType::Parallel:
        return QStringLiteral("parallel");
    case GradeNodeType::Layer:
        return QStringLiteral("layer");
    }
    return QStringLiteral("corrector");
}

GradeNodeType gradeNodeTypeFromString(const QString& text)
{
    if (text == QLatin1String("parallel"))
        return GradeNodeType::Parallel;
    if (text == QLatin1String("layer"))
        return GradeNodeType::Layer;
    return GradeNodeType::Corrector;
}

namespace gradeops {

namespace {

/// Post-order DFS from node @p index (inputs first). @p state: 0 unvisited, 1 on the stack,
/// 2 done; a node met while on the stack closes a cycle and is skipped.
void visit(const ClipGrade& g, int index, std::vector<int>& state, std::vector<int>& out)
{
    if (index < 0 || state[size_t(index)] != 0)
        return;
    state[size_t(index)] = 1;
    for (const quint64 in : g.nodes[index].inputs) {
        if (in != 0)
            visit(g, g.indexOf(in), state, out);
    }
    state[size_t(index)] = 2;
    out.push_back(index);
}

/// Makes every reference to node @p oldId (consumer inputs and the output) refer to @p newId,
/// except in node @p except.
void redirect(ClipGrade& g, quint64 oldId, quint64 newId, quint64 except)
{
    for (GradeNode& n : g.nodes) {
        if (n.id == except)
            continue;
        for (quint64& in : n.inputs) {
            if (in == oldId)
                in = newId;
        }
    }
    if (g.output == oldId)
        g.output = newId;
}

/// Adds a corrector parallel to / layered over @p nodeId through a mixer of @p type.
quint64 addMixed(ClipGrade& g, quint64 nodeId, GradeNodeType type)
{
    if (g.isEmpty())
        initialize(g);
    GradeNode* base = g.node(nodeId);
    if (!base) {
        const std::vector<int> order = evaluationOrder(g);
        base = order.empty() ? &g.nodes.back() : &g.nodes[order.back()];
    }
    const quint64 baseId = base->id;
    const quint64 source = base->inputs.value(0, 0);
    // An existing mixer of this type directly after the node takes one more input.
    for (GradeNode& n : g.nodes) {
        if (n.type == type && n.inputs.contains(baseId)) {
            GradeNode extra;
            extra.id = g.allocateId();
            extra.inputs = {source};
            const quint64 mixerId = n.id;
            g.nodes.append(extra);
            g.node(mixerId)->inputs.append(extra.id);
            placeNode(g, extra.id);
            g.nodes.last().pos.ry() += kNodeSpacingY * (g.node(mixerId)->inputs.size() - 1);
            return extra.id;
        }
    }
    GradeNode extra;
    extra.id = g.allocateId();
    extra.inputs = {source};
    GradeNode mixer;
    mixer.id = extra.id + 1;
    mixer.type = type;
    mixer.inputs = {baseId, extra.id};
    const QPointF basePos = base->pos;
    redirect(g, baseId, mixer.id, 0);
    extra.pos = basePos + QPointF(0, kNodeSpacingY);
    mixer.pos = basePos + QPointF(kNodeSpacingX, kNodeSpacingY * 0.5);
    g.nodes.append(extra);
    g.nodes.append(mixer);
    return extra.id;
}

} // namespace

std::vector<int> evaluationOrder(const ClipGrade& grade)
{
    std::vector<int> out;
    if (grade.output == 0)
        return out;
    std::vector<int> state(size_t(grade.nodes.size()), 0);
    visit(grade, grade.indexOf(grade.output), state, out);
    return out;
}

bool isNodeIdentity(const GradeNode& node, int64_t localFrame)
{
    if (node.isMixer())
        return false;
    if (!node.enabled || node.value(gradekeys::Mix, localFrame) <= 0.0)
        return true;
    if (!node.lutPath.isEmpty() || !node.curves.isIdentity())
        return false;
    for (const QString& key : gradekeys::allKeys()) {
        if (key == gradekeys::Pivot || key == gradekeys::LumMix || key == gradekeys::Mix)
            continue; // only matter together with another value
        if (node.value(key, localFrame) != gradekeys::defaultValue(key))
            return false;
    }
    return true;
}

bool isIdentity(const ClipGrade& grade, int64_t localFrame)
{
    const std::vector<int> order = evaluationOrder(grade);
    if (order.empty())
        return true;
    // Mixers of identity inputs average / layer identical pictures: identity as well.
    return std::all_of(order.begin(), order.end(), [&](int i) {
        const GradeNode& n = grade.nodes[i];
        return n.isMixer() || isNodeIdentity(n, localFrame);
    });
}

quint64 initialize(ClipGrade& grade)
{
    grade.nodes.clear();
    GradeNode n;
    n.id = 1;
    n.inputs = {0};
    grade.nodes.append(n);
    grade.output = n.id;
    return n.id;
}

quint64 addSerialNode(ClipGrade& grade, quint64 after)
{
    if (grade.isEmpty())
        return initialize(grade);
    GradeNode* prev = grade.node(after);
    if (!prev) {
        const std::vector<int> order = evaluationOrder(grade);
        prev = order.empty() ? &grade.nodes.back() : &grade.nodes[order.back()];
    }
    GradeNode n;
    n.id = grade.allocateId();
    n.inputs = {prev->id};
    n.pos = prev->pos + QPointF(kNodeSpacingX, 0);
    const quint64 prevId = prev->id;
    redirect(grade, prevId, n.id, 0);
    grade.nodes.append(n);
    // Nodes downstream that sat where the new one goes move one step right.
    for (GradeNode& other : grade.nodes) {
        if (other.id != n.id && other.pos.x() >= n.pos.x() - 1e-9 && reaches(grade, other.id, n.id))
            other.pos.rx() += kNodeSpacingX;
    }
    return n.id;
}

quint64 addParallelNode(ClipGrade& grade, quint64 node)
{
    return addMixed(grade, node, GradeNodeType::Parallel);
}

quint64 addLayerNode(ClipGrade& grade, quint64 node)
{
    return addMixed(grade, node, GradeNodeType::Layer);
}

bool removeNode(ClipGrade& grade, quint64 id)
{
    const int index = grade.indexOf(id);
    if (index < 0)
        return false;
    const quint64 replacement = grade.nodes[index].inputs.value(0, 0);
    grade.nodes.remove(index);
    // A mixer branch whose input another branch of the same mixer already carries (a deleted
    // parallel / layer node) leaves the mixer instead of feeding it the ungraded picture.
    for (GradeNode& n : grade.nodes) {
        if (!n.isMixer() || !n.inputs.contains(id))
            continue;
        const bool shared = std::any_of(n.inputs.begin(), n.inputs.end(),
            [&](quint64 in) { return in != id && reaches(grade, in, replacement); });
        if (shared)
            n.inputs.removeAll(id);
    }
    redirect(grade, id, replacement, 0);
    // Tidy the mixers: duplicate inputs merge, a mixer with a single input dissolves.
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < grade.nodes.size(); ++i) {
            GradeNode& n = grade.nodes[i];
            if (!n.isMixer())
                continue;
            QVector<quint64> unique;
            for (const quint64 in : n.inputs) {
                if (!unique.contains(in))
                    unique.append(in);
            }
            n.inputs = unique;
            if (n.inputs.size() <= 1) {
                const quint64 mixerId = n.id;
                const quint64 only = n.inputs.value(0, 0);
                grade.nodes.remove(i);
                redirect(grade, mixerId, only, 0);
                changed = true;
                break;
            }
        }
    }
    if (grade.output != 0 && grade.indexOf(grade.output) < 0)
        grade.output = 0;
    return true;
}

bool reaches(const ClipGrade& grade, quint64 of, quint64 id)
{
    if (of == 0)
        return id == 0;
    QVector<quint64> stack {of};
    QVector<quint64> seen;
    while (!stack.isEmpty()) {
        const quint64 cur = stack.takeLast();
        if (cur == id)
            return true;
        if (cur == 0 || seen.contains(cur))
            continue;
        seen.append(cur);
        if (const GradeNode* n = grade.node(cur))
            stack += n->inputs;
    }
    return false;
}

bool connect(ClipGrade& grade, quint64 from, quint64 to, int inputIndex)
{
    if (from != 0 && !grade.node(from))
        return false;
    if (to == 0) {
        grade.output = from;
        return true;
    }
    GradeNode* target = grade.node(to);
    if (!target || from == to || (from != 0 && reaches(grade, from, to)))
        return false; // to is upstream of from: the edge would close a cycle
    if (!target->isMixer()) {
        target->inputs = {from};
        return true;
    }
    if (inputIndex >= 0 && inputIndex < target->inputs.size()) {
        target->inputs[inputIndex] = from;
    } else {
        if (target->inputs.contains(from))
            return false;
        target->inputs.append(from);
    }
    return true;
}

bool disconnect(ClipGrade& grade, quint64 to, int inputIndex)
{
    if (to == 0) {
        if (grade.output == 0)
            return false;
        grade.output = 0;
        return true;
    }
    GradeNode* target = grade.node(to);
    if (!target || inputIndex < 0 || inputIndex >= target->inputs.size())
        return false;
    if (!target->isMixer()) {
        if (target->inputs[0] == 0)
            return false;
        target->inputs = {0};
        return true;
    }
    if (target->inputs.size() <= 1)
        return false;
    target->inputs.remove(inputIndex);
    return true;
}

bool resetNode(ClipGrade& grade, quint64 id)
{
    GradeNode* n = grade.node(id);
    if (!n)
        return false;
    n->params = ParamSet {};
    n->curves = GradeCurves {};
    n->qualifier = GradeQualifier {};
    n->windows.clear();
    n->lutPath.clear();
    n->lutInterpolation = LutInterpolation::Tetrahedral;
    return true;
}

bool setNodeEnabled(ClipGrade& grade, quint64 id, bool enabled)
{
    GradeNode* n = grade.node(id);
    if (!n || n->enabled == enabled)
        return false;
    n->enabled = enabled;
    return true;
}

quint64 addSerialNodeBefore(ClipGrade& grade, quint64 before)
{
    GradeNode* next = grade.node(before);
    if (grade.isEmpty() || !next)
        return addSerialNode(grade, before);
    GradeNode n;
    n.id = grade.allocateId();
    n.inputs = {next->inputs.value(0, 0)};
    n.pos = next->pos;
    next->inputs[0] = n.id;
    const quint64 nextId = next->id;
    grade.nodes.append(n);
    // The node it feeds and everything after it move one step right.
    for (GradeNode& other : grade.nodes) {
        if (other.id != n.id && reaches(grade, other.id, nextId))
            other.pos.rx() += kNodeSpacingX;
    }
    return n.id;
}

quint64 addWindow(ClipGrade& grade, quint64 nodeId, GradeWindowShape shape)
{
    GradeNode* n = grade.node(nodeId);
    if (!n || n->isMixer() || n->windows.size() >= kMaxGradeWindows)
        return 0;
    GradeWindow w;
    w.id = grade.allocateId();
    w.shape = shape;
    if (shape == GradeWindowShape::Polygon)
        w.points = {QPointF(0, -0.5), QPointF(0.5, 0), QPointF(0, 0.5), QPointF(-0.5, 0)};
    if (shape == GradeWindowShape::Gradient)
        w.params.setBase(gradewindowkeys::SoftOut, 0.25);
    n->windows.append(w);
    return w.id;
}

bool removeWindow(ClipGrade& grade, quint64 windowId)
{
    for (GradeNode& n : grade.nodes) {
        for (int i = 0; i < n.windows.size(); ++i) {
            if (n.windows[i].id == windowId) {
                n.windows.remove(i);
                return true;
            }
        }
    }
    return false;
}

quint64 defaultSelection(const ClipGrade& grade)
{
    if (grade.isEmpty())
        return 0;
    if (grade.node(grade.output))
        return grade.output;
    return grade.nodes.back().id;
}

void placeNode(ClipGrade& grade, quint64 id)
{
    GradeNode* n = grade.node(id);
    if (!n)
        return;
    const GradeNode* in = grade.node(n->inputs.value(0, 0));
    n->pos = in ? in->pos + QPointF(kNodeSpacingX, 0) : QPointF(0, 0);
}

} // namespace gradeops

} // namespace kk
