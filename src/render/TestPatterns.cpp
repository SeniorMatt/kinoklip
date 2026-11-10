// SPDX-License-Identifier: GPL-3.0-or-later
// TestPatterns — see TestPatterns.h. Every formula here has a GLSL twin in GlGenerators.cpp.
#include "render/TestPatterns.h"

#include "render/Generators.h" // noiseHash()

#include <algorithm>
#include <cmath>

namespace kk::render::testpattern {

namespace {

float clamp01(float v)
{
    return std::clamp(v, 0.f, 1.f);
}

// ---- BT.2111 patch palette ----
//
// Indices (shared with the shader through bt2111PatchAt()):
//   0        40 % grey (the side columns)
//   1..7     100 % white, yellow, cyan, green, magenta, red, blue (top strip)
//   8..14    75 % (HLG) / 58 % (PQ) white, yellow, cyan, green, magenta, red, blue (main bars)
//   15       unused
//   16..28   the stair: -7 % step (white bar), then twelve half-bar steps 0 % ... 109 %
//            (PQ full: 0 % step, then 0 % ... 100 %, 100 %)
//   29..32   black row: 0 %, -2 %, +2 %, +4 % black (PQ full: -2 % is 0 %)
//   33..38   75 % / 58 % BT.709 yellow, cyan, green (left), magenta, red, blue (right)
// Values: ITU-R BT.2111-3 Tables 2 (HLG narrow), 3 (PQ narrow) and 4 (PQ full), 10-bit.
using Code = std::array<int, 3>;
constexpr Code g(int v)
{
    return {v, v, v};
}

constexpr std::array<Code, kBt2111Patches> kHlgNarrow = {g(414), // 0
    g(940), {940, 940, 64}, {64, 940, 940}, {64, 940, 64}, {940, 64, 940}, {940, 64, 64},
    {64, 64, 940}, // 1..7
    g(721), {721, 721, 64}, {64, 721, 721}, {64, 721, 64}, {721, 64, 721}, {721, 64, 64},
    {64, 64, 721}, // 8..14
    g(0), // 15
    g(4), g(64), g(152), g(239), g(327), g(414), g(502), g(590), g(677), g(765), g(852), g(940),
    g(1019), // 16..28
    g(64), g(48), g(80), g(99), // 29..32
    {713, 719, 316}, {538, 709, 718}, {512, 706, 296}, {651, 286, 705}, {639, 269, 164},
    {227, 147, 702}}; // 33..38

constexpr std::array<Code, kBt2111Patches> kPqNarrow = {g(414), // 0
    g(940), {940, 940, 64}, {64, 940, 940}, {64, 940, 64}, {940, 64, 940}, {940, 64, 64},
    {64, 64, 940}, // 1..7
    g(573), {573, 573, 64}, {64, 573, 573}, {64, 573, 64}, {573, 64, 573}, {573, 64, 64},
    {64, 64, 573}, // 8..14
    g(0), // 15
    g(4), g(64), g(152), g(239), g(327), g(414), g(502), g(590), g(677), g(765), g(852), g(940),
    g(1019), // 16..28
    g(64), g(48), g(80), g(99), // 29..32
    {569, 572, 381}, {485, 566, 571}, {474, 565, 368}, {537, 362, 564}, {531, 351, 257},
    {318, 236, 563}}; // 33..38

constexpr std::array<Code, kBt2111Patches> kPqFull = {g(409), // 0
    g(1023), {1023, 1023, 0}, {0, 1023, 1023}, {0, 1023, 0}, {1023, 0, 1023}, {1023, 0, 0},
    {0, 0, 1023}, // 1..7
    g(594), {594, 594, 0}, {0, 594, 594}, {0, 594, 0}, {594, 0, 594}, {594, 0, 0},
    {0, 0, 594}, // 8..14
    g(0), // 15
    g(0), g(0), g(102), g(205), g(307), g(409), g(512), g(614), g(716), g(818), g(921), g(1023),
    g(1023), // 16..28
    g(0), g(0), g(19), g(41), // 29..32
    {589, 593, 370}, {491, 586, 592}, {479, 585, 355}, {552, 348, 584}, {545, 335, 225},
    {296, 201, 582}}; // 33..38

const std::array<Code, kBt2111Patches>& codes(Bt2111 v)
{
    switch (v) {
    case Bt2111::HlgNarrow:
        return kHlgNarrow;
    case Bt2111::PqNarrow:
        return kPqNarrow;
    case Bt2111::PqFull:
        break;
    }
    return kPqFull;
}

// Reference geometry, 1920 x 1080 (BT.2111 Table 1: a..k). Column edges of the seven bars
// (c = 240 side columns, d = 206, e = 204 for green) and of the twelve half-bar stair steps.
constexpr int kBarEdges[6] = {446, 652, 858, 1062, 1268, 1474};
constexpr int kStairEdges[11] = {549, 652, 755, 858, 960, 1062, 1165, 1268, 1371, 1474, 1577};
constexpr int kRowTopStrip = 90; // b / 12
constexpr int kRowBars = 630; // + b / 2
constexpr int kRowStair = 720; // + b / 12
constexpr int kRowRamp = 810; // + b / 12; the black row (b / 4) follows
constexpr int kSideLeft = 240;
constexpr int kSideRight = 1680;

} // namespace

QRgb ebuBarsAt(double u)
{
    static constexpr QRgb kBars[8]
        = {0xffffff, 0xbfbf00, 0x00bfbf, 0x00bf00, 0xbf00bf, 0xbf0000, 0x0000bf, 0x000000};
    u = std::clamp(u, 0.0, 0.999999);
    return 0xff000000u | kBars[int(u * 8)];
}

std::array<int, 3> bt2111Code(Bt2111 variant, int patch)
{
    return codes(variant)[size_t(std::clamp(patch, 0, kBt2111Patches - 1))];
}

float bt2111Level(Bt2111 variant, int code)
{
    if (variant == Bt2111::PqFull)
        return clamp01(float(code) / 1023.f);
    return clamp01(float(code - 64) / 876.f);
}

const std::array<Rgbf, kBt2111Patches>& bt2111Palette(Bt2111 variant)
{
    static const std::array<std::array<Rgbf, kBt2111Patches>, 3> all = [] {
        std::array<std::array<Rgbf, kBt2111Patches>, 3> r {};
        for (int v = 0; v < 3; ++v) {
            for (int i = 0; i < kBt2111Patches; ++i) {
                const Code c = codes(Bt2111(v))[size_t(i)];
                r[size_t(v)][size_t(i)] = {bt2111Level(Bt2111(v), c[0]),
                    bt2111Level(Bt2111(v), c[1]), bt2111Level(Bt2111(v), c[2])};
            }
        }
        return r;
    }();
    return all[size_t(variant)];
}

int bt2111PatchAt(int ix, int iy)
{
    const bool side = ix < kSideLeft || ix >= kSideRight;
    if (iy < kRowBars) {
        if (side)
            return 0;
        int bar = 0;
        for (const int e : kBarEdges)
            bar += ix >= e ? 1 : 0;
        return (iy < kRowTopStrip ? 1 : 8) + bar;
    }
    if (iy < kRowStair) {
        if (side)
            return 8; // 75 % / 58 % white
        if (ix < kBarEdges[0])
            return 16;
        int half = 0;
        for (const int e : kStairEdges)
            half += ix >= e ? 1 : 0;
        return 17 + half;
    }
    if (iy < kRowRamp)
        return ix < kSideLeft ? 29 : -1;
    // Black row: BT.709 bars (left), black / -2 / 0 / +2 / 0 / +4 / 0 % black (f g h g h g i),
    // 75 % / 58 % white (j), black (k), BT.709 bars (right); widths c/3, f, g, h, i, j, k.
    if (ix < 80)
        return 33;
    if (ix < 160)
        return 34;
    if (ix < 240)
        return 35;
    if (ix < 376)
        return 29;
    if (ix < 446)
        return 30;
    if (ix < 514)
        return 29;
    if (ix < 584)
        return 31;
    if (ix < 652)
        return 29;
    if (ix < 722)
        return 32;
    if (ix < 960)
        return 29;
    if (ix < 1398)
        return 8;
    if (ix < kSideRight)
        return 29;
    if (ix < 1760)
        return 36;
    if (ix < 1840)
        return 37;
    return 38;
}

int bt2111RampCode(Bt2111 variant, int ix)
{
    // Table 5 (narrow): B = 559 px at code 4, C = 1014 px rising 5..1018, D = 107 px at 1019.
    // Table 6 (PQ full): B = 618 px at 0, C = 1022 px rising 1..1022, D = 40 px at 1023. The
    // ramp starts at the left edge of the first bar (x = 240); 0 % lands on the green bar edge.
    const int x = ix - kSideLeft;
    if (variant == Bt2111::PqFull) {
        if (x < 618)
            return 0;
        return x < 618 + 1022 ? 1 + (x - 618) : 1023;
    }
    if (x < 559)
        return 4;
    return x < 559 + 1014 ? 5 + (x - 559) : 1019;
}

Rgbf bt2111At(Bt2111 variant, double u, double v)
{
    const int ix = std::clamp(int(std::floor(u * 1920.0)), 0, 1919);
    const int iy = std::clamp(int(std::floor(v * 1080.0)), 0, 1079);
    const int patch = bt2111PatchAt(ix, iy);
    if (patch >= 0)
        return bt2111Palette(variant)[size_t(patch)];
    const float l = bt2111Level(variant, bt2111RampCode(variant, ix));
    return {l, l, l};
}

Rgbf ycbcrToRgb(float y, float cb, float cr)
{
    // R = Y + 2(1 - Kr) Cr, B = Y + 2(1 - Kb) Cb, G from Y = Kr R + Kg G + Kb B.
    constexpr float kCrToR = 1.5748f;
    constexpr float kCbToG = -0.187324f;
    constexpr float kCrToG = -0.468124f;
    constexpr float kCbToB = 1.8556f;
    return {
        clamp01(y + kCrToR * cr), clamp01(y + kCbToG * cb + kCrToG * cr), clamp01(y + kCbToB * cb)};
}

Rgbf ycbcrRampAt(int band, float t)
{
    t = clamp01(t);
    if (band == 0)
        return ycbcrToRgb(t, 0.f, 0.f);
    if (band == 1)
        return ycbcrToRgb(0.5f, t - 0.5f, 0.f);
    return ycbcrToRgb(0.5f, 0.f, t - 0.5f);
}

namespace {

float lattice(int ix, int iy, int iz, uint32_t seed)
{
    const uint32_t s = seed ^ (uint32_t(iz) * 0x9e3779b9u);
    return float(noiseHash(uint32_t(ix), uint32_t(iy), s) & 0xffffu) / 65535.f;
}

float valueNoise3(float x, float y, float z, uint32_t seed)
{
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = int(fx), iy = int(fy), iz = int(fz);
    auto smooth = [](float t) { return t * t * (3.f - 2.f * t); };
    const float tx = smooth(x - fx), ty = smooth(y - fy), tz = smooth(z - fz);
    auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    auto plane = [&](int dz) {
        const float a = lattice(ix, iy, iz + dz, seed), b = lattice(ix + 1, iy, iz + dz, seed);
        const float c = lattice(ix, iy + 1, iz + dz, seed);
        const float d = lattice(ix + 1, iy + 1, iz + dz, seed);
        return mix(mix(a, b, tx), mix(c, d, tx), ty);
    };
    return mix(plane(0), plane(1), tz);
}

} // namespace

float noiseGradientValue(
    float x, float y, float scalePx, int octaves, float evolution, uint32_t seed)
{
    float sum = 0.f, norm = 0.f, amp = 1.f, freq = 1.f;
    for (int o = 0; o < octaves; ++o) {
        // The offset per octave keeps the layers from lining up at the origin.
        const float off = 37.f * float(o);
        sum += amp
            * valueNoise3(
                x / scalePx * freq + off, y / scalePx * freq + off, evolution * freq + off, seed);
        norm += amp;
        amp *= 0.5f;
        freq *= 2.f;
    }
    return sum / norm;
}

float gridCoverage(
    float x, float y, float cellW, float cellH, float lineW, int majorEvery, float majorW)
{
    auto axis = [&](float p, float cell) {
        const float line
            = std::floor(p / cell + 0.5f); // not round(): half-way cases differ in GLSL
        const float d = std::abs(p - line * cell);
        const bool major = majorEvery > 0 && int(line) % majorEvery == 0;
        const float w = major ? majorW : lineW;
        return w <= 0.f ? 0.f : clamp01(w * 0.5f - d + 0.5f);
    };
    return std::max(axis(x, cellW), axis(y, cellH));
}

} // namespace kk::render::testpattern
