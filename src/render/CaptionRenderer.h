// SPDX-License-Identifier: GPL-3.0-or-later
// CaptionRenderer — how a subtitle clip looks and where it sits: resolves the caption → track →
// default style chain into a TitleStyle (rasterised by TitleRenderer, cached like titles) and
// places the text block (bottom / top / middle, left / centre / right, margins).
#pragma once

#include "core/TimelineData.h"
#include "render/TitleRenderer.h"

#include <QPointF>
#include <QRectF>
#include <QSize>

#include <cstdint>

namespace kk::render {

/// Placement of a caption inside the output frame.
struct CaptionPlacement
{
    int position = 0; ///< 0 = bottom, 1 = top, 2 = middle
    double marginV = 0; ///< fraction of the frame height
    double marginH = 0; ///< fraction of the frame width
};

/// Style of caption @p caption on @p track at caption-local frame @p localFrame for a canvas
/// of @p canvas output pixels at @p scale output pixels per timeline pixel.
[[nodiscard]] TitleStyle resolveCaptionStyle(const ClipData& caption, const TrackData* track,
    int64_t localFrame, QSize canvas, double scale, CaptionPlacement* placement);

/// Offset (output px) that moves a rasterised caption block @p rect (TitleImage::rect, centred
/// in the canvas) to its place. @p align is TitleStyle::align.
[[nodiscard]] QPointF captionOffset(
    const QRectF& rect, QSize canvas, int align, const CaptionPlacement& placement);

/// The caption of @p track shown at timeline frame @p frame (enabled Subtitle clip with text),
/// or nullptr.
[[nodiscard]] const ClipData* captionAt(const TrackData& track, int64_t frame);

} // namespace kk::render
