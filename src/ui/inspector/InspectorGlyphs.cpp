// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorGlyphs implementation: each glyph is a QPainterPath stroked into a 16 px pixmap.
#include "ui/inspector/InspectorGlyphs.h"

#include <QApplication>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>

namespace kk {

namespace {

constexpr int kGlyphSize = 16;
constexpr int kGlyphScale = 2; ///< drawn at 2x so the icon stays crisp on HiDPI screens

QPainterPath glyphPath(InspectorGlyph glyph)
{
    QPainterPath path;
    switch (glyph) {
    case InspectorGlyph::Reset:
        // 300 degree arc with an arrow head at its start.
        path.arcMoveTo(QRectF(3.5, 3.5, 9, 9), 120);
        path.arcTo(QRectF(3.5, 3.5, 9, 9), 120, 290);
        path.moveTo(3.2, 2.8);
        path.lineTo(5.6, 4.6);
        path.lineTo(3.0, 6.4);
        break;
    case InspectorGlyph::Remove:
        path.moveTo(4.5, 4.5);
        path.lineTo(11.5, 11.5);
        path.moveTo(11.5, 4.5);
        path.lineTo(4.5, 11.5);
        break;
    case InspectorGlyph::MoveUp:
        path.moveTo(4, 10);
        path.lineTo(8, 6);
        path.lineTo(12, 10);
        break;
    case InspectorGlyph::MoveDown:
    case InspectorGlyph::Expanded:
        path.moveTo(4, 6);
        path.lineTo(8, 10);
        path.lineTo(12, 6);
        break;
    case InspectorGlyph::Collapsed:
        path.moveTo(6, 4);
        path.lineTo(10, 8);
        path.lineTo(6, 12);
        break;
    case InspectorGlyph::Add:
        path.moveTo(8, 3.5);
        path.lineTo(8, 12.5);
        path.moveTo(3.5, 8);
        path.lineTo(12.5, 8);
        break;
    }
    return path;
}

QPixmap renderGlyph(InspectorGlyph glyph, const QColor& color)
{
    QPixmap pixmap(kGlyphSize * kGlyphScale, kGlyphSize * kGlyphScale);
    pixmap.setDevicePixelRatio(kGlyphScale);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(glyphPath(glyph));
    return pixmap;
}

} // namespace

QIcon inspectorGlyph(InspectorGlyph glyph)
{
    // Keyed by glyph and palette colour, so switching themes produces fresh icons.
    static QHash<quint64, QIcon> cache;
    const QPalette pal = QApplication::palette();
    const QColor normal = pal.color(QPalette::Active, QPalette::WindowText);
    const quint64 key = (quint64(glyph) << 32) | normal.rgba();
    if (const auto it = cache.constFind(key); it != cache.cend())
        return *it;
    QIcon icon;
    QColor dimmed = normal;
    dimmed.setAlphaF(0.75f);
    icon.addPixmap(renderGlyph(glyph, dimmed), QIcon::Normal);
    icon.addPixmap(renderGlyph(glyph, normal), QIcon::Active);
    icon.addPixmap(
        renderGlyph(glyph, pal.color(QPalette::Disabled, QPalette::WindowText)), QIcon::Disabled);
    cache.insert(key, icon);
    return icon;
}

} // namespace kk
