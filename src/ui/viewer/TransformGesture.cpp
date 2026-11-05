// SPDX-License-Identifier: GPL-3.0-or-later
// TransformGesture — picture geometry, hit-testing and parameter maths of the on-screen
// Transform / Crop controls.
#include "ui/viewer/TransformGesture.h"

#include "core/MediaPool.h"
#include "core/ParamKeys.h"
#include "core/TimelineData.h"
#include "render/TimelineEval.h"

#include <QLineF>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::transformgesture {

namespace {

// Parameter limits of the Inspector's Transform descriptor (core/BuiltinEffects.cpp).
constexpr double kMinZoom = 0.01;
constexpr double kMaxZoom = 20.0;
constexpr double kMaxRotation = 3600.0;
constexpr double kMaxOffset = 10000.0; ///< Position / Anchor px
constexpr double kRotateSnapDegrees = 15.0; ///< Shift while rotating
constexpr double kZoomStep = 0.001; ///< zoom values are rounded to this
constexpr double kRotationStep = 0.1; ///< like the Inspector's Rotation field
/// Below this content distance (px) from the anchor an axis cannot be scaled (division by ~0).
constexpr double kMinLever = 1.0;

double roundTo(double v, double step)
{
    return std::round(v / step) * step;
}

double degrees(QPointF v)
{
    return std::atan2(v.y(), v.x()) * 180.0 / std::numbers::pi;
}

/// The linear (zoom, flip, rotation) part of @p m applied to vector @p v.
QPointF linear(const QTransform& m, QPointF v)
{
    return m.map(v) - m.map(QPointF(0, 0));
}

/// Content point of handle (@p sx, @p sy) on rectangle @p r.
QPointF handlePoint(const QRectF& r, int sx, int sy)
{
    return QPointF(r.center().x() + sx * r.width() / 2, r.center().y() + sy * r.height() / 2);
}

} // namespace

TransformPicture picture(
    const ClipData& clip, int64_t localFrame, QSizeF frame, const MediaPool* pool)
{
    TransformPicture pic;
    pic.sourcePx = frame;
    if (clip.kind == ClipKind::Media && pool) {
        if (const MediaItem* item = pool->item(clip.mediaId)) {
            if (item->info.displayWidth > 0 && item->info.displayHeight > 0)
                pic.sourcePx = QSizeF(item->info.displayWidth, item->info.displayHeight);
        }
    }
    pic.content = clip.kind == ClipKind::Media ? render::fitSize(pic.sourcePx, frame) : frame;
    const ParamSet& p = clip.transform;
    const double sx = pic.content.width() / pic.sourcePx.width();
    const double sy = pic.content.height() / pic.sourcePx.height();
    const double w2 = pic.content.width() / 2, h2 = pic.content.height() / 2;
    pic.crop = QRectF(QPointF(-w2 + std::max(0.0, p.value(keys::CropL, localFrame)) * sx,
                          -h2 + std::max(0.0, p.value(keys::CropT, localFrame)) * sy),
        QPointF(w2 - std::max(0.0, p.value(keys::CropR, localFrame)) * sx,
            h2 - std::max(0.0, p.value(keys::CropB, localFrame)) * sy));
    pic.anchor = QPointF(p.value(keys::AnchorX, localFrame), p.value(keys::AnchorY, localFrame));
    // Without Dynamic Zoom: the handles edit the Transform parameters, whose maths (pivot,
    // lever, anchor) is exact only for the transform itself (spec 24.20).
    pic.contentToFrame = render::clipTransform(clip, localFrame, frame, false);
    return pic;
}

std::vector<std::pair<QPointF, TransformHandle>> handles(
    const TransformPicture& pic, const QTransform& frameToWidget, bool crop)
{
    const QRectF r = crop ? pic.crop
                          : QRectF(-pic.content.width() / 2, -pic.content.height() / 2,
                                pic.content.width(), pic.content.height());
    const QTransform toWidget = pic.contentToFrame * frameToWidget;
    const TransformHandle::Kind kind
        = crop ? TransformHandle::Kind::Crop : TransformHandle::Kind::Scale;
    static constexpr int kOrder[8][2]
        = {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}};
    std::vector<std::pair<QPointF, TransformHandle>> out;
    out.reserve(8);
    for (const auto& [sx, sy] : kOrder)
        out.push_back({toWidget.map(handlePoint(r, sx, sy)), {kind, sx, sy}});
    return out;
}

