// SPDX-License-Identifier: GPL-3.0-or-later
// Mask implementation: shape-key evaluation, names, mask transform, polygon offset, freehand
// simplification and point insertion / removal.
#include "core/Mask.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::Mask", text);
}

QPointF lerp(QPointF a, QPointF b, double t)
{
    return a + (b - a) * t;
}

double length(QPointF d)
{
    return std::sqrt(d.x() * d.x() + d.y() * d.y());
}

/// Miter length limit of offsetPolygon(), in multiples of the offset distance.
constexpr double kMiterLimit = 4.0;

} // namespace

// ---- AnimatedPath ----

void AnimatedPath::evaluate(double localFrame, MaskPath& out) const
{
    if (shapes.isEmpty()) {
        out.clear();
        return;
    }
    const qsizetype n = std::min<qsizetype>(timing.keys.size(), shapes.size());
    if (n <= 1) {
        out = shapes[0]; // implicitly shared: no copy of the points
        return;
    }
    // timing's key values are the shape indices, so its value is "index + progress" with the
    // keys' easing applied (see the class comment).
    const double v = timing.valueAtTime(localFrame);
    const qsizetype i = std::clamp<qsizetype>(qsizetype(std::floor(v)), 0, n - 2);
    const double t = v - double(i);
    const MaskPath& a = shapes[i];
    const MaskPath& b = shapes[i + 1];
    if (t == 0.0 || a.size() != b.size()) {
        out = (t < 0.5 || a.size() != b.size()) ? a : b;
        return;
    }
    if (t == 1.0) {
        out = b;
        return;
    }
    out.resize(a.size());
    for (qsizetype k = 0; k < a.size(); ++k) {
        const MaskPoint& p = a[k];
        const MaskPoint& q = b[k];
        MaskPoint& r = out[k];
        r.x = p.x + (q.x - p.x) * t;
        r.y = p.y + (q.y - p.y) * t;
        r.inX = p.inX + (q.inX - p.inX) * t;
        r.inY = p.inY + (q.inY - p.inY) * t;
        r.outX = p.outX + (q.outX - p.outX) * t;
        r.outY = p.outY + (q.outY - p.outY) * t;
        r.smooth = t < 0.5 ? p.smooth : q.smooth;
    }
}

MaskPath AnimatedPath::at(double localFrame) const
{
    MaskPath out;
    evaluate(localFrame, out);
    return out;
}

void AnimatedPath::setShape(int64_t localFrame, const MaskPath& path)
{
    if (isAnimated())
        setKey(localFrame, path);
    else if (shapes.isEmpty())
        shapes = {path};
    else
        shapes[0] = path;
}

void AnimatedPath::setKey(int64_t localFrame, std::optional<MaskPath> path)
{
    MaskPath shape = path ? std::move(*path) : at(double(localFrame));
    const qsizetype existing = timing.indexOf(localFrame);
    if (existing >= 0 && existing < shapes.size() && isAnimated()) {
        shapes[existing] = std::move(shape);
        return;
    }
    // Append the shape and point a new key at it; normalize() then puts it in frame order
    // (and drops the static shape when this is the first key).
    shapes.append(std::move(shape));
    timing.setKey(localFrame, double(shapes.size() - 1));
    normalize();
}

void AnimatedPath::removeKey(int64_t localFrame)
{
    if (!timing.hasKeyAt(localFrame))
        return;
    timing.removeKey(localFrame); // the last key's value (= its index) becomes timing.base
    normalize();
}

void AnimatedPath::normalize()
{
    if (shapes.isEmpty())
        shapes = {MaskPath {}};
    auto pick = [this](double value) {
        return shapes[std::clamp<qsizetype>(qsizetype(std::lround(value)), 0, shapes.size() - 1)];
    };
    if (timing.keys.isEmpty()) {
        MaskPath keep = pick(timing.base);
        shapes = {std::move(keep)};
        timing.base = 0.0;
        return;
    }
    QVector<MaskPath> ordered;
    ordered.reserve(timing.keys.size());
    for (qsizetype i = 0; i < timing.keys.size(); ++i) {
        ordered.append(pick(timing.keys[i].value));
        timing.keys[i].value = double(i);
    }
    shapes = std::move(ordered);
    timing.base = 0.0;
}

