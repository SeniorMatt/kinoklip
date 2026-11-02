// SPDX-License-Identifier: GPL-3.0-or-later
// CaptionRenderer — caption style resolution and placement (shared by GPU and CPU paths).
#include "render/CaptionRenderer.h"

#include "core/ParamKeys.h"
#include "core/Subtitles.h"

#include <algorithm>

namespace kk::render {

TitleStyle resolveCaptionStyle(const ClipData& caption, const TrackData* track, int64_t localFrame,
    QSize canvas, double scale, CaptionPlacement* placement)
{
    namespace s = subtitles;
    static const QString kLeft = QStringLiteral("left");
    static const QString kRight = QStringLiteral("right");
    static const QString kTop = QStringLiteral("top");
    static const QString kMiddle = QStringLiteral("middle");
    auto color = [&](const QString& key, const QColor& fallback) {
        const QColor c = s::styleStatic(caption, track, key).value<QColor>();
        return c.isValid() ? c : fallback;
    };
    TitleStyle t;
    t.text = s::captionText(caption);
    t.font = s::styleStatic(caption, track, keys::Font).toString();
    t.bold = s::styleStatic(caption, track, keys::Bold).toBool();
    t.italic = s::styleStatic(caption, track, keys::Italic).toBool();
    const QString align = s::styleStatic(caption, track, keys::Align).toString();
    t.align = align == kLeft ? 0 : align == kRight ? 2 : 1;
    t.color = color(keys::Color, Qt::white);
    t.outlineColor = color(keys::OutlineColor, Qt::black);
    t.bgColor = color(keys::BgColor, Qt::black);
    t.bgEnabled = s::styleStatic(caption, track, keys::BgEnabled).toBool();
    t.shadow = s::styleStatic(caption, track, keys::Shadow).toBool();
    t.size = std::max(1.0, s::styleValue(caption, track, keys::Size, localFrame));
    t.tracking = s::styleValue(caption, track, keys::Tracking, localFrame);
    t.lineSpacing = std::max(10.0, s::styleValue(caption, track, keys::LineSpacing, localFrame));
    t.outlineWidth = std::max(0.0, s::styleValue(caption, track, keys::OutlineWidth, localFrame));
    t.bgOpacity
        = std::clamp(s::styleValue(caption, track, keys::BgOpacity, localFrame), 0.0, 100.0);
    t.canvas = canvas;
    t.scale = scale;
    if (placement) {
        const QString pos = s::styleStatic(caption, track, s::Position).toString();
        placement->position = pos == kTop ? 1 : pos == kMiddle ? 2 : 0;
        placement->marginV
            = std::clamp(s::styleValue(caption, track, s::MarginV, localFrame), 0.0, 50.0) / 100;
        placement->marginH
            = std::clamp(s::styleValue(caption, track, s::MarginH, localFrame), 0.0, 50.0) / 100;
    }
    return t;
}

QPointF captionOffset(
    const QRectF& rect, QSize canvas, int align, const CaptionPlacement& placement)
{
    const double w = canvas.width();
    const double h = canvas.height();
    double dx = 0;
    if (align == 0)
        dx = w * placement.marginH - rect.left();
    else if (align == 2)
        dx = w * (1.0 - placement.marginH) - rect.right();
    double dy = 0;
    if (placement.position == 0)
        dy = h * (1.0 - placement.marginV) - rect.bottom();
    else if (placement.position == 1)
        dy = h * placement.marginV - rect.top();
    return {dx, dy};
}

const ClipData* captionAt(const TrackData& track, int64_t frame)
{
    const ClipData* c = track.clipAt(frame);
    if (!c || !c->enabled || c->kind != ClipKind::Subtitle)
        return nullptr;
    return subtitles::captionText(*c).isEmpty() ? nullptr : c;
}

} // namespace kk::render