QPointF rotateKnob(const TransformPicture& pic, const QTransform& frameToWidget)
{
    const QTransform toWidget = pic.contentToFrame * frameToWidget;
    const QPointF top = toWidget.map(QPointF(0, -pic.content.height() / 2));
    const QPointF centre = toWidget.map(QPointF(0, 0));
    QLineF out(centre, top);
    if (out.length() < 1e-6)
        return top + QPointF(0, -kRotateKnobDistance);
    out.setLength(out.length() + kRotateKnobDistance);
    return out.p2();
}

TransformHandle hitTest(
    const TransformPicture& pic, const QTransform& frameToWidget, bool crop, QPointF pos)
{
    auto near = [pos](QPointF p) { return QLineF(p, pos).length() <= kGrabRadius; };
    const QTransform toWidget = pic.contentToFrame * frameToWidget;
    if (!crop) {
        if (near(toWidget.map(pic.anchor)))
            return {TransformHandle::Kind::Anchor, 0, 0};
        if (near(rotateKnob(pic, frameToWidget)))
            return {TransformHandle::Kind::Rotate, 0, 0};
    }
    // Corners before edges: they come first in a small picture where both are in reach.
    const auto all = handles(pic, frameToWidget, crop);
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& [p, h] : all) {
            const bool corner = h.sx != 0 && h.sy != 0;
            if (corner == (pass == 0) && near(p))
                return h;
        }
    }
    return crop ? TransformHandle {} : TransformHandle {TransformHandle::Kind::Move, 0, 0};
}

void TransformDrag::begin(const ClipData& clip, int64_t localFrame, const TransformPicture& pic,
    TransformHandle handle, QPointF pointer)
{
    const ParamSet& p = clip.transform;
    m_handle = handle;
    m_pic = pic;
    m_press = pointer;
    m_posX = p.value(keys::PosX, localFrame);
    m_posY = p.value(keys::PosY, localFrame);
    m_zoomX = p.value(keys::ZoomX, localFrame);
    m_zoomY = p.value(keys::ZoomY, localFrame);
    m_rotation = p.value(keys::Rotation, localFrame);
    m_anchorX = p.value(keys::AnchorX, localFrame);
    m_anchorY = p.value(keys::AnchorY, localFrame);
    m_cropL = p.value(keys::CropL, localFrame);
    m_cropR = p.value(keys::CropR, localFrame);
    m_cropT = p.value(keys::CropT, localFrame);
    m_cropB = p.value(keys::CropB, localFrame);
    m_flipX = p.staticValue(keys::FlipH, false).toBool() ? -1.0 : 1.0;
    m_flipY = p.staticValue(keys::FlipV, false).toBool() ? -1.0 : 1.0;
    m_linked = p.staticValue(keys::ZoomLinked, true).toBool();
    m_pivot = pic.contentToFrame.map(pic.anchor);
    m_lastAngle = degrees(pointer - m_pivot);
    m_turned = 0;
}

