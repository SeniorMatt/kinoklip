// SPDX-License-Identifier: GPL-3.0-or-later
// Scopes — waveform, parade, vectorscope and histogram counts of an image.
#include "render/Scopes.h"

#include "core/GradeMath.h"

#include <QString>

#include <algorithm>
#include <cmath>

namespace kk::scopes {

namespace {

/// Rec. 709 chroma scale factors: Cb = (B - Y) / 1.8556, Cr = (R - Y) / 1.5748.
constexpr double kCbScale = 1.8556;
constexpr double kCrScale = 1.5748;

/// Visits every pixel of @p image (converted to ARGB32_Premultiplied) with its column and
/// RGB in 0..1 (premultiplied over black).
template<typename F>
void forEachPixel(const QImage& image, F&& fn)
{
    const QImage img = image.format() == QImage::Format_ARGB32_Premultiplied
        ? image
        : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < img.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = line[x];
            fn(x, qRed(p) / 255.0f, qGreen(p) / 255.0f, qBlue(p) / 255.0f);
        }
    }
}

int levelOf(float v, int levels)
{
    return std::clamp(int(std::lround(v * float(levels - 1))), 0, levels - 1);
}

} // namespace

const char* scopeTypeId(ScopeType type)
{
    switch (type) {
    case ScopeType::WaveformLuma:
        return "waveform";
    case ScopeType::WaveformRgb:
        return "waveformRgb";
    case ScopeType::Parade:
        return "parade";
    case ScopeType::Vectorscope:
        return "vectorscope";
    case ScopeType::Histogram:
        return "histogram";
    }
    return "waveform";
}

ScopeType scopeTypeFromId(const QString& id)
{
    for (const ScopeType t : {ScopeType::WaveformLuma, ScopeType::WaveformRgb, ScopeType::Parade,
             ScopeType::Vectorscope, ScopeType::Histogram}) {
        if (id == QLatin1String(scopeTypeId(t)))
            return t;
    }
    return ScopeType::WaveformLuma;
}

WaveformData waveform(const QImage& image, bool rgb, int columns, int levels)
{
    WaveformData d;
    if (image.isNull() || levels < 2)
        return d;
    d.columns = columns > 0 ? columns : image.width();
    d.levels = levels;
    d.planes.assign(rgb ? 3 : 1, std::vector<uint32_t>(size_t(d.columns) * size_t(levels), 0));
    const int w = image.width();
    forEachPixel(image, [&](int x, float r, float g, float b) {
        const size_t column = size_t(int(int64_t(x) * d.columns / w));
        if (rgb) {
            const float c[3] = {r, g, b};
            for (int ch = 0; ch < 3; ++ch)
                ++d.planes[size_t(ch)][size_t(levelOf(c[ch], levels)) * size_t(d.columns) + column];
        } else {
            const float c[3] = {r, g, b};
            ++d.planes[0][size_t(levelOf(grademath::luma(c), levels)) * size_t(d.columns) + column];
        }
    });
    return d;
}

QPointF chroma(double r, double g, double b)
{
    const double y = grademath::kLumaR * r + grademath::kLumaG * g + grademath::kLumaB * b;
    return {(b - y) / kCbScale, (r - y) / kCrScale};
}

QPointF vectorscopeCell(QPointF cbCr, int size)
{
    const double n = size - 1;
    return {(cbCr.x() + 0.5) * n, (0.5 - cbCr.y()) * n};
}

VectorscopeData vectorscope(const QImage& image, int size)
{
    VectorscopeData d;
    if (image.isNull() || size < 2)
        return d;
    d.size = size;
    d.counts.assign(size_t(size) * size_t(size), 0);
    forEachPixel(image, [&](int, float r, float g, float b) {
        const QPointF cell = vectorscopeCell(chroma(r, g, b), size);
        const int x = std::clamp(int(std::lround(cell.x())), 0, size - 1);
        const int y = std::clamp(int(std::lround(cell.y())), 0, size - 1);
        ++d.counts[size_t(y) * size_t(size) + size_t(x)];
    });
    return d;
}

HistogramData histogram(const QImage& image, int bins)
{
    HistogramData d;
    if (image.isNull() || bins < 2)
        return d;
    d.bins = bins;
    for (auto& c : d.channels)
        c.assign(size_t(bins), 0);
    forEachPixel(image, [&](int, float r, float g, float b) {
        const float c[3] = {r, g, b};
        ++d.channels[0][size_t(levelOf(r, bins))];
        ++d.channels[1][size_t(levelOf(g, bins))];
        ++d.channels[2][size_t(levelOf(b, bins))];
        ++d.channels[3][size_t(levelOf(grademath::luma(c), bins))];
    });
    return d;
}

QImage scopeInput(const QImage& image, int maxWidth)
{
    if (image.isNull())
        return {};
    QImage img = image.width() > maxWidth ? image.scaledToWidth(maxWidth, Qt::SmoothTransformation)
                                          : image;
    if (img.format() != QImage::Format_ARGB32_Premultiplied)
        img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return img;
}

} // namespace kk::scopes
