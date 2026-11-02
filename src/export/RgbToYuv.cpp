// SPDX-License-Identifier: GPL-3.0-or-later
// RgbToYuv — fixed-point BT.709 RGB → YUV 4:2:0.
#include "export/RgbToYuv.h"

#include <QtGlobal>

namespace kk::exporter {

namespace {

// BT.709, limited range, 8-bit, coefficients × 2^16 (Kr = 0.2126, Kb = 0.0722):
//   Y  = 16  + 219/255 · (Kr R + Kg G + Kb B)
//   Cb = 128 + 224/255 · (B − Y') / 1.8556
//   Cr = 128 + 224/255 · (R − Y') / 1.5748
constexpr int kYR = 11966;
constexpr int kYG = 40254;
constexpr int kYB = 4064;
constexpr int kUR = -6596;
constexpr int kUG = -22189;
constexpr int kUB = 28784;
constexpr int kVR = 28784;
constexpr int kVG = -26145;
constexpr int kVB = -2639;
constexpr int kYOffset = (16 << 16) + (1 << 15); ///< black level + rounding
constexpr int kCOffset4 = (128 << 18) + (1 << 17); ///< chroma from a 4-pixel sum: >> 18

// QImage ARGB32 is a native-endian 0xAARRGGBB word: byte order B, G, R, A on little endian.
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
constexpr int kB = 0, kG = 1, kR = 2;
#else
constexpr int kB = 3, kG = 2, kR = 1;
#endif

inline uint8_t lumaOf(const uint8_t* p)
{
    return uint8_t((kYR * p[kR] + kYG * p[kG] + kYB * p[kB] + kYOffset) >> 16);
}

} // namespace

void rgb32ToYuv420(
    const uint8_t* src, int srcStride, int width, int rowBegin, int rowEnd, const Yuv420Planes& dst)
{
    // Two source rows per iteration: 4 luma samples and one chroma pair per 2x2 block. Plain
    // integer code without branches in the inner loop; GCC/Clang vectorise the luma part.
    for (int y = rowBegin; y < rowEnd; y += 2) {
        const uint8_t* row0 = src + qsizetype(y) * srcStride;
        const uint8_t* row1 = row0 + srcStride;
        uint8_t* y0 = dst.y + qsizetype(y) * dst.yStride;
        uint8_t* y1 = y0 + dst.yStride;
        uint8_t* u = dst.u + qsizetype(y / 2) * dst.uStride;
        uint8_t* v = dst.nv12 ? nullptr : dst.v + qsizetype(y / 2) * dst.vStride;
        for (int x = 0; x < width; x += 2) {
            const uint8_t* a = row0 + x * 4;
            const uint8_t* b = a + 4;
            const uint8_t* c = row1 + x * 4;
            const uint8_t* d = c + 4;
            y0[x] = lumaOf(a);
            y0[x + 1] = lumaOf(b);
            y1[x] = lumaOf(c);
            y1[x + 1] = lumaOf(d);
            const int r = a[kR] + b[kR] + c[kR] + d[kR];
            const int g = a[kG] + b[kG] + c[kG] + d[kG];
            const int bl = a[kB] + b[kB] + c[kB] + d[kB];
            const uint8_t cb = uint8_t((kUR * r + kUG * g + kUB * bl + kCOffset4) >> 18);
            const uint8_t cr = uint8_t((kVR * r + kVG * g + kVB * bl + kCOffset4) >> 18);
            if (dst.nv12) {
                u[x] = cb;
                u[x + 1] = cr;
            } else {
                u[x / 2] = cb;
                v[x / 2] = cr;
            }
        }
    }
}

} // namespace kk::exporter
