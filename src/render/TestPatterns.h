// SPDX-License-Identifier: GPL-3.0-or-later
// TestPatterns — the numbers and formulas behind the 1.2 generators that are test patterns or
// procedural: EBU colour bars, the ITU-R BT.2111 HDR colour bars, the YCbCr ramp, the noise
// gradient and the grid. The CPU raster (Generators.cpp) calls these directly; the GLSL in
// GlGenerators.cpp repeats the same arithmetic (tests/render/test_generators.cpp compares).
#pragma once

#include <QRgb>

#include <array>
#include <cstdint>

namespace kk::render::testpattern {

struct Rgbf
{
    float r = 0, g = 0, b = 0;
};

// ---- EBU colour bars ----

/// Straight 8-bit RGB of the EBU colour bars at normalised horizontal position @p u (0..1).
/// Eight bars: white, yellow, cyan, green, magenta, red, blue, black. EBU Tech 3213 / ITU-R
/// BT.471 "100/0/75/0" set: the white bar is 100 %, the colour bars are 75 % amplitude
/// (191 of 255 in full range), black is 0 %.
[[nodiscard]] QRgb ebuBarsAt(double u);

// ---- ITU-R BT.2111 colour bars ----

/// The three patterns of the Recommendation (its Tables 2, 3 and 4).
enum class Bt2111 : int {
    HlgNarrow = 0, ///< Table 2, Figure 1
    PqNarrow = 1, ///< Table 3, Figure 2
    PqFull = 2 ///< Table 4, Figure 3
};

/// Number of entries of the patch palette (indices documented in TestPatterns.cpp).
inline constexpr int kBt2111Patches = 39;

/// 10-bit code values (R', G', B') of palette entry @p patch, exactly as printed in the
/// Recommendation (BT.2111-3).
[[nodiscard]] std::array<int, 3> bt2111Code(Bt2111 variant, int patch);

/// Signal level 0..1 of a code value: narrow range maps 64..940 to 0..1 (the -7 % and 109 %
/// levels clip to 0 and 1, as a full-range RGB canvas cannot carry them), full range maps
/// 0..1023 to 0..1.
[[nodiscard]] float bt2111Level(Bt2111 variant, int code);

/// The palette as signal levels (what the renderers draw).
[[nodiscard]] const std::array<Rgbf, kBt2111Patches>& bt2111Palette(Bt2111 variant);

/// Palette index of the 1920 x 1080 reference pixel (@p ix, @p iy), or -1 inside the ramp.
[[nodiscard]] int bt2111PatchAt(int ix, int iy);

/// 10-bit code value of the ramp at reference column @p ix (BT.2111 Tables 5 and 6).
[[nodiscard]] int bt2111RampCode(Bt2111 variant, int ix);

/// Colour at normalised position (@p u, @p v), 0..1 from the top left.
[[nodiscard]] Rgbf bt2111At(Bt2111 variant, double u, double v);

// ---- YCbCr ramp ----

/// RGB of the Rec. 709 (full range) colour with Y' = @p y and Cb, Cr = @p cb, @p cr (-0.5..0.5),
/// clipped to 0..1. Coefficients from Kr = 0.2126, Kb = 0.0722 (ITU-R BT.709).
[[nodiscard]] Rgbf ycbcrToRgb(float y, float cb, float cr);

/// Colour of ramp band @p band (0 = Y', 1 = Cb, 2 = Cr) at ramp position @p t (0..1): Y' goes
/// 0..1 grey; Cb and Cr go -0.5..+0.5 with Y' = 0.5 and the other component 0.
[[nodiscard]] Rgbf ycbcrRampAt(int band, float t);

// ---- Noise gradient ----

/// Fractal value noise 0..1 at reference position (@p x, @p y) px: @p octaves layers of
/// 3D value noise (z = @p evolution) of cell size @p scalePx, each octave twice as fine and
/// half as strong. Integer hash identical to the shader's.
[[nodiscard]] float noiseGradientValue(
    float x, float y, float scalePx, int octaves, float evolution, uint32_t seed);

/// GLSL twin of noiseGradientValue(), shared by the Noise Gradient generator and the Fast
/// Noise effect: noiseHash(), lattice(), valueNoise3() and
/// `float fractalNoise(vec2 p, float scalePx, int octaves, float evolution, uint seed)`.
inline constexpr char kGlslNoise[] = R"(
uint noiseHash(uint x, uint y, uint seed)
{
    uint h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
float lattice(int ix, int iy, int iz, uint seed)
{
    uint s = seed ^ (uint(iz) * 0x9e3779b9u);
    return float(noiseHash(uint(ix), uint(iy), s) & 0xffffu) / 65535.0;
}
float valueNoise3(vec3 q, uint seed)
{
    vec3 f = floor(q);
    ivec3 i = ivec3(f);
    vec3 t = q - f;
    t = t * t * (3.0 - 2.0 * t);
    float plane0 = mix(mix(lattice(i.x, i.y, i.z, seed), lattice(i.x + 1, i.y, i.z, seed), t.x),
                       mix(lattice(i.x, i.y + 1, i.z, seed), lattice(i.x + 1, i.y + 1, i.z, seed), t.x), t.y);
    float plane1 = mix(mix(lattice(i.x, i.y, i.z + 1, seed), lattice(i.x + 1, i.y, i.z + 1, seed), t.x),
                       mix(lattice(i.x, i.y + 1, i.z + 1, seed), lattice(i.x + 1, i.y + 1, i.z + 1, seed), t.x), t.y);
    return mix(plane0, plane1, t.z);
}
float fractalNoise(vec2 p, float scalePx, int octaves, float evolution, uint seed)
{
    float sum = 0.0, norm = 0.0, amp = 1.0, freq = 1.0;
    for (int o = 0; o < octaves; ++o) {
        float off = 37.0 * float(o);
        sum += amp * valueNoise3(vec3(p.x / scalePx * freq + off, p.y / scalePx * freq + off, evolution * freq + off), seed);
        norm += amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return sum / norm;
}
)";

// ---- Grid ----

/// Coverage 0..1 of the grid lines at pixel (@p x, @p y): lines every @p cellW / @p cellH px
/// (columns / rows of the frame), @p lineW wide, every @p majorEvery-th line @p majorW wide
/// (0 = no major lines).
[[nodiscard]] float gridCoverage(
    float x, float y, float cellW, float cellH, float lineW, int majorEvery, float majorW);

} // namespace kk::render::testpattern
