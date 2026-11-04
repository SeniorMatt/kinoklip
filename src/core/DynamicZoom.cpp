// SPDX-License-Identifier: GPL-3.0-or-later
// Dynamic Zoom maths (see DynamicZoom.h).
#include "core/DynamicZoom.h"

#include "core/Animation.h"
#include "core/ParamKeys.h"
#include "core/TimelineData.h"

#include <QCoreApplication>
#include <QRectF>

#include <algorithm>

namespace kk::dynamiczoom {

namespace {

const QString kLinear = QStringLiteral("linear");
const QString kEaseIn = QStringLiteral("easeIn");
const QString kEaseOut = QStringLiteral("easeOut");
const QString kEaseInOut = QStringLiteral("easeInOut");

/// The end rectangle of the default push-in (the picture grows by a fifth over the clip).
constexpr double kDefaultEndSize = 1.0 / 1.2;

double number(const ParamSet& p, const QString& key, double fallback)
{
    bool ok = false;
    const double v = p.staticValue(key).toDouble(&ok);
    return ok ? v : fallback;
}

Rect read(const ParamSet& p, const QString& x, const QString& y, const QString& size,
    const Rect& fallback)
{
    return {number(p, x, fallback.cx), number(p, y, fallback.cy),
        std::max(kMinSize, number(p, size, fallback.size))};
}

} // namespace

QStringList easeIds()
{
    return {kLinear, kEaseIn, kEaseOut, kEaseInOut};
}

QStringList easeLabels()
{
    return {QCoreApplication::translate("kk::dynamiczoom", "Linear"),
        QCoreApplication::translate("kk::dynamiczoom", "Ease In"),
        QCoreApplication::translate("kk::dynamiczoom", "Ease Out"),
        QCoreApplication::translate("kk::dynamiczoom", "Ease In and Out")};
}

Rect defaultStart()
{
    return {};
}

Rect defaultEnd()
{
    return {0.5, 0.5, kDefaultEndSize};
}

bool isEnabled(const ParamSet& transform)
{
    return transform.staticValue(keys::DynamicZoom).toBool();
}

Rect start(const ParamSet& transform)
{
    return read(transform, keys::DynamicZoomStartX, keys::DynamicZoomStartY,
        keys::DynamicZoomStartSize, defaultStart());
}

Rect end(const ParamSet& transform)
{
    return read(transform, keys::DynamicZoomEndX, keys::DynamicZoomEndY, keys::DynamicZoomEndSize,
        defaultEnd());
}

QString ease(const ParamSet& transform)
{
    const QString id = transform.staticValue(keys::DynamicZoomEase).toString();
    return easeIds().contains(id) ? id : kLinear;
}

void setStart(ParamSet& transform, const Rect& r)
{
    transform.setStatic(keys::DynamicZoomStartX, r.cx);
    transform.setStatic(keys::DynamicZoomStartY, r.cy);
    transform.setStatic(keys::DynamicZoomStartSize, std::max(kMinSize, r.size));
}

void setEnd(ParamSet& transform, const Rect& r)
{
    transform.setStatic(keys::DynamicZoomEndX, r.cx);
    transform.setStatic(keys::DynamicZoomEndY, r.cy);
    transform.setStatic(keys::DynamicZoomEndSize, std::max(kMinSize, r.size));
}

void swap(ParamSet& transform)
{
    const Rect a = start(transform);
    const Rect b = end(transform);
    setStart(transform, b);
    setEnd(transform, a);
}

void reset(ParamSet& transform)
{
    for (const QString& key : {keys::DynamicZoom, keys::DynamicZoomEase, keys::DynamicZoomStartX,
             keys::DynamicZoomStartY, keys::DynamicZoomStartSize, keys::DynamicZoomEndX,
             keys::DynamicZoomEndY, keys::DynamicZoomEndSize})
        transform.statics.remove(key);
}

double eased(double t, const QString& easeId)
{
    t = std::clamp(t, 0.0, 1.0);
    if (easeId == kEaseIn)
        return t * t; // slow start
    if (easeId == kEaseOut)
        return 1.0 - (1.0 - t) * (1.0 - t); // slow end
    if (easeId == kEaseInOut)
        return t * t * (3.0 - 2.0 * t);
    return t;
}

Rect at(const ClipData& clip, int64_t localFrame)
{
    const Rect a = start(clip.transform);
    const Rect b = end(clip.transform);
    const double t = clip.duration > 1
        ? eased(double(localFrame) / double(clip.duration - 1), ease(clip.transform))
        : 0.0;
    return {a.cx + (b.cx - a.cx) * t, a.cy + (b.cy - a.cy) * t, a.size + (b.size - a.size) * t};
}

QTransform transform(const ClipData& clip, int64_t localFrame, QSizeF frameSize)
{
    if (!isEnabled(clip.transform))
        return {};
    const Rect r = at(clip, localFrame);
    const double s = 1.0 / std::max(kMinSize, r.size);
    // Picture point q -> (q - rect centre) * s: the rectangle fills the frame.
    QTransform m;
    m.scale(s, s);
    m.translate(-(r.cx - 0.5) * frameSize.width(), -(r.cy - 0.5) * frameSize.height());
    return m;
}

QRectF toPicture(const Rect& r, QSizeF frameSize)
{
    const double w = r.size * frameSize.width();
    const double h = r.size * frameSize.height();
    return {
        (r.cx - 0.5) * frameSize.width() - w / 2, (r.cy - 0.5) * frameSize.height() - h / 2, w, h};
}

Rect fromPicture(const QRectF& rect, QSizeF frameSize)
{
    if (frameSize.width() <= 0 || frameSize.height() <= 0)
        return {};
    return {rect.center().x() / frameSize.width() + 0.5,
        rect.center().y() / frameSize.height() + 0.5,
        std::max(kMinSize, rect.width() / frameSize.width())};
}

} // namespace kk::dynamiczoom
