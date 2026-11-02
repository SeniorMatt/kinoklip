// SPDX-License-Identifier: GPL-3.0-or-later
// Mask — shape masks of a clip (Edit page, roadmap v2): rectangle, ellipse, polygon, bezier and
// freehand shapes with mode, invert, opacity, feather, expand and a keyframeable transform.
// Plain value types plus pure helpers (path evaluation, freehand simplification, point edits).
#pragma once

#include "core/Animation.h"

#include <QPainterPath>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace kk {

/// What a mask draws. Rectangle and ellipse are parametric (width / height); the others are a
/// point path (MaskData::path).
enum class MaskShape : quint8 {
    Rectangle,
    Ellipse,
    Polygon, ///< straight segments (points without tangents)
    Bezier, ///< cubic segments, every point has an in and an out tangent (pen tool)
    Freehand ///< a drawn stroke, simplified to a bezier path when the stroke ends
};

/// How a mask combines with the masks above it in the list (the matte so far, M) — m is this
/// mask's coverage after invert and opacity, all in 0..1:
///
/// | Mode       | result            |
/// |------------|-------------------|
/// | Add        | M + m − M·m       |  union (soft edges add up like light, never above 1)
/// | Subtract   | M · (1 − m)       |  cut a hole
/// | Intersect  | M · m             |  keep only the overlap
/// | Difference | |M − m|           |  overlap removed, the rest kept
///
/// The matte starts empty (0) when the first enabled mask adds or differs, and full (1) when it
/// subtracts or intersects, so a single "subtract" mask cuts a hole into the clip.
enum class MaskMode : quint8 {
    Add,
    Subtract,
    Intersect,
    Difference
};

/// One point of a mask path. Coordinates are mask-local pixels (timeline pixels at mask scale 1,
/// origin at the mask position, y down); the mask transform maps them onto the clip.
struct MaskPoint
{
    double x = 0.0;
    double y = 0.0;
    /// Incoming tangent: control point = (x + inX, y + inY). Zero = corner without a handle.
    double inX = 0.0;
    double inY = 0.0;
    /// Outgoing tangent: control point = (x + outX, y + outY).
    double outX = 0.0;
    double outY = 0.0;
    /// Smooth point: dragging one tangent turns the other to stay collinear (Alt breaks it).
    bool smooth = false;

    [[nodiscard]] QPointF pos() const { return {x, y}; }
    [[nodiscard]] bool hasTangents() const
    {
        return inX != 0.0 || inY != 0.0 || outX != 0.0 || outY != 0.0;
    }
    friend bool operator==(const MaskPoint&, const MaskPoint&) = default;
};

/// The points of a path, in drawing order.
using MaskPath = QVector<MaskPoint>;

/// A keyframeable point path ("shape" property of a mask): the whole path is one animated
/// property, like Resolve's shape keyframes.
///
/// Storage: `timing` is an ordinary AnimatedValue whose keys carry the frames, handles and hold
/// flags — so the keyframe editors, KeyframeOps and the inline timeline lanes move, delete and
/// re-ease shape keys exactly like scalar keys. The value of key i is its index into `shapes`
/// (0, 1, 2, ...); evaluating `timing` therefore yields a fractional index i + t, and the path
/// is lerp(shapes[i], shapes[i + 1], t). Easing applies to the whole path.
///
/// Invariant (restored by normalize()): without keys `shapes` holds exactly one path (the
/// static shape) and timing.base == 0; with n keys `shapes` holds n paths, key i has value i.
/// Generic keyframe operations only touch `timing` (a moved key keeps its value = its old
/// index), so they must call normalize() afterwards; KeyframeOps does that.
///
/// All paths have the same number of points (inserting / removing a point applies to every
/// key, see MaskData helpers). Plain value type.
struct AnimatedPath
{
    QVector<MaskPath> shapes {MaskPath {}};
    AnimatedValue timing;

    /// True if the path has shape keyframes.
    [[nodiscard]] bool isAnimated() const { return timing.isAnimated(); }
    /// Number of points (of the first shape).
    [[nodiscard]] qsizetype pointCount() const { return shapes.isEmpty() ? 0 : shapes[0].size(); }
    /// Path at fractional clip-local frame @p localFrame into @p out (resized; no allocation
    /// when @p out already has the capacity). Exact for any frame (render, export, overlay).
    void evaluate(double localFrame, MaskPath& out) const;
    /// Convenience: evaluate() into a new path.
    [[nodiscard]] MaskPath at(double localFrame) const;
    /// Overlay / inspector edit: sets the shape shown at @p localFrame. Animated: adds or
    /// replaces the key at that frame; otherwise replaces the static shape.
    void setShape(int64_t localFrame, const MaskPath& path);
    /// Adds a shape key at @p localFrame holding the current (interpolated) shape, or replaces
    /// the shape of the key there with @p path when given.
    void setKey(int64_t localFrame, std::optional<MaskPath> path = std::nullopt);
    /// Removes the key at @p localFrame; removing the last key keeps its shape as the static one.
    void removeKey(int64_t localFrame);
    /// Restores the invariant after `timing` was edited directly (see above).
    void normalize();
    /// Applies @p fn to every stored shape (static or keyed): point insertion / deletion.
    template<typename F>
    void forEachShape(F&& fn)
    {
        for (MaskPath& p : shapes)
            fn(p);
    }
    /// Moves every key by @p deltaFrames (head trim / split, like ParamSet::shiftKeys).
    void shiftKeys(int64_t deltaFrames) { timing.shiftKeys(deltaFrames); }

