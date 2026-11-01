// SPDX-License-Identifier: GPL-3.0-or-later
// TitleRenderer — rasterises a Title clip's text (outline, shadow, background box, alignment,
// size/tracking/line spacing) with QPainter into a tightly cropped QImage.
#pragma once

#include "core/TimelineData.h"

#include <QColor>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>

#include <cstdint>

namespace kk::render {

/// Everything that influences a title's pixels at one frame, resolved from ClipData::title.
/// Used as the title texture cache key: equal styles produce identical images, so a static
/// title is rasterised once and then costs nothing per frame.
struct TitleStyle
{
    QString text;
    QString font;
    bool bold = false;
    bool italic = false;
    int align = 1; ///< 0 = left, 1 = centre, 2 = right (lines inside the text block)
    QColor color;
    QColor outlineColor;
    QColor bgColor;
    bool bgEnabled = false;
    bool shadow = false;
    double size = 72; ///< font pixel size at timeline resolution
    double tracking = 0; ///< percent letter spacing added (0 = normal)
    double lineSpacing = 100; ///< percent of the font's natural line spacing
    double outlineWidth = 0; ///< px at timeline resolution, drawn outside the glyphs
    double bgOpacity = 60; ///< percent
    QSize canvas; ///< size of the full title frame in OUTPUT pixels (timeline size × scale)
    double scale = 1.0; ///< output pixels per timeline pixel

    friend bool operator==(const TitleStyle&, const TitleStyle&) = default;
};

/// Hash of a TitleStyle (for the texture cache).
[[nodiscard]] size_t qHash(const TitleStyle& style, size_t seed = 0);

/// Resolves the title parameters of @p clip at clip-local frame @p localFrame (defaults from
/// kk::keys / the text_title descriptor) for a canvas of @p canvas output pixels at @p scale
/// output pixels per timeline pixel. Cheap: no allocation (QString copies are ref-counted).
[[nodiscard]] TitleStyle resolveTitleStyle(
    const ClipData& clip, int64_t localFrame, QSize canvas, double scale);

/// A rasterised title: `image` (ARGB32_Premultiplied) covers `rect` of the title canvas.
/// The text block is centred in the canvas (the clip transform positions it from there).
/// `image` is null when there is nothing to draw (empty text).
struct TitleImage
{
    QImage image;
    QRectF rect; ///< position of `image` inside the canvas, output pixels
};

/// Rasterises @p style. Thread-safe (QPainter on a QImage); takes ~1-5 ms at 1080p.
[[nodiscard]] TitleImage renderTitle(const TitleStyle& style);

} // namespace kk::render
