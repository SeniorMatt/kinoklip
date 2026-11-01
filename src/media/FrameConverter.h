// SPDX-License-Identifier: GPL-3.0-or-later
// FrameConverter — internal: turns decoded AVFrames into scaled QImages using libswscale.
#pragma once

#include "media/FFmpegUtil.h"

#include <QImage>
#include <QSize>

namespace kk::media::detail {

/// Converts decoded video frames (any pixel format) into `QImage::Format_ARGB32_Premultiplied`
/// images of a requested size, honouring the frame's colour space (BT.601/709/2020) and range.
/// Alpha is preserved for formats that carry it; opaque formats get alpha = 255.
///
/// Keeps one cached SwsContext, so converting a stream of same-sized frames is cheap.
/// Thread-safety: not thread-safe; owned by one VideoDecoder.
class FrameConverter
{
public:
    /// Converts `frame` to an image of exactly `outSize` pixels (no rotation is applied).
    /// @return the image, or a null QImage if the frame cannot be converted.
    QImage convert(const AVFrame* frame, QSize outSize);

private:
    SwsContextPtr m_sws;
    // Parameters m_sws was configured with (to know when the colour details must be reset).
    int m_srcWidth = 0;
    int m_srcHeight = 0;
    int m_srcFormat = -1;
    QSize m_dstSize;
    int m_colorspace = -1;
    bool m_fullRange = false;
};

} // namespace kk::media::detail
