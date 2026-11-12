// SPDX-License-Identifier: GPL-3.0-or-later
// TitleRenderer — QPainter text rasterisation for Title clips.
#include "render/TitleRenderer.h"

#include "core/ParamKeys.h"
#include "core/TitleLayers.h"
#include "render/EmojiText.h"
#include "render/FontReload.h"

#include <QFont>
#include <QFontMetricsF>
#include <QHashFunctions>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QTextBoundaryFinder>
#include <QTransform>

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

int currentFontGeneration()
{
    return fonts::fontGeneration();
}

QBitArray writeOnVisible(const QString& text, double start, double end)
{
    QBitArray visible(int(text.size()), true);
    start = std::clamp(start, 0.0, 1.0);
    end = std::clamp(end, 0.0, 1.0);
    if (start <= 0.0 && end >= 1.0)
        return visible;
    // Grapheme clusters as [from, to) UTF-16 ranges, without line breaks.
    std::vector<std::pair<int, int>> clusters;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    int from = 0;
    for (int to = finder.toNextBoundary(); to > 0; from = to, to = finder.toNextBoundary()) {
        const QString piece = text.mid(from, to - from);
        if (piece == QLatin1String("\n") || piece == QLatin1String("\r")
            || piece == QLatin1String("\r\n"))
            continue;
        clusters.emplace_back(from, to);
    }
    const double n = double(clusters.size());
    const int first = int(std::lround(start * n));
    const int last = int(std::lround(end * n));
    for (int i = 0; i < int(clusters.size()); ++i) {
        if (i >= first && i < last)
            continue;
        for (int u = clusters[size_t(i)].first; u < clusters[size_t(i)].second; ++u)
            visible.clearBit(u);
    }
    return visible;
}

size_t qHash(const TitleStyle& s, size_t seed)
{
    // Qt 6.12 deprecates the seedless combiner (the seed must reach the element hashes);
    // Qt 6.4 only has the default constructor.
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    QtPrivate::QHashCombine hash(seed);
#else
    QtPrivate::QHashCombine hash;
#endif
    seed = hash(seed, s.fontGeneration);
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
    seed = hash(seed, s.writeOnStart);
    seed = hash(seed, s.writeOnEnd);
    seed = hash(seed, s.outlineWidth);
    seed = hash(seed, s.bgOpacity);
    seed = hash(seed, s.posX);
    seed = hash(seed, s.posY);
    seed = hash(seed, s.anchor | int(s.scroll) << 2);
    seed = hash(seed, s.scrollProgress);
    seed = hash(seed, s.canvas.width());
    seed = hash(seed, s.canvas.height());
    seed = hash(seed, s.scale);
    seed = hash(seed, int(s.kind) | int(s.shape) << 1);
    seed = hash(seed, s.rotation);
    seed = hash(seed, s.opacity);
    seed = hash(seed, s.shapeWidth);
    seed = hash(seed, s.shapeHeight);
    seed = hash(seed, s.cornerRadius);
    seed = hash(seed, s.fillOpacity);
    for (const TitleStyle& layer : s.stack)
        seed = hash(seed, qHash(layer, seed));
    return seed;
}

namespace {

TitleStyle::Shape shapeOf(const QString& name)
{
    if (name == QLatin1String("rounded"))
        return TitleStyle::Shape::Rounded;
    if (name == QLatin1String("ellipse"))
        return TitleStyle::Shape::Ellipse;
    if (name == QLatin1String("line"))
        return TitleStyle::Shape::Line;
    return TitleStyle::Shape::Rectangle;
}

} // namespace

