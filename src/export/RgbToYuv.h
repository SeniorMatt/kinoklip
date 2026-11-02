// SPDX-License-Identifier: GPL-3.0-or-later
// RgbToYuv — internal: fast RGB32 (QImage ARGB32 premultiplied, alpha ignored) → 8-bit 4:2:0
// BT.709 limited range, planar (I420) or semi-planar (NV12). The hot path of every delivery
// export (H.264/H.265/AV1/VP9, software and hardware encoders).
#pragma once

#include <cstdint>

namespace kk::exporter {

/// Destination planes of a 4:2:0 frame. For NV12, `u` is the interleaved UV plane and `v` is
/// unused.
struct Yuv420Planes
{
    uint8_t* y = nullptr;
    int yStride = 0;
    uint8_t* u = nullptr;
    int uStride = 0;
    uint8_t* v = nullptr;
    int vStride = 0;
    bool nv12 = false;
};

/// Converts rows [rowBegin, rowEnd) of @p src (both even, except rowEnd == height) into
/// @p dst. Width and height must be even. Fixed-point (16-bit) BT.709 coefficients with
/// rounding; chroma is the average of each 2x2 block (centred siting, like swscale's
/// bilinear chroma). Thread-safe for disjoint row ranges, so callers split the frame into
/// bands and convert them in parallel. Measured on the (heavily loaded) dev box: one core
/// ~7 ms per 1080p frame, 2.3-3.7 ms with VideoEncoder's bands; swscale's bgra → yuv420p
/// path took 22-26 ms per frame under the same load (6-10 ms in isolation).
void rgb32ToYuv420(const uint8_t* src, int srcStride, int width, int rowBegin, int rowEnd,
    const Yuv420Planes& dst);

} // namespace kk::exporter