// ---- Keys, names ----

double maskkeys::defaultValue(QStringView key)
{
    if (key == ScaleX || key == ScaleY)
        return 1.0;
    if (key == Opacity)
        return 100.0;
    if (key == Width)
        return 960.0;
    if (key == Height)
        return 540.0;
    return 0.0;
}

bool MaskData::isRenderable() const
{
    if (!enabled)
        return false;
    if (!isPathShape())
        return true;
    const qsizetype minPoints = shape == MaskShape::Bezier ? 2 : 3;
    return closed && path.pointCount() >= minPoints;
}

QString maskShapeToString(MaskShape shape)
{
    switch (shape) {
    case MaskShape::Rectangle:
        return QStringLiteral("rectangle");
    case MaskShape::Ellipse:
        return QStringLiteral("ellipse");
    case MaskShape::Polygon:
        return QStringLiteral("polygon");
    case MaskShape::Bezier:
        return QStringLiteral("bezier");
    case MaskShape::Freehand:
        return QStringLiteral("freehand");
    }
    return QStringLiteral("rectangle");
}

MaskShape maskShapeFromString(const QString& text)
{
    for (MaskShape s :
        {MaskShape::Ellipse, MaskShape::Polygon, MaskShape::Bezier, MaskShape::Freehand}) {
        if (text == maskShapeToString(s))
            return s;
    }
    return MaskShape::Rectangle;
}

QString maskModeToString(MaskMode mode)
{
    switch (mode) {
    case MaskMode::Add:
        return QStringLiteral("add");
    case MaskMode::Subtract:
        return QStringLiteral("subtract");
    case MaskMode::Intersect:
        return QStringLiteral("intersect");
    case MaskMode::Difference:
        return QStringLiteral("difference");
    }
    return QStringLiteral("add");
}

MaskMode maskModeFromString(const QString& text)
{
    for (MaskMode m : {MaskMode::Subtract, MaskMode::Intersect, MaskMode::Difference}) {
        if (text == maskModeToString(m))
            return m;
    }
    return MaskMode::Add;
}

QString maskShapeName(MaskShape shape)
{
    switch (shape) {
    case MaskShape::Rectangle:
        return tr("Rectangle");
    case MaskShape::Ellipse:
        return tr("Ellipse");
    case MaskShape::Polygon:
        return tr("Polygon");
    case MaskShape::Bezier:
        return tr("Bezier");
    case MaskShape::Freehand:
        return tr("Freehand");
    }
    return tr("Mask");
}

QString maskDisplayName(const MaskData& mask, int index)
{
    if (!mask.name.isEmpty())
        return mask.name;
    return QStringLiteral("%1 %2").arg(maskShapeName(mask.shape)).arg(index + 1);
}

MaskData makeMask(MaskShape shape, quint64 id, QSizeF defaultSize)
{
    MaskData m;
    m.id = id;
    m.shape = shape;
    if (m.isPathShape()) {
        m.closed = false; // drawn point by point in the viewer
    } else {
        m.params.setBase(maskkeys::Width, std::max(1.0, defaultSize.width()));
        m.params.setBase(maskkeys::Height, std::max(1.0, defaultSize.height()));
    }
    return m;
}

// ---- Geometry ----

