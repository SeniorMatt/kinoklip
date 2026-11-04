// SPDX-License-Identifier: GPL-3.0-or-later
// HdrToSdr — shows HDR (HLG / PQ) and wide-gamut BT.2020 video on the SDR BT.709 timeline:
// the conversion used by the CPU frame converter and (as GLSL) by the GPU video shader.
#pragma once

#include "media/VideoFrame.h"

#include <QImage>

#include <array>

struct AVFrame;

namespace kk::media {

/// How a frame's colours are mapped to the SDR BT.709 working space.
enum class HdrMapping {
    None, ///< SDR BT.709 / BT.601 (or untagged): shown as is
    Hlg, ///< BT.2100 HLG, BT.2020 primaries (phones: iPhone "HDR video", Android HDR10+ HLG)
    Pq, ///< BT.2100 PQ (HDR10), BT.2020 primaries
    WideGamut, ///< SDR transfer with BT.2020 primaries: gamut conversion only
};

/// The mapping a frame needs: HLG / PQ by its transfer tag, WideGamut for SDR frames tagged
/// with BT.2020 primaries. HLG / PQ frames are assumed to use BT.2020 primaries (as every
/// HDR camera writes them).
[[nodiscard]] HdrMapping hdrMappingOf(const AVFrame* frame);
/// Same for a decoded VideoFrame (its transfer, and the primaries of its AVFrame if any).
[[nodiscard]] HdrMapping hdrMappingOf(const VideoFrame& frame);

/// Maps one non-linear R'G'B' value (0..1, BT.2020 signal) to display-referred BT.709 R'G'B'
/// (0..1). The reference implementation of the GLSL below:
///  1. linearise: HLG inverse OETF + the BT.2100 OOTF (system gamma 1.2, 1000 cd/m2);
///     PQ EOTF; WideGamut: gamma 2.4;
///  2. scale so that HDR reference white (203 cd/m2, BT.2408) is 1.0;
///  3. BT.2020 → BT.709 primaries (linear light, negative values clipped);
///  4. tone map (HLG / PQ): linear below 0.8, highlights rolled off smoothly towards 1.0;
///  5. encode with the BT.1886 display gamma (1 / 2.4), as SDR video is shown.
[[nodiscard]] std::array<float, 3> mapToSdr(std::array<float, 3> rgb, HdrMapping mapping);

/// Applies mapToSdr() to every pixel of an opaque ARGB32 / ARGB32_Premultiplied @p image in
/// place (lookup tables; no-op for HdrMapping::None).
void mapImageToSdr(QImage* image, HdrMapping mapping);

/// GLSL source of `vec3 hdrToSdr(vec3 rgb, int mapping)` (mapping = int(HdrMapping)), the same
/// maths as mapToSdr(), for the video shader.
[[nodiscard]] const char* hdrToSdrGlsl();

} // namespace kk::media
