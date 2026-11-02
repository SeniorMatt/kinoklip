// SPDX-License-Identifier: GPL-3.0-or-later
// InspectorGlyphs implementation: each glyph is a QPainterPath stroked into a 16 px pixmap.
#include "ui/inspector/InspectorGlyphs.h"

#include "ui/common/Icons.h"

#include <QApplication>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>

namespace kk {

namespace {

constexpr int kGlyphSize = 16;

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
    case InspectorGlyph::MaskRectangle:
        path.addRect(QRectF(2.5, 4.0, 11, 8));
        break;
    case InspectorGlyph::MaskEllipse:
        path.addEllipse(QRectF(2.0, 3.5, 12, 9));
        break;
    case InspectorGlyph::MaskPolygon:
        path.moveTo(8, 2.5);
        path.lineTo(13.5, 6.5);
        path.lineTo(11.5, 13);
        path.lineTo(4.5, 13);
        path.lineTo(2.5, 6.5);
        path.closeSubpath();
        break;
    case InspectorGlyph::MaskBezier:
        path.moveTo(2.5, 12.5);
        path.cubicTo(4, 3, 10, 3, 13.5, 11);
        path.moveTo(5.5, 3.5); // tangent handle through the curve's top
        path.lineTo(11.5, 3.5);
        path.addEllipse(QRectF(7.0, 2.5, 2, 2));
        break;
    case InspectorGlyph::MaskFreehand:
        path.moveTo(4, 11);
        path.cubicTo(1, 6, 6, 1, 10, 3.5);
        path.cubicTo(15, 6, 12, 13, 8, 12);
        path.cubicTo(6, 11.5, 7, 9, 9, 9.5);
        break;
    case InspectorGlyph::EditOnScreen:
        path.moveTo(3, 13);
        path.lineTo(4, 9.5);
        path.lineTo(10.5, 3);
        path.lineTo(13, 5.5);
        path.lineTo(6.5, 12);
        path.closeSubpath();
        path.moveTo(9, 4.5);
        path.lineTo(11.5, 7);
        break;
    }
    return path;
}

} // namespace

QIcon inspectorGlyph(InspectorGlyph glyph)
{
    // Painted at paint time from the current palette (paletteIcon), so the glyphs follow a
    // colour scheme switch; normal state slightly dimmed, full text colour when hovered.
    constexpr qreal kNormalOpacity = 0.75;
    static QHash<int, QIcon> cache; // one engine per glyph; the engine caches the pixmaps
    if (const auto it = cache.constFind(int(glyph)); it != cache.cend())
        return *it;
    const QPainterPath path = glyphPath(glyph);
    QIcon icon = paletteIcon(
        QStringLiteral("inspector-glyph-%1").arg(int(glyph)),
        [path](QPainter& p, const QSizeF& size, const QColor& color) {
            p.scale(size.width() / kGlyphSize, size.height() / kGlyphSize);
            p.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        },
        kNormalOpacity);
    cache.insert(int(glyph), icon);
    return icon;
}

} // namespace kk