QPointF maskToContent(const MaskData& mask, int64_t localFrame, QPointF local)
{
    const double sx = mask.value(maskkeys::ScaleX, localFrame);
    const double sy = mask.value(maskkeys::ScaleY, localFrame);
    const double rad = mask.value(maskkeys::Rotation, localFrame) * std::numbers::pi / 180.0;
    const double c = std::cos(rad);
    const double s = std::sin(rad);
    const double x = local.x() * sx;
    const double y = local.y() * sy;
    // y points down, so this standard rotation turns clockwise on screen.
    return {mask.value(maskkeys::PosX, localFrame) + c * x - s * y,
        mask.value(maskkeys::PosY, localFrame) + s * x + c * y};
}

std::optional<QPointF> contentToMask(const MaskData& mask, int64_t localFrame, QPointF content)
{
    const double sx = mask.value(maskkeys::ScaleX, localFrame);
    const double sy = mask.value(maskkeys::ScaleY, localFrame);
    if (std::abs(sx) < 1e-9 || std::abs(sy) < 1e-9)
        return std::nullopt;
    const double rad = mask.value(maskkeys::Rotation, localFrame) * std::numbers::pi / 180.0;
    const double c = std::cos(rad);
    const double s = std::sin(rad);
    const double dx = content.x() - mask.value(maskkeys::PosX, localFrame);
    const double dy = content.y() - mask.value(maskkeys::PosY, localFrame);
    return QPointF((c * dx + s * dy) / sx, (-s * dx + c * dy) / sy);
}

void offsetPolygon(QVector<QPointF>& poly, double distance)
{
    const qsizetype n = poly.size();
    if (n < 3 || distance == 0.0)
        return;
    // Shoelace sign: > 0 means the outward normal of edge e = (dx, dy) is (dy, -dx) in these
    // y-down coordinates; flip it for the other orientation.
    double area = 0.0;
    for (qsizetype i = 0; i < n; ++i) {
        const QPointF& a = poly[i];
        const QPointF& b = poly[(i + 1) % n];
        area += a.x() * b.y() - b.x() * a.y();
    }
    const double orient = area >= 0.0 ? 1.0 : -1.0;
    auto normal = [&](QPointF a, QPointF b) {
        const QPointF e = b - a;
        const double len = length(e);
        if (len < 1e-12)
            return QPointF();
        return QPointF(e.y(), -e.x()) * (orient / len);
    };
    // Compute every offset vertex from the ORIGINAL vertices before writing any of them.
    // Static thread-local scratch: this runs per mask per frame on the render thread.
    thread_local QVector<QPointF> moved;
    moved.resize(n);
    for (qsizetype i = 0; i < n; ++i) {
        const QPointF prev = poly[(i + n - 1) % n];
        const QPointF cur = poly[i];
        const QPointF next = poly[(i + 1) % n];
        const QPointF n1 = normal(prev, cur);
        const QPointF n2 = normal(cur, next);
        QPointF m = n1 + n2;
        const double mlen = length(m);
        if (mlen < 1e-9) {
            moved[i] = cur + n1 * distance; // a spike folding back: use one side
            continue;
        }
        m /= mlen;
        const double cosHalf = std::max(m.x() * n1.x() + m.y() * n1.y(), 1.0 / kMiterLimit);
        moved[i] = cur + m * (distance / cosHalf);
    }
    for (qsizetype i = 0; i < n; ++i)
        poly[i] = moved[i];
}

namespace {

// Ramer-Douglas-Peucker on stroke[first..last]: marks kept points.
void rdp(
    const QVector<QPointF>& s, qsizetype first, qsizetype last, double tol, QVector<bool>& keep)
{
    if (last <= first + 1)
        return;
    const QPointF a = s[first];
    const QPointF b = s[last];
    const QPointF ab = b - a;
    const double abLen = length(ab);
    double worst = -1.0;
    qsizetype worstIndex = -1;
    for (qsizetype i = first + 1; i < last; ++i) {
        const QPointF ap = s[i] - a;
        const double d
            = abLen < 1e-12 ? length(ap) : std::abs(ab.x() * ap.y() - ab.y() * ap.x()) / abLen;
        if (d > worst) {
            worst = d;
            worstIndex = i;
        }
    }
    if (worst > tol) {
        keep[worstIndex] = true;
        rdp(s, first, worstIndex, tol, keep);
        rdp(s, worstIndex, last, tol, keep);
    }
}

} // namespace

