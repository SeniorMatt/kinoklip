// SPDX-License-Identifier: GPL-3.0-or-later
// PointTracker — follows one image point from frame to frame: normalised cross-correlation
// template matching, coarse to fine over an image pyramid, then a Gauss-Newton refinement
// for sub-pixel accuracy. Standard textbook methods (template matching and Lucas-Kanade style
// translation estimation); written for Kinoklip, no code taken from other projects.
// Pure maths on grey images: no Qt widgets, no threads, no project types.
#pragma once

#include <QImage>

#include <vector>

namespace kk::tracking {

/// A position in image pixels. Pixel (i, j) has its centre at exactly (i, j), so the top-left
/// pixel is at (0, 0) and the picture spans -0.5 .. width - 0.5.
struct Vec2
{
    double x = 0;
    double y = 0;
    friend bool operator==(const Vec2&, const Vec2&) = default;
};

/// A grey picture, values 0..1.
struct GrayImage
{
    int width = 0;
    int height = 0;
    std::vector<float> data; ///< width * height, row by row

    [[nodiscard]] bool isEmpty() const { return width <= 0 || height <= 0; }
    /// Pixel (x, y), which must lie inside the picture.
    [[nodiscard]] float at(int x, int y) const { return data[size_t(y) * size_t(width) + x]; }
    /// Bicubic (Catmull-Rom) value at (x, y); positions outside the picture read the nearest edge
    /// pixel.
    [[nodiscard]] float sample(double x, double y) const;
    /// Half-size picture (2x2 box filter); at least 1x1.
    [[nodiscard]] GrayImage halved() const;
};

/// Luma (Rec. 709 weights on the stored values) of @p image as a GrayImage. A null image gives
/// an empty one.
[[nodiscard]] GrayImage grayFromImage(const QImage& image);

/// How a point is followed.
struct TrackerSettings
{
    int patternSize = 31; ///< side of the square pattern box, pixels (>= 5)
    int searchSize = 101; ///< side of the square search box around the last position, pixels
    /// A match below this normalised correlation (-1..1) is a failure: tracking stops there.
    double minCorrelation = 0.6;
    /// A match below this (but above minCorrelation) re-takes the pattern from the current
    /// frame, so a slowly changing target stays followed. 1 = every frame, 0 = never.
    double refreshBelow = 0.85;
    int maxLevels = 4; ///< pyramid levels at most (the pattern is kept at least ~8 px wide)
};

/// Result of following the point into one more frame.
struct TrackResult
{
    bool ok = false; ///< false: the pattern was lost (or the point left the picture)
    Vec2 position; ///< the new position (the old one when !ok)
    double correlation = 0; ///< normalised correlation of the final match, -1..1
};

/// Follows one point through a sequence of frames, in either direction (the caller decides
/// the order of the frames). Create, reset() on the first frame, then track() each next frame.
class PointTracker
{
public:
    explicit PointTracker(TrackerSettings settings = {});

    /// Starts on @p frame with the point at @p position: the pattern is taken from there.
    /// False if the pattern box does not fit inside the picture at all.
    bool reset(const GrayImage& frame, Vec2 position);
    /// Finds the point in @p next, searching around the last position.
    TrackResult track(const GrayImage& next);

    [[nodiscard]] Vec2 position() const { return m_position; }
    [[nodiscard]] const TrackerSettings& settings() const { return m_settings; }

private:
    /// A pattern at one pyramid level.
    struct Patch
    {
        int size = 0; ///< side in pixels (odd)
        std::vector<float> values; ///< size * size, zero mean
        double norm = 0; ///< sqrt(sum of squares) of values
    };
    struct Level
    {
        GrayImage image;
        Patch patch;
    };

    [[nodiscard]] int levelCount(const GrayImage& image) const;
    void takePatches(std::vector<Level>& pyramid, Vec2 position);
    [[nodiscard]] static std::vector<Level> buildPyramid(const GrayImage& image, int levels);

    TrackerSettings m_settings;
    Vec2 m_position;
    int m_levels = 1;
    std::vector<Patch> m_patches; ///< the pattern at every level (0 = full size)
    bool m_ready = false;
};

} // namespace kk::tracking
