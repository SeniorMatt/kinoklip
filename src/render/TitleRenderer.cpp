// SPDX-License-Identifier: GPL-3.0-or-later
// TitleRenderer — QPainter text rasterisation for Title clips.
#include "render/TitleRenderer.h"

#include "core/ParamKeys.h"

#include <QFont>
#include <QFontMetricsF>
#include <QHashFunctions>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

/// Background box padding around the text block, as a fraction of the font size.
constexpr double kBgPaddingEm = 0.25;
/// Drop shadow offset (down-right) as a fraction of the font size, at least kMinShadowPx.
constexpr double kShadowOffsetEm = 0.05;
constexpr double kMinShadowPx = 2.0;
/// Shadow colour: black at ~60 % (fixed for titles; use the drop_shadow effect for control).
constexpr int kShadowAlpha = 150;
/// Extra transparent border so antialiased edges are never cut off.
constexpr int kAaMarginPx = 2;
/// Upper bound of the rasterised image relative to the canvas (huge fonts are clipped).
constexpr int kMaxCanvasMultiple = 3;

QColor colorStatic(const ParamSet& p, const QString& key, const QColor& fallback)
{
    const auto it = p.statics.constFind(key);
    if (it == p.statics.constEnd())
        return fallback;
    const QColor c = it->value<QColor>();
    return c.isValid() ? c : fallback;
}

bool boolStatic(const ParamSet& p, const QString& key, bool fallback)
{
    const auto it = p.statics.constFind(key);
    return it == p.statics.constEnd() ? fallback : it->toBool();
}

QString stringStatic(const ParamSet& p, const QString& key, const QString& fallback)
{
    const auto it = p.statics.constFind(key);
    return it == p.statics.constEnd() ? fallback : it->toString();
}

} // namespace

size_t qHash(const TitleStyle& s, size_t seed)
{
    // Qt 6.12 deprecates the seedless combiner (the seed must reach the element hashes);
    // Qt 6.4 only has the default constructor.
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    QtPrivate::QHashCombine hash(seed);
#else
    QtPrivate::QHashCombine hash;
#endif
    seed = hash(seed, s.text);
    seed = hash(seed, s.font);
    seed
        = hash(seed, int(s.bold) | int(s.italic) << 1 | int(s.bgEnabled) << 2 | int(s.shadow) << 3);
    seed = hash(seed, s.align);
    seed = hash(seed, s.color.rgba());
    seed = hash(seed, s.outlineColor.rgba());
    seed = hash(seed, s.bgColor.rgba());
    seed = hash(seed, s.size);
    seed = hash(seed, s.tracking);
    seed = hash(seed, s.lineSpacing);
    seed = hash(seed, s.outlineWidth);
    seed = hash(seed, s.bgOpacity);
    seed = hash(seed, s.posX);
    seed = hash(seed, s.posY);
    seed = hash(seed, s.anchor | int(s.scroll) << 2);
    seed = hash(seed, s.scrollProgress);
    seed = hash(seed, s.canvas.width());
    seed = hash(seed, s.canvas.height());
    seed = hash(seed, s.scale);
    return seed;
}

TitleStyle resolveTitleStyle(const ClipData& clip, int64_t localFrame, QSize canvas, double scale)
{
    static const QString kDefaultText = QStringLiteral("Title");
    static const QString kDefaultFont = QStringLiteral("Sans Serif");
    static const QString kLeft = QStringLiteral("left");
    static const QString kRight = QStringLiteral("right");

    const ParamSet& p = clip.title;
    TitleStyle s;
    s.text = stringStatic(p, keys::Text, kDefaultText);
    s.font = stringStatic(p, keys::Font, kDefaultFont);
    s.bold = boolStatic(p, keys::Bold, false);
    s.italic = boolStatic(p, keys::Italic, false);
    const QString align = stringStatic(p, keys::Align, QString());
    s.align = align == kLeft ? 0 : align == kRight ? 2 : 1;
    s.color = colorStatic(p, keys::Color, QColor(Qt::white));
    s.outlineColor = colorStatic(p, keys::OutlineColor, QColor(Qt::black));
    s.bgColor = colorStatic(p, keys::BgColor, QColor(Qt::black));
    s.bgEnabled = boolStatic(p, keys::BgEnabled, false);
    s.shadow = boolStatic(p, keys::Shadow, false);
    s.size = std::max(1.0, p.value(keys::Size, localFrame));
    s.tracking = p.value(keys::Tracking, localFrame);
    s.lineSpacing = p.value(keys::LineSpacing, localFrame);
    s.outlineWidth = std::max(0.0, p.value(keys::OutlineWidth, localFrame));
    s.bgOpacity = std::clamp(p.value(keys::BgOpacity, localFrame), 0.0, 100.0);
    s.posX = p.value(keys::TextPosX, localFrame);
    s.posY = p.value(keys::TextPosY, localFrame);
    const QString anchor = stringStatic(p, keys::Anchor, QString());
    s.anchor = anchor == kLeft ? 0 : anchor == kRight ? 2 : 1;
    s.scroll = boolStatic(p, keys::Scroll, false);
    if (s.scroll && clip.duration > 1) {
        // Over the clip's whole length: the first frame just below, the last just above.
        s.scrollProgress = std::clamp(double(localFrame) / double(clip.duration - 1), 0.0, 1.0);
    }
    s.canvas = canvas;
    s.scale = scale;
    return s;
}