TitleStyle resolveLayerStyle(const ParamSet& p, TitleLayerKind kind, const ClipData& clip,
    int64_t localFrame, QSize canvas, double scale)
{
    static const QString kDefaultText = QStringLiteral("Title");
    static const QString kDefaultFont = QStringLiteral("Sans Serif");
    static const QString kLeft = QStringLiteral("left");
    static const QString kRight = QStringLiteral("right");

    TitleStyle s;
    s.canvas = canvas;
    s.scale = scale;
    s.rotation = p.value(keys::LayerRotation, localFrame);
    s.opacity = std::clamp(p.value(keys::LayerOpacity, localFrame), 0.0, 100.0);
    s.posX = p.value(keys::TextPosX, localFrame);
    s.posY = p.value(keys::TextPosY, localFrame);
    s.outlineColor = colorStatic(p, keys::OutlineColor, QColor(Qt::black));
    s.outlineWidth = std::max(0.0, p.value(keys::OutlineWidth, localFrame));
    if (kind == TitleLayerKind::Shape) {
        s.kind = TitleStyle::Kind::Shape;
        s.shape = shapeOf(stringStatic(p, keys::ShapeType, QString()));
        s.color = colorStatic(p, keys::Color, QColor(0x2d, 0x7d, 0xd2));
        s.outlineColor = colorStatic(p, keys::OutlineColor, QColor(Qt::white));
        s.shapeWidth = std::max(1.0, p.value(keys::ShapeWidth, localFrame));
        s.shapeHeight = std::max(1.0, p.value(keys::ShapeHeight, localFrame));
        s.cornerRadius = std::max(0.0, p.value(keys::CornerRadius, localFrame));
        s.fillOpacity = std::clamp(p.value(keys::FillOpacity, localFrame), 0.0, 100.0);
        return s;
    }
    s.text = stringStatic(p, keys::Text, kDefaultText);
    s.font = stringStatic(p, keys::Font, kDefaultFont);
    s.bold = boolStatic(p, keys::Bold, false);
    s.italic = boolStatic(p, keys::Italic, false);
    const QString align = stringStatic(p, keys::Align, QString());
    s.align = align == kLeft ? 0 : align == kRight ? 2 : 1;
    s.color = colorStatic(p, keys::Color, QColor(Qt::white));
    s.bgColor = colorStatic(p, keys::BgColor, QColor(Qt::black));
    s.bgEnabled = boolStatic(p, keys::BgEnabled, false);
    s.shadow = boolStatic(p, keys::Shadow, false);
    s.size = std::max(1.0, p.value(keys::Size, localFrame));
    s.tracking = p.value(keys::Tracking, localFrame);
    s.lineSpacing = p.value(keys::LineSpacing, localFrame);
    s.writeOnStart = std::clamp(p.value(keys::WriteOnStart, localFrame), 0.0, 1.0);
    s.writeOnEnd = std::clamp(p.value(keys::WriteOnEnd, localFrame), 0.0, 1.0);
    s.bgOpacity = std::clamp(p.value(keys::BgOpacity, localFrame), 0.0, 100.0);
    const QString anchor = stringStatic(p, keys::Anchor, QString());
    s.anchor = anchor == kLeft ? 0 : anchor == kRight ? 2 : 1;
    s.scroll = boolStatic(p, keys::Scroll, false);
    if (s.scroll && clip.duration > 1) {
        // Over the clip's whole length: the first frame just below, the last just above.
        s.scrollProgress = std::clamp(double(localFrame) / double(clip.duration - 1), 0.0, 1.0);
    }
    return s;
}

TitleStyle resolveTitleStyle(const ClipData& clip, int64_t localFrame, QSize canvas, double scale)
{
    if (titlelayers::isPlain(clip))
        return resolveLayerStyle(clip.title, TitleLayerKind::Text, clip, localFrame, canvas, scale);
    // Several layers (or a hidden first one): the shown ones, bottom to top.
    TitleStyle s;
    s.canvas = canvas;
    s.scale = scale;
    for (const quint64 id : titlelayers::order(clip)) {
        if (!titlelayers::visible(clip, id))
            continue;
        s.stack.push_back(resolveLayerStyle(*titlelayers::params(clip, id),
            titlelayers::kind(clip, id), clip, localFrame, canvas, scale));
    }
    return s;
}