MaskPath simplifyStroke(const QVector<QPointF>& stroke, double tolerance)
{
    MaskPath out;
    if (stroke.size() < 3) {
        for (const QPointF& p : stroke)
            out.append(MaskPoint {p.x(), p.y()});
        return out;
    }
    QVector<bool> keep(stroke.size(), false);
    keep.front() = true;
    keep.back() = true;
    rdp(stroke, 0, stroke.size() - 1, std::max(tolerance, 0.1), keep);
    QVector<QPointF> pts;
    for (qsizetype i = 0; i < stroke.size(); ++i) {
        if (keep[i])
            pts.append(stroke[i]);
    }
    // The stroke is closed back to its start: drop an end point lying on the start point.
    if (pts.size() > 3 && length(pts.back() - pts.front()) < tolerance)
        pts.removeLast();
    if (pts.size() < 3) {
        // A nearly straight stroke: keep its two ends and its middle so the shape has area 0
        // but stays editable.
        pts = {stroke.front(), stroke[stroke.size() / 2], stroke.back()};
    }
    // Catmull-Rom tangents (closed): T_i = (P_{i+1} - P_{i-1}) / 6 gives the bezier control
    // points of the uniform Catmull-Rom spline through the kept points.
    const qsizetype n = pts.size();
    out.reserve(n);
    for (qsizetype i = 0; i < n; ++i) {
        const QPointF t = (pts[(i + 1) % n] - pts[(i + n - 1) % n]) / 6.0;
        MaskPoint p {pts[i].x(), pts[i].y(), -t.x(), -t.y(), t.x(), t.y(), true};
        out.append(p);
    }
    return out;
}

qsizetype insertPathPoint(MaskData& mask, qsizetype index, double t)
{
    const qsizetype count = mask.path.pointCount();
    if (count == 0 || index < 0 || index >= count)
        return -1;
    t = std::clamp(t, 0.0, 1.0);
    mask.path.forEachShape([&](MaskPath& path) {
        if (path.size() != count)
            return;
        MaskPoint& a = path[index];
        MaskPoint& b = path[(index + 1) % count];
        MaskPoint mid;
        if (!a.hasTangents() && !b.hasTangents()) {
            const QPointF p = lerp(a.pos(), b.pos(), t);
            mid.x = p.x();
            mid.y = p.y();
        } else {
            // de Casteljau split: the two halves trace the original cubic exactly.
            const QPointF p0 = a.pos();
            const QPointF c0(a.x + a.outX, a.y + a.outY);
            const QPointF c1(b.x + b.inX, b.y + b.inY);
            const QPointF p1 = b.pos();
            const QPointF q0 = lerp(p0, c0, t);
            const QPointF q1 = lerp(c0, c1, t);
            const QPointF q2 = lerp(c1, p1, t);
            const QPointF r0 = lerp(q0, q1, t);
            const QPointF r1 = lerp(q1, q2, t);
            const QPointF s = lerp(r0, r1, t);
            a.outX = q0.x() - p0.x();
            a.outY = q0.y() - p0.y();
            b.inX = q2.x() - p1.x();
            b.inY = q2.y() - p1.y();
            mid = MaskPoint {
                s.x(), s.y(), r0.x() - s.x(), r0.y() - s.y(), r1.x() - s.x(), r1.y() - s.y(), true};
        }
        path.insert(index + 1, mid);
    });
    return index + 1;
}

void removePathPoint(MaskData& mask, qsizetype index)
{
    mask.path.forEachShape([index](MaskPath& path) {
        if (index >= 0 && index < path.size())
            path.remove(index);
    });
}

} // namespace kk