    friend bool operator==(const AnimatedPath&, const AnimatedPath&) = default;
};

/// Well-known keys of MaskData::params (all animated, timeline pixels unless stated).
namespace maskkeys {
inline const QString PosX = QStringLiteral("posX"); ///< mask centre, px from the clip centre
inline const QString PosY = QStringLiteral("posY"); ///< px from the clip centre, +down
inline const QString Width = QStringLiteral("width"); ///< rectangle / ellipse width, px
inline const QString Height = QStringLiteral("height"); ///< rectangle / ellipse height, px
inline const QString ScaleX = QStringLiteral("scaleX"); ///< factor; default 1
inline const QString ScaleY = QStringLiteral("scaleY"); ///< factor; default 1
inline const QString Rotation = QStringLiteral("rotation"); ///< degrees clockwise; default 0
inline const QString Opacity = QStringLiteral("opacity"); ///< percent 0..100; default 100
inline const QString FeatherIn = QStringLiteral("featherIn"); ///< soft edge inside, px >= 0
inline const QString FeatherOut = QStringLiteral("featherOut"); ///< soft edge outside, px >= 0
inline const QString Expand = QStringLiteral("expand"); ///< px, > 0 grows, < 0 contracts
/// Inspector-only keys of the "builtin.mask" descriptor routed to MaskData::mode / invert (not
/// stored in params; the renderer reads the struct fields, no QVariant per frame).
inline const QString Mode = QStringLiteral("mode");
inline const QString Invert = QStringLiteral("invert");
/// Key path suffix of the animated shape (KeyframeOps "mask/<id>/shape").
inline const QString Shape = QStringLiteral("shape");

/// Documented default of a mask parameter (unknown keys: 0).
[[nodiscard]] double defaultValue(QStringView key);
} // namespace maskkeys

/// One mask of a clip. Masks are evaluated in list order on the clip's picture before its
/// transform (so they move with the clip), after its effects; see src/render/readme.md.
/// Descriptor id of the mask parameters (core/BuiltinEffects.cpp; Inspector rows, keyframe
/// labels). Its name is the "Masks" group title.
inline constexpr char kMaskDescriptorId[] = "builtin.mask";

struct MaskData
{
    quint64 id = 0; ///< unique within the timeline (TimelineData::allocateId())
    QString name; ///< user name; empty = automatic ("Ellipse 1", see maskDisplayName())
    MaskShape shape = MaskShape::Rectangle;
    bool enabled = true;
    MaskMode mode = MaskMode::Add;
    bool invert = false;
    /// Point shapes: the path is closed (only closed paths with >= 3 points (polygon) or >= 2
    /// points (bezier) render; an open path is still being drawn).
    bool closed = true;
    ParamSet params; ///< maskkeys::*
    AnimatedPath path; ///< Polygon / Bezier / Freehand only

    /// True for the point shapes (Polygon, Bezier, Freehand).
    [[nodiscard]] bool isPathShape() const
    {
        return shape == MaskShape::Polygon || shape == MaskShape::Bezier
            || shape == MaskShape::Freehand;
    }
    /// True if the mask contributes to the matte (enabled, closed, enough points, opacity > 0
    /// is checked per frame by the renderer).
    [[nodiscard]] bool isRenderable() const;
    /// Value of parameter @p key at @p localFrame with its documented default.
    [[nodiscard]] double value(const QString& key, int64_t localFrame) const
    {
        return params.value(key, localFrame, maskkeys::defaultValue(key));
    }
    /// Shifts every key (params and shape) by @p deltaFrames.
    void shiftKeys(int64_t deltaFrames)
    {
        params.shiftKeys(deltaFrames);
        path.shiftKeys(deltaFrames);
    }
    /// True if any parameter or the shape has keyframes.
    [[nodiscard]] bool isAnimated() const { return params.isAnimated() || path.isAnimated(); }

    friend bool operator==(const MaskData&, const MaskData&) = default;
};

/// Stable string id of a shape stored in project files ("rectangle", "ellipse", "polygon",
/// "bezier", "freehand"); unknown strings parse as Rectangle.
[[nodiscard]] QString maskShapeToString(MaskShape shape);
[[nodiscard]] MaskShape maskShapeFromString(const QString& text);
/// Stable string id of a mode ("add", "subtract", "intersect", "difference"); unknown = Add.
[[nodiscard]] QString maskModeToString(MaskMode mode);
[[nodiscard]] MaskMode maskModeFromString(const QString& text);
/// Translated name of a shape type ("Rectangle", ...).
[[nodiscard]] QString maskShapeName(MaskShape shape);
/// Display name: the user name, or "<Shape> <n>" with the 1-based position @p index + 1.
[[nodiscard]] QString maskDisplayName(const MaskData& mask, int index);

