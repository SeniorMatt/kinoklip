// SPDX-License-Identifier: GPL-3.0-or-later
// PointTracker implementation. See the header for the method.
#include "compositing/tracking/PointTracker.h"

#include <algorithm>
#include <cmath>

namespace kk::tracking {

namespace {

constexpr int kMinPatternAtTop = 9; ///< the coarsest level keeps a pattern at least this wide
constexpr int kRefineRadius = 2; ///< integer search around the scaled-up coarse result
constexpr int kMaxIterations = 25;

int oddAtLeast(int v, int minimum)
{
    v = std::max(v, minimum);
    return v | 1;
}

/// Pattern values at @p position of @p image (bicubic, centred on the position), zero mean.
/// Returns the sum-of-squares root; the patch values are written to @p values.
double extractPatch(const GrayImage& image, Vec2 position, int size, std::vector<float>& values)
{
    values.assign(size_t(size) * size_t(size), 0.0f);
    const double half = (size - 1) / 2.0;
    double mean = 0;
    for (int j = 0; j < size; ++j) {
        for (int i = 0; i < size; ++i) {
            const float v = image.sample(position.x + i - half, position.y + j - half);
            values[size_t(j) * size_t(size) + size_t(i)] = v;
            mean += v;
        }
    }
    mean /= double(values.size());
    double sq = 0;
    for (float& v : values) {
        v = float(v - mean);
        sq += double(v) * double(v);
    }
    return std::sqrt(sq);
}

/// Normalised correlation of @p values (zero mean, norm @p norm, side @p size) with the
/// window of @p image whose centre pixel is (cx, cy); the window lies inside the picture.
double correlationAt(
    const GrayImage& image, int cx, int cy, const std::vector<float>& values, int size, double norm)
{
    const int half = size / 2;
    double sum = 0, sumSq = 0, dot = 0;
    for (int j = 0; j < size; ++j) {
        const float* row
            = image.data.data() + size_t(cy - half + j) * size_t(image.width) + (cx - half);
        const float* t = values.data() + size_t(j) * size_t(size);
        for (int i = 0; i < size; ++i) {
            const double v = row[i];
            sum += v;
            sumSq += v * v;
            dot += v * t[i]; // t has zero mean, so the window mean drops out of the dot product
        }
    }
    const double n = double(size) * size;
    const double var = sumSq - sum * sum / n;
    if (var < 1e-9 || norm < 1e-9)
        return 0.0;
    return dot / (std::sqrt(var) * norm);
}

/// Normalised correlation at a real position (bicubic samples).
double correlationReal(
    const GrayImage& image, Vec2 p, const std::vector<float>& values, int size, double norm)
{
    const double half = (size - 1) / 2.0;
    double sum = 0, sumSq = 0, dot = 0;
    for (int j = 0; j < size; ++j) {
        for (int i = 0; i < size; ++i) {
            const double v = image.sample(p.x + i - half, p.y + j - half);
            sum += v;
            sumSq += v * v;
            dot += v * values[size_t(j) * size_t(size) + size_t(i)];
        }
    }
    const double n = double(size) * size;
    const double var = sumSq - sum * sum / n;
    if (var < 1e-9 || norm < 1e-9)
        return 0.0;
    return dot / (std::sqrt(var) * norm);
}

/// Best integer position in the square of centres [cx - r, cx + r] x [cy - r, cy + r] whose
/// whole window lies inside the picture. Returns false when no candidate fits.
bool bestInteger(const GrayImage& image, int cx, int cy, int radius,
    const std::vector<float>& values, int size, double norm, int* bx, int* by, double* best)
{
    const int half = size / 2;
    const int x0 = std::max(cx - radius, half), x1 = std::min(cx + radius, image.width - 1 - half);
    const int y0 = std::max(cy - radius, half), y1 = std::min(cy + radius, image.height - 1 - half);
    if (x0 > x1 || y0 > y1)
        return false;
    *best = -2.0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const double c = correlationAt(image, x, y, values, size, norm);
            // Ties go to the candidate nearest the centre of the search (steadier on flat areas).
            if (c > *best + 1e-12) {
                *best = c;
                *bx = x;
                *by = y;
            }
        }
    }
    return true;
}

