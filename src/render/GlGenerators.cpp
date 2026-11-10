// SPDX-License-Identifier: GPL-3.0-or-later
// GlGenerators — GLSL versions of the generator patterns of render/Generators.cpp. The
// formulas (and the noise hash / bar table) are kept identical so GPU, CPU fallback and tests
// agree; tests/render/test_generators.cpp checks them against each other.
#include "render/GlGenerators.h"

#include "render/TestPatterns.h"

#include <cmath>
#include <numbers>
#include <string>

namespace kk::render {

namespace {

// Mode numbers of the shader (uMode).
constexpr int kModeNone = 0;
constexpr int kModeFourColor = 1;
constexpr int kModeLinear = 2;
constexpr int kModeRadial = 3;
constexpr int kModeBars = 4;
constexpr int kModeGrey = 5;
constexpr int kModeNoise = 6;
constexpr int kModeCountdown = 7;
constexpr int kModeWindow = 8;
constexpr int kModeYcbcr = 9;
constexpr int kModeNoiseGradient = 10;
constexpr int kModeGrid = 11;
constexpr int kModeBt2111 = 12;

const char* const kFragment = R"(
uniform int uMode;
uniform vec2 uFrame;      // timeline pixels covered by the target
uniform vec4 uColors[4];  // premultiplied
uniform vec2 uCenter;     // gradient centre, timeline px from the frame centre
uniform vec2 uDir;        // linear gradient direction (unit)
uniform float uLength;
uniform float uRadius;
uniform int uFull;
uniform int uSteps;
uniform int uVertical;
uniform int uMono;
uniform vec3 uNoise;      // brightness, amount, grain
uniform uint uSeed;
uniform float uSweep;
uniform vec3 uDial;       // countdown radius, ring width, (unused) in timeline px
// 1.2 generators (twins of render/TestPatterns.cpp)
uniform vec3 uPal[39];    // BT.2111 palette (signal levels)
uniform int uBt;          // BT.2111 variant: 0 HLG narrow, 1 PQ narrow, 2 PQ full
uniform int uBand;        // YCbCr ramp: -1 = all three, else 0 Y', 1 Cb, 2 Cr
uniform vec2 uWinSize;    // window half width, half height (px)
uniform vec2 uWinMisc;    // window: ellipse (0 / 1), softness (px)
uniform vec4 uNg;         // noise gradient: scale px, octaves, contrast factor, evolution
uniform vec4 uGrid;       // grid: cell width, cell height, line width, major every
uniform float uGridMajor; // grid: major line width

vec3 rgb8(uint c) { return vec3(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u)) / 255.0; }

vec3 bars(vec2 uv)
{
    uv = clamp(uv, 0.0, 0.999999);
    if (uFull == 2) {
        const uint ebu[8] = uint[8](0xffffffu, 0xbfbf00u, 0x00bfbfu, 0x00bf00u, 0xbf00bfu,
                                    0xbf0000u, 0x0000bfu, 0x000000u);
        return rgb8(ebu[int(uv.x * 8.0)]);
    }
    if (uFull == 1) {
        const uint full[8] = uint[8](0xffffffu, 0xffff00u, 0x00ffffu, 0x00ff00u, 0xff00ffu,
                                     0xff0000u, 0x0000ffu, 0x000000u);
        return rgb8(full[int(uv.x * 8.0)]);
    }
    const uint top[7] = uint[7](0xbfbfbfu, 0xbfbf00u, 0x00bfbfu, 0x00bf00u, 0xbf00bfu,
                                0xbf0000u, 0x0000bfu);
    const uint mid[7] = uint[7](0x0000bfu, 0x000000u, 0xbf00bfu, 0x000000u, 0x00bfbfu,
                                0x000000u, 0xbfbfbfu);
    if (uv.y < 2.0 / 3.0) return rgb8(top[int(uv.x * 7.0)]);
    if (uv.y < 0.75) return rgb8(mid[int(uv.x * 7.0)]);
    float x = uv.x * 7.0;
    if (x < 1.25) return rgb8(0x00214cu);
    if (x < 2.5) return vec3(1.0);
    if (x < 3.75) return rgb8(0x32006au);
    if (x < 5.0 + 1.0 / 3.0) return vec3(0.0);
    if (x < 5.0 + 2.0 / 3.0) return rgb8(0x0a0a0au);
    if (x < 6.0) return rgb8(0x141414u);
    return vec3(0.0);
}

const int kBarEdges[6] = int[6](446, 652, 858, 1062, 1268, 1474);
const int kStairEdges[11] = int[11](549, 652, 755, 858, 960, 1062, 1165, 1268, 1371, 1474, 1577);

int bt2111Patch(int ix, int iy)
{
    bool side = ix < 240 || ix >= 1680;
    if (iy < 630) {
        if (side) return 0;
        int bar = 0;
        for (int i = 0; i < 6; ++i) bar += ix >= kBarEdges[i] ? 1 : 0;
        return (iy < 90 ? 1 : 8) + bar;
    }
    if (iy < 720) {
        if (side) return 8;
        if (ix < 446) return 16;
        int stairStep = 0;
        for (int i = 0; i < 11; ++i) stairStep += ix >= kStairEdges[i] ? 1 : 0;
        return 17 + stairStep;
    }
    if (iy < 810) return ix < 240 ? 29 : -1;
    if (ix < 80) return 33;
    if (ix < 160) return 34;
    if (ix < 240) return 35;
    if (ix < 376) return 29;
    if (ix < 446) return 30;
    if (ix < 514) return 29;
    if (ix < 584) return 31;
    if (ix < 652) return 29;
    if (ix < 722) return 32;
    if (ix < 960) return 29;
    if (ix < 1398) return 8;
    if (ix < 1680) return 29;
    if (ix < 1760) return 36;
    if (ix < 1840) return 37;
    return 38;
}

vec3 bt2111(vec2 uv)
{
    int ix = clamp(int(floor(uv.x * 1920.0)), 0, 1919);
    int iy = clamp(int(floor(uv.y * 1080.0)), 0, 1079);
    int p = bt2111Patch(ix, iy);
    if (p >= 0) return uPal[p];
    int x = ix - 240;
    if (uBt == 2) {
        int code = x < 618 ? 0 : (x < 1640 ? 1 + (x - 618) : 1023);
        return vec3(float(code) / 1023.0);
    }
    int code = x < 559 ? 4 : (x < 1573 ? 5 + (x - 559) : 1019);
    return vec3(clamp(float(code - 64) / 876.0, 0.0, 1.0));
}

vec3 ycbcrToRgb(float y, float cb, float cr)
{
    return clamp(vec3(y + 1.5748 * cr, y - 0.187324 * cb - 0.468124 * cr, y + 1.8556 * cb), 0.0, 1.0);
}

vec3 ycbcrRamp(int band, float t)
{
    t = clamp(t, 0.0, 1.0);
    if (band == 0) return ycbcrToRgb(t, 0.0, 0.0);
    if (band == 1) return ycbcrToRgb(0.5, t - 0.5, 0.0);
    return ycbcrToRgb(0.5, 0.0, t - 0.5);
}

float noiseGradient(vec2 p)
{
    return fractalNoise(p, uNg.x, int(uNg.y), uNg.w, uSeed);
}

float gridAxis(float p, float cell)
{
    float line = floor(p / cell + 0.5);
    float d = abs(p - line * cell);
    int every = int(uGrid.w);
    bool major = every > 0 && int(line) % every == 0;
    float w = major ? uGridMajor : uGrid.z;
    return w <= 0.0 ? 0.0 : clamp(w * 0.5 - d + 0.5, 0.0, 1.0);
}

void main()
{
    vec2 p = vQuad * uFrame;            // timeline px, origin top-left
    vec2 c = p - 0.5 * uFrame;          // from the frame centre
    vec4 o = vec4(0.0);
    if (uMode == 1) {
        o = mix(mix(uColors[0], uColors[1], vQuad.x), mix(uColors[2], uColors[3], vQuad.x), vQuad.y);
    } else if (uMode == 2) {
        float t = clamp(dot(c - uCenter, uDir) / uLength + 0.5, 0.0, 1.0);
        o = mix(uColors[0], uColors[1], t);
    } else if (uMode == 3) {
        o = mix(uColors[0], uColors[1], clamp(length(c - uCenter) / uRadius, 0.0, 1.0));
    } else if (uMode == 4) {
        o = vec4(bars(vQuad), 1.0);
    } else if (uMode == 5) {
        float t = uVertical != 0 ? vQuad.y : vQuad.x;
        float level = uSteps < 2 ? t : min(floor(t * float(uSteps)), float(uSteps - 1)) / float(uSteps - 1);
        o = vec4(vec3(level), 1.0);
    } else if (uMode == 6) {
        uvec2 cell = uvec2(max(floor(p / uNoise.z), vec2(0.0)));
        float r = float(noiseHash(cell.x, cell.y, uSeed) & 0xffffu) / 65535.0;
        vec3 n = vec3(r);
        if (uMono == 0) {
            n.g = float(noiseHash(cell.x, cell.y, uSeed + 1u) & 0xffffu) / 65535.0;
            n.b = float(noiseHash(cell.x, cell.y, uSeed + 2u) & 0xffffu) / 65535.0;
        }
        o = vec4(clamp(uNoise.x + uNoise.y * (n - 0.5), 0.0, 1.0), 1.0);
    } else if (uMode == 7) {
        float r = length(c);
        float ring = uDial.y;
        float outer = uDial.x;
        o = uColors[0];
        if (r < outer) {
            float a = atan(c.x, -c.y) / 6.283185307179586;
            if (a < 0.0) a += 1.0;
            if (a < uSweep) o = uColors[1];
        }
        if (abs(r - outer) < ring || abs(r - outer * 0.86) < ring * 0.5
            || abs(c.x) < ring * 0.4 || abs(c.y) < ring * 0.4)
            o = uColors[2];
    } else if (uMode == 8) {
        vec2 q = c - uCenter;
        float d = uWinMisc.x > 0.5 ? (length(q / uWinSize) - 1.0) * min(uWinSize.x, uWinSize.y)
                                   : max(abs(q.x) - uWinSize.x, abs(q.y) - uWinSize.y);
        float cover = uWinMisc.y > 0.0 ? clamp(0.5 - d / uWinMisc.y, 0.0, 1.0) : (d < 0.0 ? 1.0 : 0.0);
        o = mix(uColors[0], uColors[1], cover);
    } else if (uMode == 9) {
        float along = uVertical != 0 ? vQuad.y : vQuad.x;
        float across = uVertical != 0 ? vQuad.x : vQuad.y;
        int band = uBand >= 0 ? uBand : min(2, int(across * 3.0));
        o = vec4(ycbcrRamp(band, along), 1.0);
    } else if (uMode == 10) {
        float n = noiseGradient(p);
        o = mix(uColors[0], uColors[1], clamp((n - 0.5) * uNg.z + 0.5, 0.0, 1.0));
    } else if (uMode == 11) {
        float cover = max(gridAxis(p.x, uGrid.x), gridAxis(p.y, uGrid.y));
        o = mix(uColors[0], uColors[1], cover);
    } else if (uMode == 12) {
        o = vec4(bt2111(vQuad), 1.0);
    }
    fragColor = o;
}
)";