/// A new mask of @p shape with id @p id. Rectangle / ellipse get @p defaultSize (px, usually
/// half of the clip's content); point shapes start empty and open (drawn in the viewer).
[[nodiscard]] MaskData makeMask(MaskShape shape, quint64 id, QSizeF defaultSize);

/// Maps a mask-local point to clip content coordinates (timeline px, origin at the content
/// centre): content = pos + R(rotation) · S(scale) · local.
[[nodiscard]] QPointF maskToContent(const MaskData& mask, int64_t localFrame, QPointF local);
/// Inverse of maskToContent() (std::nullopt when a scale is 0).
[[nodiscard]] std::optional<QPointF> contentToMask(
    const MaskData& mask, int64_t localFrame, QPointF content);

/// Outline of @p mask at @p localFrame in clip content coordinates (no feather or expansion):
/// a rotated rectangle / ellipse, or the bezier path (closed when the mask is). Used by the
/// viewer overlay and the CPU renderer.
[[nodiscard]] QPainterPath maskOutline(const MaskData& mask, int64_t localFrame);

/// Flattens the closed bezier path @p path (mask-local) into a polygon @p out, mapping every
/// vertex through @p map (e.g. maskToContent then to layer pixels). Curved segments are split
/// adaptively so that the chord error stays below @p tolerance (same units as the mapped
/// output). @p out is cleared first and reused (no allocation once it has the capacity).
template<typename Map>
void flattenPath(
    const MaskPath& path, bool closed, double tolerance, Map&& map, QVector<QPointF>& out);

/// Offsets the closed polygon @p poly by @p distance along its outward normals (miter joins,
/// limited to 4 × distance), in place. Positive grows, negative shrinks; orientation-safe.
void offsetPolygon(QVector<QPointF>& poly, double distance);

/// Simplifies a freehand stroke (@p stroke in mask-local px) into a smooth closed bezier path:
/// Ramer-Douglas-Peucker with @p tolerance px, then Catmull-Rom tangents. Returns at least 3
/// points when the stroke has them.
[[nodiscard]] MaskPath simplifyStroke(const QVector<QPointF>& stroke, double tolerance);

/// Inserts a point into every shape of @p mask's path after point @p index (segment
/// index → index + 1) at bezier parameter @p t, splitting the segment with de Casteljau so the
/// outline does not change. Returns the index of the new point.
qsizetype insertPathPoint(MaskData& mask, qsizetype index, double t);
/// Removes point @p index from every shape of @p mask's path.
void removePathPoint(MaskData& mask, qsizetype index);

// ---- Template implementation ----

namespace maskdetail {
/// Cubic bezier point at parameter t.
inline QPointF cubic(QPointF p0, QPointF c0, QPointF c1, QPointF p1, double t)
{
    const double u = 1.0 - t;
    return u * u * u * p0 + 3.0 * u * u * t * c0 + 3.0 * u * t * t * c1 + t * t * t * p1;
}
/// Number of line pieces for a cubic whose control polygon has length @p hullLength, so the
/// chord error stays below @p tolerance (error of n pieces ~ L² / (8 n²) for a smooth curve;
/// conservative bound using the control polygon). Clamped to [1, 64].
inline int cubicPieces(double hullLength, double chordLength, double tolerance)
{
    const double bend = hullLength - chordLength; // 0 for a straight segment
    if (bend <= tolerance * 0.25)
        return 1;
    const double n = std::sqrt(bend * hullLength / (2.0 * std::max(tolerance, 1e-3)));
    return std::clamp(int(std::ceil(n)), 2, 64);
}
} // namespace maskdetail

template<typename Map>
void flattenPath(
    const MaskPath& path, bool closed, double tolerance, Map&& map, QVector<QPointF>& out)
{
    out.clear();
    const qsizetype n = path.size();
    if (n == 0)
        return;
    const qsizetype segments = closed ? n : n - 1;
    for (qsizetype i = 0; i < segments; ++i) {
        const MaskPoint& a = path[i];
        const MaskPoint& b = path[(i + 1) % n];
        const QPointF p0 = map(a.pos());
        out.append(p0);
        if (!a.hasTangents() && !b.hasTangents())
            continue; // straight edge: only the start vertex
        const QPointF c0 = map(QPointF(a.x + a.outX, a.y + a.outY));
        const QPointF c1 = map(QPointF(b.x + b.inX, b.y + b.inY));
        const QPointF p1 = map(b.pos());
        auto len = [](QPointF d) { return std::sqrt(d.x() * d.x() + d.y() * d.y()); };
        const double hull = len(c0 - p0) + len(c1 - c0) + len(p1 - c1);
        const int pieces = maskdetail::cubicPieces(hull, len(p1 - p0), tolerance);
        for (int k = 1; k < pieces; ++k)
            out.append(maskdetail::cubic(p0, c0, c1, p1, double(k) / pieces));
    }
    if (!closed)
        out.append(map(path[n - 1].pos()));
}

} // namespace kk
