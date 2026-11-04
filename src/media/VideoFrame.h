// SPDX-License-Identifier: GPL-3.0-or-later
// VideoFrame — a decoded video frame as raw planes (for GPU upload) + its colour description.
#pragma once

#include "core/Rational.h"

#include <array>
#include <cstdint>
#include <memory>

struct AVFrame;

namespace kk::media {

/// Memory layout of a VideoFrame's planes. Only these layouts ever reach consumers: any other
/// decoder output (12-bit, alpha YUV, RGB codecs, stills...) is converted to `Bgra`.
///
/// | Format       | Planes | Layout                                                    |
/// |--------------|--------|-----------------------------------------------------------|
/// | Nv12         | 2      | Y 8-bit; UV interleaved 8-bit, ½ width × ½ height (HW)     |
/// | P010         | 2      | like NV12 but 16-bit words, 10 bits in the HIGH bits (HW) |
/// | Yuv420p      | 3      | Y, U, V 8-bit; U/V ½ width × ½ height                     |
/// | Yuv422p      | 3      | Y, U, V 8-bit; U/V ½ width × full height                  |
/// | Yuv444p      | 3      | Y, U, V 8-bit, all full size                              |
/// | Yuv420p10    | 3      | like Yuv420p, 16-bit LE words, 10 bits in the LOW bits    |
/// | Yuv422p10    | 3      | like Yuv422p, 16-bit LE words, 10 bits in the LOW bits    |
/// | Yuv444p10    | 3      | like Yuv444p, 16-bit LE words, 10 bits in the LOW bits    |
/// | Bgra         | 1      | 8-bit B,G,R,A bytes, alpha PREMULTIPLIED (= QImage        |
/// |              |        | Format_ARGB32_Premultiplied on little-endian); RGB, no    |
/// |              |        | colour matrix                                             |
enum class PixelFormat {
    Nv12,
    P010,
    Yuv420p,
    Yuv422p,
    Yuv444p,
    Yuv420p10,
    Yuv422p10,
    Yuv444p10,
    Bgra,
};

/// YUV → RGB matrix of a frame (ignored for Bgra).
enum class ColorMatrix {
    Bt601,
    Bt709,
    Bt2020,
};

/// Value range of a frame's YUV samples: Limited = 16-235 (8-bit, scaled for 10-bit),
/// Full = 0-255. Ignored for Bgra.
enum class ColorRange {
    Limited,
    Full,
};

/// Transfer function. The timeline is SDR BT.709: PQ / HLG frames (and BT.2020 primaries) are
/// mapped to it when shown or rendered (media/HdrToSdr.h).
enum class ColorTransfer {
    Sdr,
    Pq,
    Hlg,
};

/// A decoded video frame: plane pointers into reference-counted pixel memory, ready to be
/// uploaded as textures (one texture per plane; YUV → RGB in a shader). Frames are immutable
/// and shared as `std::shared_ptr<const VideoFrame>`; the pixel memory stays valid as long as
/// the frame object lives (it may outlive the decoder) and is recycled into a pool afterwards.
///
/// The picture is stored as coded: apply `sampleAspectRatio` and then `rotation` (clockwise)
/// when displaying.
///
/// Thread-safety: immutable; can be read from any thread.
struct VideoFrame
{
    PixelFormat format = PixelFormat::Yuv420p;
    int width = 0; ///< luma width in pixels (coded picture, before SAR/rotation)
    int height = 0; ///< luma height in pixels
    int planeCount = 0;
    std::array<const uint8_t*, 4> data {}; ///< plane pointers (see PixelFormat)
    std::array<int, 4> linesize {}; ///< bytes per row of each plane (64-byte aligned)
    ColorMatrix matrix = ColorMatrix::Bt709;
    ColorRange range = ColorRange::Limited;
    ColorTransfer transfer = ColorTransfer::Sdr;
    int64_t ptsUs = 0; ///< media time at which this frame starts (µs)
    int64_t durationUs = 0; ///< nominal display duration (µs); 0 if unknown
    Rational sampleAspectRatio {1, 1};
    int rotation = 0; ///< display rotation, degrees clockwise (0/90/180/270)

    /// The FFmpeg frame that owns the planes, or nullptr when the frame wraps other memory
    /// (still images). Useful for media-internal conversions and encoders; never modify it.
    const AVFrame* avFrame = nullptr;
    /// Keeps the pixel memory alive (an AVFrame reference or a QImage).
    std::shared_ptr<const void> storage;

    /// Width/height of plane `plane` in samples (chroma planes are subsampled).
    int planeWidth(int plane) const;
    int planeHeight(int plane) const;
    /// Bytes per texel of plane `plane`: 1 (8-bit), 2 (10-bit planar, P010 Y, NV12 UV pair),
    /// 4 (P010 UV pair, Bgra). Use it to pick the texture format of each plane.
    int bytesPerPixel(int plane) const;
};

/// Short name of a pixel format ("nv12", "yuv420p10", ...), e.g. for logs and shader lookup.
const char* pixelFormatName(PixelFormat format);

} // namespace kk::media
