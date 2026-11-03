// SPDX-License-Identifier: GPL-3.0-or-later
// Scopes — video scope data of a picture (Color page, Edit page workspace window): waveform
// (luma or RGB overlaid), RGB parade, vectorscope and histogram, computed on the CPU from a
// small (downscaled) frame. Pure functions of an image; the widgets in src/ui/scopes draw
// them. Values are the gamma-encoded Rec. 709 RGB of the rendered frame ("premultiplied over
// black", as delivered), luma with the Rec. 709 weights.
#pragma once

#include <QImage>
#include <QPointF>

#include <array>
#include <cstdint>
#include <vector>

namespace kk::scopes {

/// Which scope a view shows.
enum class ScopeType : quint8 {
    WaveformLuma, ///< luma level per column
    WaveformRgb, ///< R, G and B levels per column, overlaid
    Parade, ///< R, G and B waveforms side by side
    Vectorscope, ///< Cb / Cr scatter (colour hue and saturation)
    Histogram ///< R, G, B and luma value counts
};

/// Stable string ids ("waveform", "waveformRgb", "parade", "vectorscope", "histogram").
[[nodiscard]] const char* scopeTypeId(ScopeType type);
[[nodiscard]] ScopeType scopeTypeFromId(const QString& id);

/// Waveform counts: for each channel plane (1 for luma, 3 for RGB: R, G, B) a grid of
/// `columns` x `levels` counters, row-major by level (level 0 = black). Pixel column x of an
/// image of width W lands in column x * columns / W; value v (0..1) in level
/// round(v * (levels - 1)).
struct WaveformData
{
    int columns = 0;
    int levels = 0;
    std::vector<std::vector<uint32_t>> planes;

    /// Count of plane @p plane at (@p column, @p level).
    [[nodiscard]] uint32_t at(int plane, int column, int level) const
    {
        return planes[size_t(plane)][size_t(level) * size_t(columns) + size_t(column)];
    }
};

/// Waveform of @p image: luma only (@p rgb false) or the three channels. @p columns <= 0 uses
/// the image width.
[[nodiscard]] WaveformData waveform(
    const QImage& image, bool rgb, int columns = 0, int levels = 256);

/// Vectorscope counts on a size x size grid, row-major, row 0 at the top: Cb to the right,
/// Cr up, the centre is neutral grey, the edge of the grid is |Cb| or |Cr| = 0.5.
struct VectorscopeData
{
    int size = 0;
    std::vector<uint32_t> counts;
    [[nodiscard]] uint32_t at(int x, int y) const
    {
        return counts[size_t(y) * size_t(size) + size_t(x)];
    }
};

/// Cb / Cr (Rec. 709, -0.5..0.5) of gamma-encoded RGB (0..1).
[[nodiscard]] QPointF chroma(double r, double g, double b);
/// Grid cell (fractional, 0..size - 1) of chroma @p cbCr on a vectorscope of @p size.
[[nodiscard]] QPointF vectorscopeCell(QPointF cbCr, int size);
/// Vectorscope of @p image on a @p size grid.
[[nodiscard]] VectorscopeData vectorscope(const QImage& image, int size = 256);

/// Histogram: `bins` counters per channel (R, G, B, luma), value v in bin round(v (bins - 1)).
struct HistogramData
{
    int bins = 0;
    std::array<std::vector<uint32_t>, 4> channels; ///< R, G, B, luma
};
[[nodiscard]] HistogramData histogram(const QImage& image, int bins = 256);

/// The picture a scope is computed from: @p image reduced to at most @p maxWidth pixels wide
/// (aspect kept, smooth), ARGB32_Premultiplied. Scopes of a 1080p frame from a 480 wide copy
/// cost about a millisecond.
[[nodiscard]] QImage scopeInput(const QImage& image, int maxWidth = 480);

} // namespace kk::scopes
