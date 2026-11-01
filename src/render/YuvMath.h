// SPDX-License-Identifier: GPL-3.0-or-later
// YuvMath — YUV → RGB conversion coefficients (BT.601/709/2020, limited/full range, 8/10-bit)
// as used by the compositor's conversion shader. Pure maths, unit-tested on the CPU.
#pragma once

#include "media/VideoFrame.h"

#include <array>

namespace kk::render {

/// rgb = matrix · (sample · sampleScale − offset), all values normalised to [0, 1].
///
/// `sampleScale` maps what the GPU returns for a normalised texture of the plane format to
/// "code / (2^bits − 1)": 1 for 8-bit; 65535/1023 for 10-bit stored in the LOW bits of 16-bit
/// words (planar yuv4xxp10); 65535/(1023·64) for P010 (10 bits in the HIGH bits).
struct YuvCoefficients
{
    std::array<float, 9> matrix {}; ///< 3×3, COLUMN-major (glUniformMatrix3fv, transpose false)
    std::array<float, 3> offset {}; ///< subtracted from (Y, Cb, Cr) before the matrix
    float sampleScale = 1.0f;
};

/// Coefficients for @p matrix / @p range at @p bitDepth (8 or 10) with samples stored as
/// described by @p format (only the 10-bit packing matters for sampleScale).
[[nodiscard]] YuvCoefficients yuvCoefficients(
    media::ColorMatrix matrix, media::ColorRange range, media::PixelFormat format);

/// Bit depth of the samples of @p format (8 or 10; Bgra → 8).
[[nodiscard]] int bitDepth(media::PixelFormat format);

/// CPU reference conversion of one sample triple (normalised code values, i.e. code/(2^n−1))
/// with @p c. Used by tests to cross-check the shader.
[[nodiscard]] std::array<float, 3> applyYuv(const YuvCoefficients& c, float y, float cb, float cr);

} // namespace kk::render
