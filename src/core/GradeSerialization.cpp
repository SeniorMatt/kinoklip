// SPDX-License-Identifier: GPL-3.0-or-later
// GradeSerialization — JSON form of grades (project format version 16, see
// core/Serialization.h). Only values that differ from the defaults are written.
#include "core/Serialization.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>

namespace kk::serialization {

namespace {

/// JSON keys of the curves, in GradeCurve order.
const char* const kCurveNames[kGradeCurveCount]
    = {"luma", "red", "green", "blue", "hueHue", "hueSat", "hueLum", "lumSat", "satSat"};

QJsonObject qualifierToJson(const GradeQualifier& q)
{
    const GradeQualifier d;
    QJsonObject o;
    auto flag = [&](const char* key, bool v, bool def) {
        if (v != def)
            o[QLatin1String(key)] = v;
    };
    auto num = [&](const char* key, double v, double def) {
        if (v != def)
            o[QLatin1String(key)] = v;
    };
    flag("enabled", q.enabled, d.enabled);
    flag("invert", q.invert, d.invert);
    flag("useHue", q.useHue, d.useHue);
    flag("useSat", q.useSat, d.useSat);
    flag("useLum", q.useLum, d.useLum);
    num("hueCenter", q.hueCenter, d.hueCenter);
    num("hueWidth", q.hueWidth, d.hueWidth);
    num("hueSoft", q.hueSoft, d.hueSoft);
    num("satLow", q.satLow, d.satLow);
    num("satHigh", q.satHigh, d.satHigh);
    num("satSoft", q.satSoft, d.satSoft);
    num("lumLow", q.lumLow, d.lumLow);
    num("lumHigh", q.lumHigh, d.lumHigh);
    num("lumSoft", q.lumSoft, d.lumSoft);
    return o;
}

GradeQualifier qualifierFromJson(const QJsonObject& o)
{
    GradeQualifier q;
    auto flag = [&](const char* key, bool& v) { v = o.value(QLatin1String(key)).toBool(v); };
    auto num = [&](const char* key, double& v) { v = o.value(QLatin1String(key)).toDouble(v); };
    flag("enabled", q.enabled);
    flag("invert", q.invert);
    flag("useHue", q.useHue);
    flag("useSat", q.useSat);
    flag("useLum", q.useLum);
    num("hueCenter", q.hueCenter);
    num("hueWidth", q.hueWidth);
    num("hueSoft", q.hueSoft);
    num("satLow", q.satLow);
    num("satHigh", q.satHigh);
    num("satSoft", q.satSoft);
    num("lumLow", q.lumLow);
    num("lumHigh", q.lumHigh);
    num("lumSoft", q.lumSoft);
    return q;
}

QJsonObject curvesToJson(const GradeCurves& curves)
{
    QJsonObject o;
    for (int c = 0; c < kGradeCurveCount; ++c) {
        const CurvePoints& points = curves.points[size_t(c)];
        if (points.isEmpty())
            continue;
        QJsonArray list;
        for (const CurvePoint& p : points)
            list.append(QJsonArray {p.x, p.y});
        o[QLatin1String(kCurveNames[c])] = list;
    }
    return o;
}

GradeCurves curvesFromJson(const QJsonObject& o)
{
    GradeCurves curves;
    for (int c = 0; c < kGradeCurveCount; ++c) {
        CurvePoints& points = curves.points[size_t(c)];
        for (const QJsonValue& v : o.value(QLatin1String(kCurveNames[c])).toArray()) {
            const QJsonArray a = v.toArray();
            if (a.size() < 2)
                continue;
            points.append(
                {std::clamp(a[0].toDouble(), 0.0, 1.0), std::clamp(a[1].toDouble(), 0.0, 1.0)});
        }
        std::sort(points.begin(), points.end(),
            [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    }
    return curves;
}

QJsonObject windowToJson(const GradeWindow& w)
{
    QJsonObject o {{QLatin1String("id"), qint64(w.id)},
        {QLatin1String("shape"), gradeWindowShapeToString(w.shape)}};
    if (!w.enabled)
        o[QLatin1String("enabled")] = false;
    if (w.invert)
        o[QLatin1String("invert")] = true;
    if (w.params != ParamSet {})
        o[QLatin1String("params")] = paramSetToJson(w.params);
    if (!w.points.isEmpty()) {
        QJsonArray points;
        for (const QPointF& p : w.points)
            points.append(QJsonArray {p.x(), p.y()});
        o[QLatin1String("points")] = points;
    }
    return o;
}

GradeWindow windowFromJson(const QJsonObject& o)
{
    GradeWindow w;
    w.id = quint64(o.value(QLatin1String("id")).toInteger());
    w.shape = gradeWindowShapeFromString(o.value(QLatin1String("shape")).toString());
    w.enabled = o.value(QLatin1String("enabled")).toBool(true);
    w.invert = o.value(QLatin1String("invert")).toBool(false);
    w.params = paramSetFromJson(o.value(QLatin1String("params")).toObject());
    for (const QJsonValue& v : o.value(QLatin1String("points")).toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() >= 2)
            w.points.append(QPointF(a[0].toDouble(), a[1].toDouble()));
    }
    return w;
}

} // namespace

QJsonObject gradeToJson(const ClipGrade& grade)
{
    QJsonArray nodes;
    for (const GradeNode& n : grade.nodes) {
        QJsonObject o {{QLatin1String("id"), qint64(n.id)},
            {QLatin1String("type"), gradeNodeTypeToString(n.type)},
            {QLatin1String("pos"), QJsonArray {n.pos.x(), n.pos.y()}}};
        QJsonArray inputs;
        for (const quint64 in : n.inputs)
            inputs.append(qint64(in));
        o[QLatin1String("inputs")] = inputs;
        if (!n.label.isEmpty())
            o[QLatin1String("label")] = n.label;
        if (!n.enabled)
            o[QLatin1String("enabled")] = false;
        if (n.params != ParamSet {})
            o[QLatin1String("params")] = paramSetToJson(n.params);
        if (n.curves != GradeCurves {})
            o[QLatin1String("curves")] = curvesToJson(n.curves);
        if (n.qualifier != GradeQualifier {})
            o[QLatin1String("qualifier")] = qualifierToJson(n.qualifier);
        if (!n.windows.isEmpty()) {
            QJsonArray windows;
            for (const GradeWindow& w : n.windows)
                windows.append(windowToJson(w));
            o[QLatin1String("windows")] = windows;
        }
        if (!n.lutPath.isEmpty())
            o[QLatin1String("lut")] = n.lutPath;
        if (n.lutInterpolation == LutInterpolation::Trilinear)
            o[QLatin1String("lutInterp")] = QStringLiteral("trilinear");
        nodes.append(o);
    }
    return QJsonObject {
        {QLatin1String("nodes"), nodes}, {QLatin1String("output"), qint64(grade.output)}};
}

ClipGrade gradeFromJson(const QJsonObject& json)
{
    ClipGrade g;
    QSet<quint64> used;
    for (const QJsonValue& v : json.value(QLatin1String("nodes")).toArray()) {
        const QJsonObject o = v.toObject();
        GradeNode n;
        n.id = quint64(o.value(QLatin1String("id")).toInteger());
        if (n.id == 0 || used.contains(n.id))
            continue; // damaged / hand-edited file: drop the node rather than guess
        used.insert(n.id);
        n.type = gradeNodeTypeFromString(o.value(QLatin1String("type")).toString());
        n.label = o.value(QLatin1String("label")).toString();
        n.enabled = o.value(QLatin1String("enabled")).toBool(true);
        n.params = paramSetFromJson(o.value(QLatin1String("params")).toObject());
        n.curves = curvesFromJson(o.value(QLatin1String("curves")).toObject());
        n.qualifier = qualifierFromJson(o.value(QLatin1String("qualifier")).toObject());
        for (const QJsonValue& w : o.value(QLatin1String("windows")).toArray())
            n.windows.append(windowFromJson(w.toObject()));
        n.lutPath = o.value(QLatin1String("lut")).toString();
        n.lutInterpolation
            = o.value(QLatin1String("lutInterp")).toString() == QLatin1String("trilinear")
            ? LutInterpolation::Trilinear
            : LutInterpolation::Tetrahedral;
        n.inputs.clear();
        for (const QJsonValue& in : o.value(QLatin1String("inputs")).toArray())
            n.inputs.append(quint64(in.toInteger()));
        if (n.inputs.isEmpty() || (!n.isMixer() && n.inputs.size() > 1))
            n.inputs.resize(1);
        const QJsonArray pos = o.value(QLatin1String("pos")).toArray();
        n.pos = QPointF(pos.at(0).toDouble(), pos.at(1).toDouble());
        g.nodes.append(n);
    }
    // Inputs naming nodes that do not exist read as the source; window ids must be unique
    // within the grade (they share the node id space).
    for (GradeNode& n : g.nodes) {
        for (quint64& in : n.inputs) {
            if (in != 0 && !used.contains(in))
                in = 0;
        }
    }
    quint64 next = g.allocateId();
    QSet<quint64> windowIds;
    for (GradeNode& n : g.nodes) {
        for (GradeWindow& w : n.windows) {
            if (w.id == 0 || used.contains(w.id) || windowIds.contains(w.id))
                w.id = next++;
            windowIds.insert(w.id);
        }
    }
    g.output = quint64(json.value(QLatin1String("output")).toInteger());
    if (g.output != 0 && !used.contains(g.output))
        g.output = 0;
    return g;
}

} // namespace kk::serialization