TitleImage renderTitle(const TitleStyle& s)
{
    TitleImage result;
    if (s.text.isEmpty() || s.canvas.isEmpty())
        return result;

    QFont font(s.font);
    const double pixelSize = s.size * s.scale;
    font.setPixelSize(std::max(1, int(std::lround(pixelSize))));
    font.setBold(s.bold);
    font.setItalic(s.italic);
    font.setLetterSpacing(QFont::PercentageSpacing, std::max(1.0, 100.0 + s.tracking));
    font.setHintingPreference(QFont::PreferNoHinting); // smooth scaling/animation
    const QFontMetricsF fm(font);

    // Lay out the lines: baselines lineStep apart, each aligned inside the block width.
    const QStringList lines = s.text.split(QLatin1Char('\n'));
    const double lineStep = fm.lineSpacing() * s.lineSpacing / 100.0;
    double blockWidth = 0;
    for (const QString& line : lines)
        blockWidth = std::max(blockWidth, fm.horizontalAdvance(line));
    const double blockHeight = fm.ascent() + fm.descent() + (lines.size() - 1) * lineStep;
    const double bgPad = s.bgEnabled ? pixelSize * kBgPaddingEm : 0.0;
    const double anchorX = s.posX * s.canvas.width();
    const double left = s.anchor == 0 ? anchorX
        : s.anchor == 2               ? anchorX - blockWidth
                                      : anchorX - blockWidth * 0.5;
    // A scroll starts with the block (and its box) just below the bottom edge and ends with it
    // just above the top edge.
    const double h = s.canvas.height();
    const double top = s.scroll
        ? (h + bgPad) + (-(blockHeight + bgPad) - (h + bgPad)) * s.scrollProgress
        : (1.0 - s.posY) * h - blockHeight * 0.5;
    const QRectF block(left, top, blockWidth, blockHeight);

    QPainterPath path;
    for (int i = 0; i < lines.size(); ++i) {
        const double w = fm.horizontalAdvance(lines[i]);
        const double x = s.align == 0 ? 0.0
            : s.align == 2            ? blockWidth - w
                                      : (blockWidth - w) * 0.5;
        path.addText(block.left() + x, block.top() + fm.ascent() + i * lineStep, font, lines[i]);
    }

    const double outline = s.outlineWidth * s.scale;
    const double shadowOffset = s.shadow ? std::max(kMinShadowPx, pixelSize * kShadowOffsetEm) : 0;
    const QRectF bgRect = block.adjusted(-bgPad, -bgPad, bgPad, bgPad);

    // Bounding box of everything we draw, padded for antialiasing; clipped to a sane maximum.
    QRectF bounds = path.boundingRect().adjusted(-outline, -outline, outline, outline);
    if (s.bgEnabled)
        bounds = bounds.united(bgRect);
    if (s.shadow)
        bounds = bounds.united(bounds.translated(shadowOffset, shadowOffset));
    bounds.adjust(-kAaMarginPx, -kAaMarginPx, kAaMarginPx, kAaMarginPx);
    // A scroll (long credits) only rasterises what is inside the canvas at this frame.
    const QRectF limit = s.scroll
        ? QRectF(QPointF(0, 0), QSizeF(s.canvas))
              .adjusted(-kAaMarginPx, -kAaMarginPx, kAaMarginPx, kAaMarginPx)
        : QRectF(-s.canvas.width() * (kMaxCanvasMultiple - 1) * 0.5,
              -s.canvas.height() * (kMaxCanvasMultiple - 1) * 0.5,
              s.canvas.width() * double(kMaxCanvasMultiple),
              s.canvas.height() * double(kMaxCanvasMultiple));
    const QRect pixelRect = bounds.intersected(limit).toAlignedRect();
    if (pixelRect.isEmpty())
        return result;

    QImage image(pixelRect.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(-pixelRect.topLeft());

    if (s.bgEnabled) {
        QColor bg = s.bgColor;
        bg.setAlphaF(float(bg.alphaF() * s.bgOpacity / 100.0));
        painter.fillRect(bgRect, bg);
    }
    const QPen outlinePen(
        s.outlineColor, 2.0 * outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    if (s.shadow) {
        const QPainterPath shadowPath = path.translated(shadowOffset, shadowOffset);
        const QColor shadowColor(0, 0, 0, kShadowAlpha);
        if (outline > 0)
            painter.strokePath(shadowPath,
                QPen(shadowColor, 2.0 * outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.fillPath(shadowPath, shadowColor);
    }
    // The stroke is centred on the glyph edge; filling afterwards hides its inner half, so the
    // visible outline is `outline` px wide and lies outside the glyphs.
    if (outline > 0)
        painter.strokePath(path, outlinePen);
    painter.fillPath(path, s.color);
    painter.end();

    result.image = std::move(image);
    result.rect = QRectF(pixelRect);
    return result;
}

} // namespace kk::render