/// Gauss-Newton refinement of a real position: minimises the difference between the
/// normalised pattern and the normalised window. Keeps @p p when it does not converge to a
/// place within @p maxMove of the start.
Vec2 refine(const GrayImage& image, Vec2 start, const std::vector<float>& values, int size,
    double norm, double maxMove)
{
    Vec2 p = start;
    const double half = (size - 1) / 2.0;
    const size_t count = values.size();
    std::vector<float> window(count), gx(count), gy(count);
    for (int iter = 0; iter < kMaxIterations; ++iter) {
        double mean = 0;
        for (int j = 0; j < size; ++j) {
            for (int i = 0; i < size; ++i) {
                const double x = p.x + i - half, y = p.y + j - half;
                const size_t k = size_t(j) * size_t(size) + size_t(i);
                window[k] = image.sample(x, y);
                gx[k] = 0.5f * (image.sample(x + 1, y) - image.sample(x - 1, y));
                gy[k] = 0.5f * (image.sample(x, y + 1) - image.sample(x, y - 1));
                mean += window[k];
            }
        }
        mean /= double(count);
        double sq = 0;
        for (float& v : window) {
            v = float(v - mean);
            sq += double(v) * double(v);
        }
        const double wnorm = std::sqrt(sq);
        if (wnorm < 1e-6 || norm < 1e-9)
            return start;
        // residual r = window/|window| - pattern/|pattern|; Jacobian = gradient/|window|.
        double a = 0, b = 0, c = 0, bx = 0, by = 0;
        for (size_t k = 0; k < count; ++k) {
            const double r = window[k] / wnorm - values[k] / norm;
            const double jx = gx[k] / wnorm, jy = gy[k] / wnorm;
            a += jx * jx;
            b += jx * jy;
            c += jy * jy;
            bx += jx * r;
            by += jy * r;
        }
        const double det = a * c - b * b;
        if (det < 1e-18)
            break;
        const double dx = -(c * bx - b * by) / det;
        const double dy = -(a * by - b * bx) / det;
        p.x += dx;
        p.y += dy;
        if (std::hypot(p.x - start.x, p.y - start.y) > maxMove)
            return start;
        if (std::hypot(dx, dy) < 1e-4)
            break;
    }
    return p;
}

} // namespace

float GrayImage::sample(double x, double y) const
{
    if (isEmpty())
        return 0.0f;
    x = std::clamp(x, 0.0, double(width - 1));
    y = std::clamp(y, 0.0, double(height - 1));
    const int x0 = int(x), y0 = int(y);
    const float fx = float(x - x0), fy = float(y - y0);
    // Catmull-Rom (bicubic) interpolation: bilinear smooths the picture and biases sub-pixel
    // estimates by up to a tenth of a pixel.
    auto weights = [](float t, float w[4]) {
        w[0] = 0.5f * (-t * t * t + 2 * t * t - t);
        w[1] = 0.5f * (3 * t * t * t - 5 * t * t + 2);
        w[2] = 0.5f * (-3 * t * t * t + 4 * t * t + t);
        w[3] = 0.5f * (t * t * t - t * t);
    };
    float wx[4], wy[4];
    weights(fx, wx);
    weights(fy, wy);
    float sum = 0;
    for (int j = 0; j < 4; ++j) {
        const int yy = std::clamp(y0 - 1 + j, 0, height - 1);
        float row = 0;
        for (int i = 0; i < 4; ++i)
            row += wx[i] * at(std::clamp(x0 - 1 + i, 0, width - 1), yy);
        sum += wy[j] * row;
    }
    return sum;
}

GrayImage GrayImage::halved() const
{
    GrayImage out;
    out.width = std::max(1, width / 2);
    out.height = std::max(1, height / 2);
    out.data.resize(size_t(out.width) * size_t(out.height));
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const int sx = std::min(2 * x, width - 1), sy = std::min(2 * y, height - 1);
            const int sx1 = std::min(sx + 1, width - 1), sy1 = std::min(sy + 1, height - 1);
            out.data[size_t(y) * size_t(out.width) + size_t(x)]
                = 0.25f * (at(sx, sy) + at(sx1, sy) + at(sx, sy1) + at(sx1, sy1));
        }
    }
    return out;
}

GrayImage grayFromImage(const QImage& image)
{
    GrayImage out;
    if (image.isNull())
        return out;
    const QImage rgb = image.format() == QImage::Format_ARGB32_Premultiplied
        ? image
        : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    out.width = rgb.width();
    out.height = rgb.height();
    out.data.resize(size_t(out.width) * size_t(out.height));
    for (int y = 0; y < out.height; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        for (int x = 0; x < out.width; ++x) {
            const QRgb c = line[x];
            out.data[size_t(y) * size_t(out.width) + size_t(x)]
                = (0.2126f * qRed(c) + 0.7152f * qGreen(c) + 0.0722f * qBlue(c)) / 255.0f;
        }
    }
    return out;
}

