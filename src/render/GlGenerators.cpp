// SPDX-License-Identifier: GPL-3.0-or-later
// GlGenerators — GLSL versions of the generator patterns of render/Generators.cpp. The
// formulas (and the noise hash / bar table) are kept identical so GPU, CPU fallback and tests
// agree; tests/render/test_generators.cpp checks them against each other.
#include "render/GlGenerators.h"

#include <cmath>
#include <numbers>

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

uint noiseHash(uint x, uint y, uint seed)
{
    uint h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}

vec3 rgb8(uint c) { return vec3(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u)) / 255.0; }

vec3 bars(vec2 uv)
{
    uv = clamp(uv, 0.0, 0.999999);
    if (uFull != 0) {
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
    if (!m_program.build(gl, "generators", kFragment, why))
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
    gl->glUniform1i(m_full, g.fullBars ? 1 : 0);
    gl->glUniform1i(m_steps, g.steps);
    gl->glUniform1i(m_vertical, g.vertical ? 1 : 0);
    gl->glUniform1i(m_mono, g.monochrome ? 1 : 0);
    gl->glUniform3f(m_noise, float(g.brightness), float(g.amount), float(g.grain));
    gl->glUniform1ui(m_seed, g.seed);
    gl->glUniform1f(m_sweep, float(g.sweep));
    gl->glUniform3f(m_dial, float(kCountdownRadius * frameSize.height()),
        float(kCountdownRing * frameSize.height()), 0.0f);
    kit.drawQuad();
}

} // namespace kk::render
