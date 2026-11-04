// SPDX-License-Identifier: GPL-3.0-or-later
// Dynamic Zoom (the Inspector's Video tab, spec 24.20): a clip moves from a start framing to
// an end framing over its length, like a pan-and-zoom over a still. The framing at each frame
// is a rectangle of the picture (keeping the frame's aspect) that fills the frame.
#pragma once

#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QTransform>

#include <cstdint>

namespace kk {

struct ClipData;
struct ParamSet;

namespace dynamiczoom {

/// A framing rectangle in fractions of the frame: centre (0.5, 0.5 = the middle) and width
/// (1 = the whole frame; the height is the same fraction of the frame height).
struct Rect
{
    double cx = 0.5;
    double cy = 0.5;
    double size = 1.0;

    friend bool operator==(const Rect&, const Rect&) = default;
};

/// Smallest rectangle size (a 20x zoom).
inline constexpr double kMinSize = 0.05;

/// Ease of the move ("linear", "easeIn", "easeOut", "easeInOut"), in the Inspector's order.
[[nodiscard]] QStringList easeIds();
/// Display names matching easeIds() (Linear, Ease In, Ease Out, Ease In and Out).
[[nodiscard]] QStringList easeLabels();

/// The defaults of a clip that never had Dynamic Zoom: a slow push-in from the whole frame to
/// a slightly smaller centred rectangle.
[[nodiscard]] Rect defaultStart();
[[nodiscard]] Rect defaultEnd();

/// True if the clip's Dynamic Zoom is switched on.
[[nodiscard]] bool isEnabled(const ParamSet& transform);
[[nodiscard]] Rect start(const ParamSet& transform);
[[nodiscard]] Rect end(const ParamSet& transform);
[[nodiscard]] QString ease(const ParamSet& transform);
void setStart(ParamSet& transform, const Rect& r);
void setEnd(ParamSet& transform, const Rect& r);
/// Swaps the start and end rectangles (the Inspector's Swap button: a push-in becomes a pull-out).
void swap(ParamSet& transform);
/// Removes every Dynamic Zoom setting (off, default rectangles and ease).
void reset(ParamSet& transform);

/// @p t (0..1, the position in the clip) through the ease @p easeId.
[[nodiscard]] double eased(double t, const QString& easeId);
/// The framing rectangle of @p clip at clip-local frame @p localFrame (the start rectangle on
/// the first frame, the end rectangle on the last), whether or not it is enabled.
[[nodiscard]] Rect at(const ClipData& clip, int64_t localFrame);

/// The zoom of @p clip at @p localFrame in the picture coordinates of render::clipTransform()
/// (frame px, origin at the frame centre): maps the framing rectangle onto the whole frame.
/// Identity when Dynamic Zoom is off.
[[nodiscard]] QTransform transform(const ClipData& clip, int64_t localFrame, QSizeF frameSize);
/// The rectangle @p r in those picture coordinates (frame px, origin at the frame centre).
[[nodiscard]] QRectF toPicture(const Rect& r, QSizeF frameSize);
/// Inverse of toPicture() (the size from the width).
[[nodiscard]] Rect fromPicture(const QRectF& rect, QSizeF frameSize);

} // namespace dynamiczoom
} // namespace kk