PointTracker::PointTracker(TrackerSettings settings)
    : m_settings(settings)
{
    m_settings.patternSize = std::max(5, m_settings.patternSize);
    m_settings.searchSize = std::max(m_settings.patternSize, m_settings.searchSize);
}

int PointTracker::levelCount(const GrayImage& image) const
{
    int levels = 1;
    int pattern = m_settings.patternSize;
    int w = image.width, h = image.height;
    while (levels < m_settings.maxLevels && pattern / 2 >= kMinPatternAtTop && w >= 2 && h >= 2) {
        pattern /= 2;
        w /= 2;
        h /= 2;
        ++levels;
    }
    return levels;
}

std::vector<PointTracker::Level> PointTracker::buildPyramid(const GrayImage& image, int levels)
{
    std::vector<Level> pyramid;
    pyramid.resize(size_t(levels));
    pyramid[0].image = image;
    for (int l = 1; l < levels; ++l)
        pyramid[size_t(l)].image = pyramid[size_t(l - 1)].image.halved();
    return pyramid;
}

void PointTracker::takePatches(std::vector<Level>& pyramid, Vec2 position)
{
    m_patches.assign(pyramid.size(), Patch {});
    for (size_t l = 0; l < pyramid.size(); ++l) {
        const double scale = 1.0 / double(1 << l);
        Patch& patch = m_patches[l];
        patch.size = oddAtLeast(m_settings.patternSize >> l, 5);
        patch.norm = extractPatch(
            pyramid[l].image, {position.x * scale, position.y * scale}, patch.size, patch.values);
    }
}

bool PointTracker::reset(const GrayImage& frame, Vec2 position)
{
    m_ready = false;
    if (frame.isEmpty())
        return false;
    m_position = position;
    m_levels = levelCount(frame);
    std::vector<Level> pyramid = buildPyramid(frame, m_levels);
    takePatches(pyramid, position);
    m_ready = m_patches.front().norm > 1e-6; // a flat pattern has nothing to follow
    return m_ready;
}

TrackResult PointTracker::track(const GrayImage& next)
{
    TrackResult result;
    result.position = m_position;
    if (!m_ready || next.isEmpty())
        return result;
    std::vector<Level> pyramid = buildPyramid(next, m_levels);

    // Coarsest level: exhaustive search of the (scaled) search box around the last position.
    const int top = m_levels - 1;
    const double topScale = 1.0 / double(1 << top);
    const Patch& topPatch = m_patches[size_t(top)];
    int bx = int(std::lround(m_position.x * topScale)),
        by = int(std::lround(m_position.y * topScale));
    double best = -2.0;
    const int radius = std::max(1, int(std::ceil((m_settings.searchSize - 1) / 2.0 * topScale)));
    if (!bestInteger(pyramid[size_t(top)].image, bx, by, radius, topPatch.values, topPatch.size,
            topPatch.norm, &bx, &by, &best))
        return result;
    // Finer levels: scale up and look a few pixels around.
    for (int l = top - 1; l >= 0; --l) {
        const Patch& patch = m_patches[size_t(l)];
        int nx = bx * 2, ny = by * 2;
        double c = -2.0;
        if (!bestInteger(pyramid[size_t(l)].image, nx, ny, kRefineRadius, patch.values, patch.size,
                patch.norm, &nx, &ny, &c))
            return result;
        bx = nx;
        by = ny;
        best = c;
    }
    const Patch& base = m_patches.front();
    const GrayImage& image = pyramid.front().image;
    Vec2 p = refine(image, {double(bx), double(by)}, base.values, base.size, base.norm, 1.5);
    double corr = correlationReal(image, p, base.values, base.size, base.norm);
    if (corr < best) { // refinement never makes the match worse
        p = {double(bx), double(by)};
        corr = best;
    }
    result.correlation = corr;
    if (corr < m_settings.minCorrelation)
        return result;
    result.ok = true;
    result.position = p;
    m_position = p;
    if (corr < m_settings.refreshBelow)
        takePatches(pyramid, p);
    return result;
}

} // namespace kk::tracking