int modeOf(GeneratorFrame::Kind kind)
{
    using K = GeneratorFrame::Kind;
    switch (kind) {
    case K::FourColor:
        return kModeFourColor;
    case K::Linear:
        return kModeLinear;
    case K::Radial:
        return kModeRadial;
    case K::Bars:
        return kModeBars;
    case K::GreyScale:
        return kModeGrey;
    case K::Noise:
        return kModeNoise;
    case K::Countdown:
        return kModeCountdown;
    case K::Window:
        return kModeWindow;
    case K::YCbCr:
        return kModeYcbcr;
    case K::NoiseGradient:
        return kModeNoiseGradient;
    case K::Grid:
        return kModeGrid;
    case K::Bt2111:
        return kModeBt2111;
    case K::Unknown:
    case K::Timecode:
        break;
    }
    return kModeNone;
}

} // namespace

bool GlGenerators::initialize(GlKit& kit, QString* why)
{
    GlFunctions* gl = kit.gl();
    const std::string source = std::string(testpattern::kGlslNoise) + kFragment;
    if (!m_program.build(gl, "generators", source.c_str(), why))
        return false;
    m_mode = m_program.uniform(gl, "uMode");
    m_frame = m_program.uniform(gl, "uFrame");
    m_colors = m_program.uniform(gl, "uColors");
    m_center = m_program.uniform(gl, "uCenter");
    m_dir = m_program.uniform(gl, "uDir");
    m_length = m_program.uniform(gl, "uLength");
    m_radius = m_program.uniform(gl, "uRadius");
    m_full = m_program.uniform(gl, "uFull");
    m_steps = m_program.uniform(gl, "uSteps");
    m_vertical = m_program.uniform(gl, "uVertical");
    m_mono = m_program.uniform(gl, "uMono");
    m_noise = m_program.uniform(gl, "uNoise");
    m_seed = m_program.uniform(gl, "uSeed");
    m_sweep = m_program.uniform(gl, "uSweep");
    m_dial = m_program.uniform(gl, "uDial");
    m_pal = m_program.uniform(gl, "uPal");
    m_bt = m_program.uniform(gl, "uBt");
    m_band = m_program.uniform(gl, "uBand");
    m_winSize = m_program.uniform(gl, "uWinSize");
    m_winMisc = m_program.uniform(gl, "uWinMisc");
    m_ng = m_program.uniform(gl, "uNg");
    m_grid = m_program.uniform(gl, "uGrid");
    m_gridLine = m_program.uniform(gl, "uGridMajor");
    return true;
}