void TransformDrag::update(ParamSet& t, int64_t local, QPointF pointer, bool shift)
{
    auto set = [&t, local](const QString& key, double fallback, double value) {
        t.animated(key, fallback).setValue(local, value);
    };
    switch (m_handle.kind) {
    case TransformHandle::Kind::None:
        return;
    case TransformHandle::Kind::Move: {
        QPointF d = pointer - m_press;
        if (shift) {
            if (std::abs(d.x()) >= std::abs(d.y()))
                d.setY(0);
            else
                d.setX(0);
        }
        // Whole pixels, like the Inspector's Position fields.
        set(keys::PosX, 0, std::clamp(std::round(m_posX + d.x()), -kMaxOffset, kMaxOffset));
        set(keys::PosY, 0, std::clamp(std::round(m_posY + d.y()), -kMaxOffset, kMaxOffset));
        return;
    }
    case TransformHandle::Kind::Rotate: {
        // Unwrapped: each step adds the change of the pointer angle around the pivot, so
        // several turns accumulate instead of jumping at +-180 degrees.
        const double angle = degrees(pointer - m_pivot);
        double step = angle - m_lastAngle;
        if (step > 180)
            step -= 360;
        else if (step < -180)
            step += 360;
        m_lastAngle = angle;
        m_turned += step;
        double rotation = m_rotation + m_turned;
        rotation = shift ? roundTo(rotation, kRotateSnapDegrees) : roundTo(rotation, kRotationStep);
        set(keys::Rotation, 0, std::clamp(rotation, -kMaxRotation, kMaxRotation));
        return;
    }
    case TransformHandle::Kind::Anchor: {
        // The content point under the pointer becomes the anchor; the picture must not move:
        // p = c + pos + a + L(q - a) for every q  =>  pos' = pos + (I - L)(a - a').
        bool invertible = false;
        const QTransform toContent = m_pic.contentToFrame.inverted(&invertible);
        if (!invertible)
            return;
        const QPointF q = toContent.map(pointer);
        const QPointF a0(m_anchorX, m_anchorY);
        const QPointF a1(std::clamp(std::round(q.x()), -kMaxOffset, kMaxOffset),
            std::clamp(std::round(q.y()), -kMaxOffset, kMaxOffset));
        const QPointF diff = a0 - a1;
        const QPointF pos = QPointF(m_posX, m_posY) + diff - linear(m_pic.contentToFrame, diff);
        set(keys::AnchorX, 0, a1.x());
        set(keys::AnchorY, 0, a1.y());
        set(keys::PosX, 0, std::clamp(std::round(pos.x()), -kMaxOffset, kMaxOffset));
        set(keys::PosY, 0, std::clamp(std::round(pos.y()), -kMaxOffset, kMaxOffset));
        return;
    }
    case TransformHandle::Kind::Scale: {
        // Pointer relative to the pivot, turned back into the picture's unrotated axes.
        const QPointF v = QTransform().rotate(-m_rotation).map(pointer - m_pivot);
        const QPointF handle
            = handlePoint(QRectF(-m_pic.content.width() / 2, -m_pic.content.height() / 2,
                              m_pic.content.width(), m_pic.content.height()),
                m_handle.sx, m_handle.sy);
        const QPointF lever = handle - m_pic.anchor; // content px from the anchor
        const bool uniform = m_linked != shift;
        const bool canX = m_handle.sx != 0 && std::abs(lever.x()) >= kMinLever;
        const bool canY = m_handle.sy != 0 && std::abs(lever.y()) >= kMinLever;
        double zx = m_zoomX, zy = m_zoomY;
        if (m_handle.sx != 0 && m_handle.sy != 0 && uniform) {
            // Corner, aspect kept: the factor that best fits the pointer along the diagonal.
            const QPointF d0(m_zoomX * m_flipX * lever.x(), m_zoomY * m_flipY * lever.y());
            const double len2 = d0.x() * d0.x() + d0.y() * d0.y();
            if (len2 < kMinLever)
                return;
            const double f = (v.x() * d0.x() + v.y() * d0.y()) / len2;
            zx = m_zoomX * f;
            zy = m_zoomY * f;
        } else {
            if (canX)
                zx = v.x() / (lever.x() * m_flipX);
            if (canY)
                zy = v.y() / (lever.y() * m_flipY);
            if (uniform && m_handle.sy == 0 && canX && m_zoomX != 0)
                zy = m_zoomY * zx / m_zoomX;
            else if (uniform && m_handle.sx == 0 && canY && m_zoomY != 0)
                zx = m_zoomX * zy / m_zoomY;
        }
        zx = roundTo(std::clamp(zx, kMinZoom, kMaxZoom), kZoomStep);
        zy = roundTo(std::clamp(zy, kMinZoom, kMaxZoom), kZoomStep);
        set(keys::ZoomX, 1, zx);
        set(keys::ZoomY, 1, zy);
        // Shift only frees (or links) this one drag: the stored Link Zoom stays as the user
        // set it with its button (spec 49.2). A linked Inspector edit afterwards keeps the
        // X:Y ratio the drag left (paramaccess::setScalar).
        return;
    }
    case TransformHandle::Kind::Crop: {
        bool invertible = false;
        const QTransform toContent = m_pic.contentToFrame.inverted(&invertible);
        if (!invertible)
            return;
        const QPointF q = toContent.map(pointer);
        const double sx = m_pic.content.width() / m_pic.sourcePx.width();
        const double sy = m_pic.content.height() / m_pic.sourcePx.height();
        const double w2 = m_pic.content.width() / 2, h2 = m_pic.content.height() / 2;
        const double srcW = m_pic.sourcePx.width(), srcH = m_pic.sourcePx.height();
        // Source pixels, whole, and never past the opposite edge (one pixel stays visible).
        auto clampPx = [](double v, double max) { return std::clamp(std::round(v), 0.0, max); };
        if (m_handle.sx < 0)
            set(keys::CropL, 0, clampPx((q.x() + w2) / sx, srcW - m_cropR - 1));
        else if (m_handle.sx > 0)
            set(keys::CropR, 0, clampPx((w2 - q.x()) / sx, srcW - m_cropL - 1));
        if (m_handle.sy < 0)
            set(keys::CropT, 0, clampPx((q.y() + h2) / sy, srcH - m_cropB - 1));
        else if (m_handle.sy > 0)
            set(keys::CropB, 0, clampPx((h2 - q.y()) / sy, srcH - m_cropT - 1));
        return;
    }
    }
}

} // namespace kk::transformgesture
