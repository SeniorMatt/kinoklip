// SPDX-License-Identifier: GPL-3.0-or-later
// Generators — the built-in generator clips (gradients, colour bars, grey scale, noise,
// countdown, timecode burn-in): parameter resolution shared by the GPU and CPU renderers, the
// CPU raster used by the QPainter fallback, and the text part (as a TitleStyle) both draw.
#pragma once

#include "core/TimelineData.h"
#include "render/TitleRenderer.h"

#include <QColor>
#include <QImage>
#include <QSize>
#include <QSizeF>
#include <QString>

#include <array>
#include <cstdint>

namespace kk::render {

/// Countdown geometry, as fractions of the frame height (shared with the GLSL pass).
inline constexpr double kCountdownRadius = 0.36;
inline constexpr double kCountdownRing = 0.012;
inline constexpr double kCountdownNumberSize = 0.42;

/// Everything a generator draws at one frame, resolved from ClipData::generator /
/// ClipData::title (pure value type; no GL).
struct GeneratorFrame
{
    enum class Kind {
        Unknown, ///< unknown id: draws nothing
        FourColor,
        Linear,
        Radial,
        Bars,
        GreyScale,
        Noise,
        Countdown,
        Timecode
    };
    Kind kind = Kind::Unknown;
    /// Premultiplied RGBA colours: gradients use [0..1] (four-colour: TL, TR, BL, BR);
    /// countdown: [0] background, [1] sweep, [2] lines.
    std::array<std::array<float, 4>, 4> colors {};
    double centerX = 0; ///< timeline px from the frame centre
    double centerY = 0;
    double angleDeg = 0; ///< linear gradient direction, clockwise, 0 = left → right
    double length = 1920; ///< linear gradient length (px)
    double radius = 800; ///< radial gradient radius (px)
    bool fullBars = false; ///< colour bars: full-field 100 % bars instead of SMPTE-style
    int steps = 11; ///< grey scale steps (< 2 = smooth ramp)
    bool vertical = false; ///< grey scale orientation
    bool monochrome = true; ///< noise
    double brightness = 0.5; ///< noise mean level 0..1
    double amount = 1.0; ///< noise strength 0..1
    double grain = 1.0; ///< noise cell size (timeline px, >= 1)
    uint32_t seed = 1; ///< noise seed, already mixed with the frame for animated noise
    double sweep = 0; ///< countdown: fraction of the current second (0..1)
    /// Text drawn on top (countdown number, timecode); empty = none. `textStyle.canvas` and
    /// `scale` are filled by the renderer for its target.
    TitleStyle textStyle;
    double textOffsetY = 0; ///< fraction of the frame height the text sits below the centre

    /// True if the kind has a per-pixel pattern (everything but the timecode text).
    [[nodiscard]] bool hasPattern() const
    {
        return kind != Kind::Unknown && kind != Kind::Timecode;
    }
};

/// Resolves generator clip @p clip at clip-local frame @p localFrame (@p timelineFrame: the
/// record frame, i.e. the timeline frame plus its starting timecode, of a timeline at @p fps;
/// what a timeline timecode burn-in shows). Allocation-free except for the text of the
/// countdown / timecode generators.
[[nodiscard]] GeneratorFrame resolveGenerator(
    const ClipData& clip, int64_t localFrame, int64_t timelineFrame, Rational fps);

/// The 32-bit integer hash behind the noise generator (identical in the GLSL shader).
[[nodiscard]] constexpr uint32_t noiseHash(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

/// Straight 8-bit RGB of the colour bars at normalised position (@p u, @p v) (0..1, top-left
/// origin). SMPTE-style layout (75 % bars, reverse-blue band, -I / white / +Q / black / PLUGE
/// row) or full-field 100 % bars. Shared with the GLSL implementation (same table).
[[nodiscard]] QRgb colorBarsAt(double u, double v, bool fullField);

/// "HH:MM:SS:FF" of @p frames at @p fps (whole-frame timecode base, non-drop-frame).
[[nodiscard]] QString generatorTimecode(int64_t frames, Rational fps);

/// Countdown number font size (timeline px) for a frame @p frameHeight timeline pixels high.
[[nodiscard]] double countdownNumberSize(double frameHeight);

/// CPU raster of @p g for a canvas of @p canvas output pixels showing a frame of
/// @p frameSize timeline pixels (ARGB32_Premultiplied, transparent where nothing is drawn).
/// Used by the QPainter fallback and tests; the GPU draws the same picture with shaders.
[[nodiscard]] QImage renderGeneratorImage(GeneratorFrame g, QSize canvas, QSizeF frameSize);

} // namespace kk::render
