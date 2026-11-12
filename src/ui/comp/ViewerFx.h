// SPDX-License-Identifier: GPL-3.0-or-later
// viewerfx — pure picture helpers of the Compositing viewers: single channel display, the checker
// underlay for alpha and the A / B split wipe. They work on QImage so tests need no GL.
#pragma once

#include <QImage>
#include <QPointF>

namespace kk::viewerfx {

/// What a viewer shows of the picture (Fusion's C, R, G, B, A keys).
enum class Channel { Color, Red, Green, Blue, Alpha };

/// @p image (any format) with only @p channel shown as grey; Color returns it unchanged. The
/// result is opaque ARGB32_Premultiplied.
[[nodiscard]] QImage showChannel(const QImage& image, Channel channel);

/// @p image over a two-grey checker (squares of @p square pixels), opaque.
[[nodiscard]] QImage overChecker(const QImage& image, int square = 16);

/// Split wipe: buffer A on one side of a line through (@p pos.x, @p pos.y) (fractions of the
/// picture) at @p angleDeg (90 = vertical line, A on its left), buffer B on the other. B is
/// scaled to A's size when they differ.
[[nodiscard]] QImage splitWipe(const QImage& a, const QImage& b, QPointF pos, double angleDeg);

/// True when the pixel (@p x, @p y) of a picture of @p size shows buffer A in splitWipe().
[[nodiscard]] bool isSideA(QSize size, QPointF pos, double angleDeg, int x, int y);

} // namespace kk::viewerfx
