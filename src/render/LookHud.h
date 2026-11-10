// SPDX-License-Identifier: GPL-3.0-or-later
// LookHud — the drawn overlays of the template looks (docs/behavior-spec.md section 65): the
// camera name, date and time of CCTV, the readouts and marks of Drone Overlay, DSLR and Video
// Camera, the name label of Video Call. Every element (text, brackets, grids, icons) is
// painted here with QPainter into a transparent picture the size of the layer; the GPU effect
// (GlTemplateLooks.cpp) uploads it and lays it over the picture. Nothing here needs OpenGL,
// so the drawing is tested without a GPU.
#pragma once

#include "core/Animation.h"

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>

#include <cstdint>

namespace kk::render::lookhud {

/// Where and when an overlay is drawn.
struct Frame
{
    QSize size; ///< layer size in pixels
    QRectF content; ///< the picture inside the layer (the layer minus its padding)
    double scale = 1.0; ///< layer pixels per timeline pixel (for parameters given in px)
    int64_t localFrame = 0; ///< clip-local frame
    double fps = 25.0;
};

/// True for the descriptor ids that have an overlay (CCTV, Drone Overlay, DSLR, Video Call,
/// Video Camera).
[[nodiscard]] bool hasOverlay(const QString& effectId);

/// The overlay of effect @p effectId at @p frame: ARGB32_Premultiplied, transparent where
/// nothing is drawn, with Overlay Opacity applied. Null for an unknown id.
[[nodiscard]] QImage paint(const QString& effectId, const ParamSet& params, const Frame& frame);

/// Text that identifies the overlay for caching: the same key means the same pixels. It holds
/// the frame size, every parameter value at the frame and what changes with time (the clock,
/// the timecode, the blink).
[[nodiscard]] QString cacheKey(const QString& effectId, const ParamSet& params, const Frame& frame);

/// CCTV: the date and time shown at @p localFrame (Start Date and Time plus the clip time,
/// written with Time Format).
[[nodiscard]] QString cctvTimestamp(const ParamSet& params, int64_t localFrame, double fps);
/// Video Camera: the running timecode HH:MM:SS:FF (Start Timecode plus the clip time).
[[nodiscard]] QString cameraTimecode(const ParamSet& params, int64_t localFrame, double fps);
/// Elapsed clip time as M:SS (H:MM:SS from an hour on), as in call timers and recording times.
[[nodiscard]] QString elapsedText(int64_t localFrame, double fps);
/// True while a blinking dot is lit: on for the first half second of each second.
[[nodiscard]] bool blinkOn(int64_t localFrame, double fps);

} // namespace kk::render::lookhud