namespace {

/// Multiplies the whole image by @p percent (layer opacity).
void applyLayerOpacity(QImage& image, double percent)
{
    if (percent >= 100.0)
        return;
    QPainter p(&image);
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.fillRect(image.rect(), QColor(0, 0, 0, int(std::lround(percent * 2.55))));
}

/// Maps @p rect (degrees clockwise) around its own centre.
QTransform rotationAround(const QPointF& centre, double degrees)
{
    QTransform t;
    t.translate(centre.x(), centre.y());
    t.rotate(degrees);
    t.translate(-centre.x(), -centre.y());
    return t;
}

TitleImage renderText(const TitleStyle& s)
{
    TitleImage result;
    if (s.text.isEmpty() || s.canvas.isEmpty())
        return result;

    emoji::registerBundledFont();
    QFont font(s.font);
    const double pixelSize = s.size * s.scale;
    font.setPixelSize(std::max(1, int(std::lround(pixelSize))));
    font.setBold(s.bold);
    font.setItalic(s.italic);
    font.setLetterSpacing(QFont::PercentageSpacing, std::max(1.0, 100.0 + s.tracking));
    font.setHintingPreference(QFont::PreferNoHinting); // smooth scaling/animation
    const QFontMetricsF fm(font);
    // The font of a text with emoji falls back to a colour emoji font for them.
    QFont emojiFont = font;
    if (emoji::containsEmoji(s.text))
        emojiFont.setFamilies(emoji::familiesWithEmoji(s.font));
    const QFontMetricsF emojiMetrics(emojiFont);

    // Lay out the lines: baselines lineStep apart, each aligned inside the block width.
    const QStringList lines = s.text.split(QLatin1Char('\n'));
    const double lineStep = fm.lineSpacing() * s.lineSpacing / 100.0;
    double blockWidth = 0;
    for (const QString& line : lines)
        blockWidth = std::max(blockWidth, emojiMetrics.horizontalAdvance(line));
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

    // Colour emoji have no outline to put in the path: they are cut out of the lines and drawn
    // as glyphs (spec section 69). Text without emoji takes the old path unchanged.
    struct EmojiRun
    {
        QString text;
        QPointF origin; ///< left end of the baseline
        double advance = 0;
    };
    QVector<EmojiRun> emojiRuns;
    QPainterPath path;
    // Write On: hidden characters are left out but the line keeps its width and alignment, so
    // the visible ones stay where they will be once the whole text is shown.
    const QBitArray shown = writeOnVisible(s.text, s.writeOnStart, s.writeOnEnd);
    const bool partial = shown.count(false) > 0;
    const auto addRun = [&](const QString& text, double x, double baseline) {
        if (!emoji::containsEmoji(text)) {
            path.addText(x, baseline, font, text);
            return;
        }
        double runX = x;
        for (const emoji::Run& run : emoji::split(text)) {
            const double advance = emojiMetrics.horizontalAdvance(run.text);
            if (run.emoji)
                emojiRuns.append({run.text, QPointF(runX, baseline), advance});
            else
                path.addText(runX, baseline, font, run.text);
            runX += advance;
        }
    };
    int lineOffset = 0; // UTF-16 offset of the line inside s.text
    for (int i = 0; i < lines.size(); ++i) {
        const double w = emojiMetrics.horizontalAdvance(lines[i]);
        const double x = s.align == 0 ? 0.0
            : s.align == 2            ? blockWidth - w
                                      : (blockWidth - w) * 0.5;
        const double baseline = block.top() + fm.ascent() + i * lineStep;
        if (!partial) {
            addRun(lines[i], block.left() + x, baseline);
        } else {
            const int len = int(lines[i].size());
            int a = 0;
            while (a < len) {
                if (!shown.testBit(lineOffset + a)) {
                    ++a;
                    continue;
                }
                int b = a;
                while (b < len && shown.testBit(lineOffset + b))
                    ++b;
                addRun(lines[i].mid(a, b - a),
                    block.left() + x + emojiMetrics.horizontalAdvance(lines[i].left(a)), baseline);
                a = b;
            }
        }
        lineOffset += int(lines[i].size()) + 1;
    }
    if (partial && path.isEmpty() && emojiRuns.isEmpty() && !s.bgEnabled)
        return result; // nothing typed yet

    const double outline = s.outlineWidth * s.scale;
    const double shadowOffset = s.shadow ? std::max(kMinShadowPx, pixelSize * kShadowOffsetEm) : 0;
    const QRectF bgRect = block.adjusted(-bgPad, -bgPad, bgPad, bgPad);
    // A rotated layer turns the glyphs and the box around the middle of the block.
    const bool rotated = s.rotation != 0.0;
    QPolygonF bgPolygon(bgRect);
    QTransform turn;
    if (rotated) {
        turn = rotationAround(block.center(), s.rotation);
        path = turn.map(path);
        bgPolygon = turn.map(bgPolygon);
    }

    // Bounding box of everything we draw, padded for antialiasing; clipped to a sane maximum.
    QRectF bounds = path.isEmpty() && !emojiRuns.isEmpty()
        ? QRectF()
        : path.boundingRect().adjusted(-outline, -outline, outline, outline);
    for (const EmojiRun& run : std::as_const(emojiRuns)) {
        const QRectF box(
            run.origin.x(), run.origin.y() - fm.ascent(), run.advance, fm.ascent() + fm.descent());
        bounds = bounds.united(rotated ? turn.mapRect(box) : box);
    }
    if (s.bgEnabled)
        bounds = bounds.united(rotated ? bgPolygon.boundingRect() : bgRect);
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

    // The emoji of the text on a layer of their own (shadow and opacity follow the text).
    QImage emojiLayer;
    if (!emojiRuns.isEmpty()) {
        emojiLayer = QImage(pixelRect.size(), QImage::Format_ARGB32_Premultiplied);
        emojiLayer.fill(Qt::transparent);
        QPainter ep(&emojiLayer);
        ep.setRenderHints(
            QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        ep.translate(-pixelRect.topLeft());
        if (rotated)
            ep.setTransform(turn, true);
        ep.setFont(emojiFont);
        ep.setPen(Qt::white);
        for (const EmojiRun& run : std::as_const(emojiRuns))
            ep.drawText(run.origin, run.text);
    }

    if (s.bgEnabled) {
        QColor bg = s.bgColor;
        bg.setAlphaF(float(bg.alphaF() * s.bgOpacity / 100.0));
        if (rotated) {
            painter.setBrush(bg);
            painter.setPen(Qt::NoPen);
            painter.drawPolygon(bgPolygon);
        } else
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
        if (!emojiLayer.isNull()) {
            QImage tint = emojiLayer;
            QPainter tp(&tint);
            tp.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tp.fillRect(tint.rect(), shadowColor);
            tp.end();
            painter.drawImage(pixelRect.topLeft() + QPointF(shadowOffset, shadowOffset), tint);
        }
    }
    // The stroke is centred on the glyph edge; filling afterwards hides its inner half, so the
    // visible outline is `outline` px wide and lies outside the glyphs.
    if (outline > 0)
        painter.strokePath(path, outlinePen);
    painter.fillPath(path, s.color);
    if (!emojiLayer.isNull())
        painter.drawImage(pixelRect.topLeft(), emojiLayer);
    painter.end();
    applyLayerOpacity(image, s.opacity);

    result.image = std::move(image);
    result.rect = QRectF(pixelRect);
    return result;
}

/// A shape layer: filled and stroked outline centred at posX / posY, turned by the rotation.
TitleImage renderShape(const TitleStyle& s)
{
    TitleImage result;
    if (s.canvas.isEmpty())
        return result;
    const double w = std::max(1.0, s.shapeWidth * s.scale);
    const double h = std::max(1.0, s.shapeHeight * s.scale);
    const QPointF centre(s.posX * s.canvas.width(), (1.0 - s.posY) * s.canvas.height());
    const QRectF box(-w / 2, -h / 2, w, h);
    QPainterPath path;
    switch (s.shape) {
    case TitleStyle::Shape::Rectangle:
        path.addRect(box);
        break;
    case TitleStyle::Shape::Rounded: {
        const double r = std::min(s.cornerRadius * s.scale, std::min(w, h) / 2);
        path.addRoundedRect(box, r, r);
        break;
    }
    case TitleStyle::Shape::Ellipse:
        path.addEllipse(box);
        break;
    case TitleStyle::Shape::Line:
        path.addRoundedRect(box, std::min(w, h) / 2, std::min(w, h) / 2);
        break;
    }
    QTransform place;
    place.translate(centre.x(), centre.y());
    place.rotate(s.rotation);
    path = place.map(path);

    const double stroke = s.outlineWidth * s.scale;
    const QRectF bounds = path.boundingRect().adjusted(-stroke / 2 - kAaMarginPx,
        -stroke / 2 - kAaMarginPx, stroke / 2 + kAaMarginPx, stroke / 2 + kAaMarginPx);
    const QRectF limit(-s.canvas.width() * (kMaxCanvasMultiple - 1) * 0.5,
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
    QColor fill = s.color;
    fill.setAlphaF(float(fill.alphaF() * s.fillOpacity / 100.0));
    painter.fillPath(path, fill);
    if (stroke > 0)
        painter.strokePath(
            path, QPen(s.outlineColor, stroke, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    painter.end();
    applyLayerOpacity(image, s.opacity);

    result.image = std::move(image);
    result.rect = QRectF(pixelRect);
    return result;
}

/// The shown layers of a title composited bottom to top into one image.
TitleImage renderStack(const TitleStyle& s)
{
    std::vector<TitleImage> parts;
    QRect united;
    for (const TitleStyle& layer : s.stack) {
        TitleImage part = renderTitle(layer);
        if (part.image.isNull())
            continue;
        united = united.united(part.rect.toRect());
        parts.push_back(std::move(part));
    }
    if (parts.empty())
        return {};
    if (parts.size() == 1)
        return std::move(parts.front());
    QImage image(united.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    for (const TitleImage& part : parts)
        painter.drawImage(part.rect.toRect().topLeft() - united.topLeft(), part.image);
    painter.end();
    TitleImage result;
    result.image = std::move(image);
    result.rect = QRectF(united);
    return result;
}

} // namespace

TitleImage renderTitle(const TitleStyle& style)
{
    if (!style.stack.empty())
        return renderStack(style);
    return style.kind == TitleStyle::Kind::Shape ? renderShape(style) : renderText(style);
}

} // namespace kk::render
