// SPDX-License-Identifier: GPL-3.0-or-later
// ShakyFootage — synthetic shaky camera footage for the stabilization tests: a large textured
// picture seen through a camera that moves by a known path (a slow pan plus random jitter, with
// rotation and zoom jitter if asked), so the measured movement can be compared with the truth.
// Optionally a bright object moves independently in front of the scene (it must not steer the
// measurement). Frames are written as a PNG sequence that the editor opens as a video clip.
#pragma once

#include <QDir>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QTransform>

#include <cmath>
#include <cstdint>
#include <vector>

namespace kk::test {

/// Where the first frame's picture is in frame t: scaled by `zoom`, turned by `degrees`
/// (clockwise on screen), then moved by (`tx`, `ty`) pixels, all about the frame centre.
struct CameraPose
{
    double tx = 0;
    double ty = 0;
    double degrees = 0;
    double zoom = 1;
};

struct ShakeSettings
{
    int frames = 60;
    double panPerFrameX = 0.0; ///< slow drift, pixels per frame
    double panPerFrameY = 0.0;
    double jitterPx = 0.0; ///< each frame moved by up to this much in x and y (uniform)
    double jitterDegrees = 0.0;
    double jitterZoom = 0.0; ///< each frame scaled by 1 +- this much
    uint32_t seed = 7;
};

/// Deterministic random numbers in -1..1.
struct Lcg
{
    uint32_t state = 1;
    explicit Lcg(uint32_t seed = 1)
        : state(seed * 2654435761u + 12345u)
    { }
    double next()
    {
        state = state * 1664525u + 1013904223u;
        return double(state >> 8) / double(1 << 23) - 1.0;
    }
};

/// Detailed texture (several scales of smooth noise), @p width x @p height, grey values.
inline QImage texturedPicture(int width, int height, uint32_t seed = 3)
{
    QImage out(width, height, QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    Lcg rng(seed);
    for (const int cells : {6, 12, 24, 48, 96}) {
        const int w = cells, h = std::max(2, cells * height / width);
        QImage layer(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int v = int(std::lround(12 + 12 * (rng.next() + 1)));
                layer.setPixel(x, y, qRgb(v, v, v));
            }
        }
        painter.drawImage(QRect(0, 0, width, height), layer);
    }
    painter.end();
    return out;
}

/// The camera path: pose of every frame (frame 0 is the identity pose).
inline std::vector<CameraPose> shakyPath(const ShakeSettings& s)
{
    std::vector<CameraPose> poses(size_t(s.frames));
    Lcg rng(s.seed);
    for (int t = 1; t < s.frames; ++t) {
        CameraPose& p = poses[size_t(t)];
        p.tx = s.panPerFrameX * t + s.jitterPx * rng.next();
        p.ty = s.panPerFrameY * t + s.jitterPx * rng.next();
        p.degrees = s.jitterDegrees * rng.next();
        p.zoom = 1.0 + s.jitterZoom * rng.next();
    }
    return poses;
}

/// The picture @p big seen by a camera with pose @p pose, as a @p width x @p height frame. @p big
/// must be larger than the frame by more than the movement. @p objectIndex >= 0 draws a bright
/// rectangle (about a twelfth of the frame) that moves across the scene independently.
inline QImage shakyFrame(
    const QImage& big, int width, int height, const CameraPose& pose, int objectIndex = -1)
{
    QImage frame(width, height, QImage::Format_RGB32);
    frame.fill(Qt::black);
    QPainter painter(&frame);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.translate(width / 2.0 + pose.tx, height / 2.0 + pose.ty);
    painter.rotate(pose.degrees + 0.01);
    // The nudge keeps Qt from rounding a pure shift to whole pixels.
    painter.scale(pose.zoom * 1.000001, pose.zoom * 1.000001);
    painter.translate(-big.width() / 2.0, -big.height() / 2.0);
    painter.drawImage(0, 0, big);
    painter.resetTransform();
    if (objectIndex >= 0) {
        const double u = std::fmod(objectIndex * 0.031, 1.0);
        painter.fillRect(
            QRectF(width * (0.1 + 0.7 * u), height * (0.15 + 0.3 * u), width * 0.18, height * 0.25),
            QColor(250, 250, 250));
    }
    return frame;
}

/// Writes @p frames as dir/frame_0000.png ... and returns the sequence path the media layer
/// understands ("dir/frame_%04d.png#fps=N").
inline QString writeSequence(const QString& dir, const std::vector<QImage>& frames, int fps = 25)
{
    QDir().mkpath(dir);
    for (size_t i = 0; i < frames.size(); ++i)
        frames[i].save(
            QDir(dir).filePath(QStringLiteral("frame_%1.png").arg(i, 4, 10, QLatin1Char('0'))));
    return QDir(dir).filePath(QStringLiteral("frame_%04d.png#fps=%1").arg(fps));
}

} // namespace kk::test
