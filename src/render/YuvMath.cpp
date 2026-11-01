// SPDX-License-Identifier: GPL-3.0-or-later
// YuvMath — derivation of the YUV → RGB matrices from the Kr/Kb constants of each standard.
#include "render/YuvMath.h"

namespace kk::render {

int bitDepth(media::PixelFormat format)
{
    switch (format) {
    case media::PixelFormat::P010:
    case media::PixelFormat::Yuv420p10:
    case media::PixelFormat::Yuv422p10:
    case media::PixelFormat::Yuv444p10:
        return 10;
    default:
        return 8;
    }
}

YuvCoefficients yuvCoefficients(
    media::ColorMatrix matrix, media::ColorRange range, media::PixelFormat format)
{
    // Luma weights of each standard (ITU-R BT.601, BT.709, BT.2020 non-constant luminance).
    double kr = 0.2126, kb = 0.0722;
    if (matrix == media::ColorMatrix::Bt601) {
        kr = 0.299;
        kb = 0.114;
    } else if (matrix == media::ColorMatrix::Bt2020) {
        kr = 0.2627;
        kb = 0.0593;
    }
    const double kg = 1.0 - kr - kb;

    // Normalised code values v = code / (2^n − 1). Limited range: Y in [16, 235]·2^(n−8),
    // Cb/Cr in [16, 240]·2^(n−8) centred on 128·2^(n−8). Full range: Y in [0, 2^n − 1], chroma
    // centred on 2^(n−1).
    const int bits = bitDepth(format);
    const double maxCode = double((1 << bits) - 1);
    const double unit = double(1 << (bits - 8)); // one 8-bit step in n-bit codes
    double yOffset = 0.0, yScale = 1.0, cScale = 1.0;
    const double cOffset = (128.0 * unit) / maxCode;
    if (range == media::ColorRange::Limited) {
        yOffset = 16.0 * unit / maxCode;
        yScale = maxCode / (219.0 * unit);
        cScale = maxCode / (224.0 * unit);
    }

    // R = Y' + 2(1−Kr)·Cr ; B = Y' + 2(1−Kb)·Cb ; G = (Y' − Kr·R − Kb·B) / Kg, expanded.
    const double rCr = 2.0 * (1.0 - kr);
    const double bCb = 2.0 * (1.0 - kb);
    const double gCb = -bCb * kb / kg;
    const double gCr = -rCr * kr / kg;

    YuvCoefficients c;
    // Column-major: column 0 multiplies Y, column 1 Cb, column 2 Cr.
    c.matrix = {float(yScale), float(yScale), float(yScale), // Y → R, G, B
        0.0f, float(gCb * cScale), float(bCb * cScale), // Cb → R, G, B
        float(rCr * cScale), float(gCr * cScale), 0.0f}; // Cr → R, G, B
    c.offset = {float(yOffset), float(cOffset), float(cOffset)};

    switch (format) {
    case media::PixelFormat::P010: // 10 bits in the high bits: GPU reads code·64 / 65535
        c.sampleScale = float(65535.0 / (1023.0 * 64.0));
        break;
    case media::PixelFormat::Yuv420p10:
    case media::PixelFormat::Yuv422p10:
    case media::PixelFormat::Yuv444p10: // 10 bits in the low bits: GPU reads code / 65535
        c.sampleScale = float(65535.0 / 1023.0);
        break;
    default:
        c.sampleScale = 1.0f;
        break;
    }
    return c;
}

std::array<float, 3> applyYuv(const YuvCoefficients& c, float y, float cb, float cr)
{
    const float v[3] = {y - c.offset[0], cb - c.offset[1], cr - c.offset[2]};
    std::array<float, 3> rgb {};
    for (int row = 0; row < 3; ++row)
        rgb[size_t(row)] = c.matrix[size_t(row)] * v[0] + c.matrix[size_t(3 + row)] * v[1]
            + c.matrix[size_t(6 + row)] * v[2];
    return rgb;
}

} // namespace kk::render