void GlGenerators::destroy(GlKit& kit)
{
    m_program.destroy(kit.gl());
}

void GlGenerators::drawPattern(GlKit& kit, const GeneratorFrame& g, QSizeF frameSize)
{
    GlFunctions* gl = kit.gl();
    kit.setBlendOver(false);
    const int mode = modeOf(g.kind);
    if (mode == kModeNone) {
        kit.clearTransparent();
        return;
    }
    gl->glUseProgram(m_program.id);
    kit.setFullscreen(m_program);
    float colors[16];
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 4; ++k)
            colors[i * 4 + k] = g.colors[size_t(i)][size_t(k)];
    }
    const double a = g.angleDeg * std::numbers::pi / 180.0;
    gl->glUniform1i(m_mode, mode);
    gl->glUniform2f(m_frame, float(frameSize.width()), float(frameSize.height()));
    gl->glUniform4fv(m_colors, 4, colors);
    gl->glUniform2f(m_center, float(g.centerX), float(g.centerY));
    gl->glUniform2f(m_dir, float(std::cos(a)), float(std::sin(a)));
    gl->glUniform1f(m_length, float(g.length));
    gl->glUniform1f(m_radius, float(g.radius));
    gl->glUniform1i(m_full, g.ebuBars ? 2 : (g.fullBars ? 1 : 0));
    gl->glUniform1i(m_steps, g.steps);
    gl->glUniform1i(m_vertical, g.vertical ? 1 : 0);
    gl->glUniform1i(m_mono, g.monochrome ? 1 : 0);
    gl->glUniform3f(m_noise, float(g.brightness), float(g.amount), float(g.grain));
    gl->glUniform1ui(m_seed, g.seed);
    gl->glUniform1f(m_sweep, float(g.sweep));
    gl->glUniform3f(m_dial, float(kCountdownRadius * frameSize.height()),
        float(kCountdownRing * frameSize.height()), 0.0f);
    gl->glUniform1i(m_bt, g.bt2111);
    if (g.kind == GeneratorFrame::Kind::Bt2111) {
        float pal[testpattern::kBt2111Patches * 3];
        const auto& palette = testpattern::bt2111Palette(testpattern::Bt2111(g.bt2111));
        for (size_t i = 0; i < palette.size(); ++i) {
            pal[i * 3] = palette[i].r;
            pal[i * 3 + 1] = palette[i].g;
            pal[i * 3 + 2] = palette[i].b;
        }
        gl->glUniform3fv(m_pal, testpattern::kBt2111Patches, pal);
    }
    gl->glUniform1i(m_band, g.ycbcrBand);
    gl->glUniform2f(m_winSize, float(g.windowW / 2), float(g.windowH / 2));
    gl->glUniform2f(m_winMisc, g.windowEllipse ? 1.f : 0.f, float(g.softness));
    gl->glUniform4f(m_ng, float(g.noiseScale), float(g.noiseOctaves), float(g.noiseContrast),
        float(g.noiseEvolution));
    gl->glUniform4f(m_grid, float(frameSize.width() / g.gridColumns),
        float(frameSize.height() / g.gridRows), float(g.lineWidth), float(g.majorEvery));
    gl->glUniform1f(m_gridLine, float(g.majorWidth));
    kit.drawQuad();
}

} // namespace kk::render
