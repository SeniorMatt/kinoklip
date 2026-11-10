// SPDX-License-Identifier: GPL-3.0-or-later
// GlFusionTransitions — the Fusion Transitions and Resolve FX Transitions folders: Brightness
// Flash, Zoom In, Zoom In and Out, Crash Zoom, Rotate, Rotate 90, Noise Dissolve, Glitch, Camera
// Shake, Circles, Flip 3D, Slice Push, Warp, Drop Warp, Tunnel of Light and Burn Away. Designed
// from one-line descriptions of such transitions (clean room, docs/clean-room.md); one shader,
// `uKind` picks the picture. Every kind starts as the outgoing picture and ends as the incoming
// one (the effect strength is zero at both ends).
#include "core/TransitionKeys.h"
#include "render/GlShaderTransition.h"

#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

namespace k = tkeys;

const char* const kFusionFamily = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uProgress;
uniform int uKind;
uniform float uAspect;
uniform vec2 uCenter;     // centre offset in uv units
uniform float uZoom;      // zoom: extra scale; rotate: total angle in radians
uniform float uStrength;  // 0..1
uniform float uSpeed;
uniform float uSize;      // noise: cell size in uv heights; tunnel: glow radius
uniform float uSoft;
uniform float uGlow;
uniform int uCount;
uniform float uAngle;
uniform float uChar;

const float PI = 3.14159265;

vec4 A(vec2 uv) { return texture(uA, clamp(uv, 0.0, 1.0)); }
vec4 B(vec2 uv) { return texture(uB, clamp(uv, 0.0, 1.0)); }
vec4 insideA(vec2 uv)
{
    vec2 ok = step(vec2(0.0), uv) * step(uv, vec2(1.0));
    return texture(uA, clamp(uv, 0.0, 1.0)) * (ok.x * ok.y);
}
vec4 insideB(vec2 uv)
{
    vec2 ok = step(vec2(0.0), uv) * step(uv, vec2(1.0));
    return texture(uB, clamp(uv, 0.0, 1.0)) * (ok.x * ok.y);
}
vec4 over(vec4 top, vec4 bottom) { return top + bottom * (1.0 - top.a); }
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x),
        mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}
float fbm(vec2 p) { return 0.5 * vnoise(p) + 0.3 * vnoise(p * 2.1) + 0.2 * vnoise(p * 4.3); }

vec2 centre() { return vec2(0.5) + uCenter; }
vec2 zoomAbout(vec2 uv, float s) { return centre() + (uv - centre()) / s; }
vec2 rotateAbout(vec2 uv, float angle)
{
    vec2 p = (uv - 0.5) * vec2(uAspect, 1.0);
    float c = cos(angle);
    float s = sin(angle);
    p = vec2(c * p.x + s * p.y, -s * p.x + c * p.y);
    return p / vec2(uAspect, 1.0) + 0.5;
}
// A -> B at weight w, sampled at uv.
vec4 mixAB(vec2 uv, float w) { return mix(A(uv), B(uv), w); }

void main()
{
    float t = uProgress;
    vec2 uv = vUv;
    float bell = sin(PI * t);

    if (uKind == 0) { // brightness flash
        vec4 m = mixAB(uv, smoothstep(0.3, 0.7, t));
        float f = bell * bell * uStrength;
        fragColor = vec4(m.rgb + f * m.a, m.a);
    } else if (uKind == 1) { // zoom in
        float sA = 1.0 + uZoom * t;
        float sB = (1.0 + uZoom * t) / (1.0 + uZoom);
        fragColor = mix(A(zoomAbout(uv, sA)), B(zoomAbout(uv, sB)), t);
    } else if (uKind == 2) { // zoom in and out
        float sA = 1.0 + uZoom * t;
        float sB = 1.0 + uZoom * (1.0 - t);
        fragColor = mix(A(zoomAbout(uv, sA)), B(zoomAbout(uv, sB)), t);
    } else if (uKind == 3) { // crash zoom: a fast zoom into A, then a fast zoom out of B
        bool first = t < 0.5;
        float q = first ? 2.0 * t : 2.0 * (1.0 - t);
        float s = 1.0 + uZoom * q * q * q;
        vec4 sum = vec4(0.0);
        for (int i = 0; i < 8; ++i) {
            float ss = 1.0 + (s - 1.0) * (1.0 - 0.25 * float(i) / 7.0);
            vec2 p = zoomAbout(uv, ss);
            sum += first ? A(p) : B(p);
        }
        fragColor = sum / 8.0;
    } else if (uKind == 4) { // rotate (uZoom = total angle)
        fragColor = mix(A(rotateAbout(uv, uZoom * t)), B(rotateAbout(uv, uZoom * (t - 1.0))), t);
    } else if (uKind == 5) { // noise dissolve
        float n = vnoise(uv * vec2(uAspect, 1.0) / uSize);
        float s = uSoft * 0.5 + 0.001;
        float th = mix(-s, 1.0 + s, t);
        float b = 1.0 - smoothstep(th - s, th + s, n);
        fragColor = mix(A(uv), B(uv), b);
    } else if (uKind == 6) { // glitch
        float g = bell * uStrength;
        float row = floor(uv.y * 24.0);
        float frame = floor(t * 14.0);
        float shift = (hash(vec2(row, frame)) - 0.5) * 0.4 * g
            * step(0.55, hash(vec2(row + 7.0, frame)));
        float pick = step(hash(vec2(row, 3.7)), t);
        float split = g * 0.02;
        vec2 p = vec2(uv.x + shift, uv.y);
        vec4 r = mixAB(p + vec2(split, 0.0), pick);
        vec4 gg = mixAB(p, pick);
        vec4 bb = mixAB(p - vec2(split, 0.0), pick);
        fragColor = vec4(r.r, gg.g, bb.b, gg.a);
    } else if (uKind == 7) { // camera shake
        float sh = bell * uStrength * 0.05;
        float ph = t * uSpeed * 6.2832;
        vec2 off = vec2(sin(ph * 1.3), cos(ph * 1.7)) * sh;
        float split = sh * 0.3;
        float w = smoothstep(0.3, 0.7, t);
        vec4 r = mixAB(uv + off + vec2(split, 0.0), w);
        vec4 gg = mixAB(uv + off, w);
        vec4 bb = mixAB(uv + off - vec2(split, 0.0), w);
        fragColor = vec4(r.r, gg.g, bb.b, gg.a);
    } else if (uKind == 8) { // circles
        vec2 p = (uv - centre()) * vec2(uAspect, 1.0);
        float r = length(p) / (0.5 * length(vec2(uAspect, 1.0)) + length(uCenter * vec2(uAspect, 1.0)));
        float q = fract(r * float(uCount));
        float w = 0.03;
        float e = t * (1.0 + w);
        float b = 1.0 - smoothstep(e - w, e, q);
        fragColor = mix(A(uv), B(uv), b);
    } else if (uKind == 9) { // flip 3d
        float phi = PI * t;
        float c = cos(phi);
        float x = uv.x - 0.5;
        float dx = max(abs(c), 0.001);
        float side = c >= 0.0 ? 1.0 : -1.0;
        float srcX = 0.5 + x / dx;
        float yScale = 1.0 + 0.5 * sin(phi) * x * side;
        float srcY = 0.5 + (uv.y - 0.5) * yScale;
        vec2 src = vec2(srcX, srcY);
        fragColor = side > 0.0 ? insideA(src) : insideB(src);
    } else if (uKind == 10) { // slice push
        float n = float(uCount);
        float i = floor(uv.x * n);
        float p = clamp(t * 1.6 - 0.6 * i / n, 0.0, 1.0);
        float d = mod(i, 2.0) < 0.5 ? 1.0 : -1.0;
        fragColor = insideA(uv - vec2(0.0, d * p)) + insideB(uv - vec2(0.0, d * (p - 1.0)));
    } else if (uKind == 11) { // warp: a swirl that draws the picture in
        vec2 p = (uv - centre()) * vec2(uAspect, 1.0);
        float r = length(p);
        float ang = uStrength * 6.0 * bell * exp(-r * 2.5);
        float c = cos(ang);
        float s = sin(ang);
        p = vec2(c * p.x - s * p.y, s * p.x + c * p.y) * (1.0 - 0.3 * bell * uStrength);
        fragColor = mixAB(centre() + p / vec2(uAspect, 1.0), smoothstep(0.35, 0.65, t));
    } else if (uKind == 12) { // drop warp
        vec2 p = (uv - centre()) * vec2(uAspect, 1.0);
        float r = length(p);
        float rmax = 0.5 * length(vec2(uAspect, 1.0)) + length(uCenter * vec2(uAspect, 1.0));
        float w = 0.03;
        float front = t * (rmax + w);
        float d = r - front;
        float wave = sin(d * 40.0) * exp(-d * d * 60.0) * uStrength * 0.04 * bell;
        vec2 uv2 = uv + normalize(p + vec2(1e-5)) * wave / vec2(uAspect, 1.0);
        float b = 1.0 - smoothstep(front - w, front, r);
        fragColor = mix(A(uv2), B(uv2), b);
    } else if (uKind == 13) { // tunnel of light
        vec2 p = (uv - centre()) * vec2(uAspect, 1.0);
        float r = length(p);
        vec4 a = insideA(centre() + (uv - centre()) / max(1.0 - t, 0.001));
        vec4 b = insideB(centre() + (uv - centre()) / max(t, 0.001));
        float glow = exp(-pow(r / uSize, 2.0)) * bell * uGlow;
        fragColor = over(b, a) + vec4(vec3(1.0, 0.95, 0.8) * glow, glow);
    } else { // 14: burn away
        vec2 dir = vec2(cos(uAngle), sin(uAngle));
        vec2 w = (uv - 0.5) * vec2(uAspect, 1.0);
        float extent = 0.5 * (abs(dir.x) * uAspect + abs(dir.y));
        float s = 0.5 + dot(w, dir) / (2.0 * extent);
        s += (fbm(uv * vec2(uAspect, 1.0) * 4.0) - 0.5) * 0.8 * uStrength;
        float charW = 0.02 + 0.2 * uChar;
        float edge = mix(-0.6, 1.6 + charW, t);
        float alive = smoothstep(edge - 0.02, edge, s);
        float charMask = smoothstep(edge - charW - 0.02, edge - charW, s);
        vec4 a = A(uv);
        vec4 burnt = vec4(mix(a.rgb * 0.12, a.rgb, alive), a.a);
        vec4 m = mix(B(uv), burnt, charMask);
        float glow = exp(-pow((s - edge) / 0.03, 2.0)) * uGlow;
        fragColor = vec4(m.rgb + vec3(1.0, 0.45, 0.1) * glow * max(m.a, 0.5), m.a);
    }
}
)";

enum Kind {
    Flash = 0,
    ZoomIn,
    ZoomInOut,
    CrashZoom,
    Rotate,
    NoiseDissolve,
    Glitch,
    CameraShake,
    Circles,
    Flip3d,
    SlicePush,
    Warp,
    DropWarp,
    Tunnel,
    BurnAway
};

double percent(const TransitionInput& in, const QString& key, double def, double max)
{
    return std::clamp(numberParam(in, key, def), 0.0, max) / 100.0;
}

void setPlace(GlKit& kit, const GlProgram& program, const TransitionInput& in)
{
    setFloat(kit, program, "uAspect", double(in.size.width()) / double(in.size.height()));
    setVec2(kit, program, "uCenter", numberParam(in, k::OffsetX, 0) / 100.0,
        numberParam(in, k::OffsetY, 0) / 100.0);
}

using Setter = std::function<void(GlKit&, const GlProgram&, const TransitionInput&)>;

std::unique_ptr<GlTransition> make(const char* id, int kind, Setter extra)
{
    return std::make_unique<ShaderTransition>(id, kFusionFamily,
        [kind, extra = std::move(extra)](
            GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setInt(kit, program, "uKind", kind);
            setPlace(kit, program, in);
            // Neutral values for what a kind does not set.
            setFloat(kit, program, "uZoom", 1);
            setFloat(kit, program, "uStrength", 1);
            setFloat(kit, program, "uSpeed", 12);
            setFloat(kit, program, "uSize", 0.1);
            setFloat(kit, program, "uSoft", 0.2);
            setFloat(kit, program, "uGlow", 1);
            setFloat(kit, program, "uAngle", 0);
            setFloat(kit, program, "uChar", 0.4);
            setInt(kit, program, "uCount", 8);
            if (extra)
                extra(kit, program, in);
        });
}

void add(std::vector<GlTransitionFactory>& out, const char* id, int kind, Setter extra = {})
{
    out.push_back({QLatin1String(id), [=] { return make(id, kind, extra); }});
}

} // namespace

void appendFusionTransitions(std::vector<GlTransitionFactory>& out)
{
    add(out, "brightness_flash", Flash,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setFloat(kit, program, "uStrength", percent(in, k::Brightness, 100, 300));
        });
    const auto zoom = [](double def) {
        return [def](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setFloat(kit, program, "uZoom", percent(in, k::Zoom, def, 500));
        };
    };
    add(out, "zoom_in", ZoomIn, zoom(100));
    add(out, "zoom_in_out", ZoomInOut, zoom(100));
    add(out, "crash_zoom", CrashZoom, zoom(150));
    add(out, "rotate", Rotate, [](GlKit& kit, const GlProgram& program, const TransitionInput&) {
        setFloat(kit, program, "uZoom", std::numbers::pi);
    });
    add(out, "rotate_90", Rotate, [](GlKit& kit, const GlProgram& program, const TransitionInput&) {
        setFloat(kit, program, "uZoom", std::numbers::pi / 2);
    });
    add(out, "noise_dissolve", NoiseDissolve,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setFloat(kit, program, "uSize",
                std::clamp(numberParam(in, k::Size, 40), 2.0, 400.0) / double(in.size.height()));
            setFloat(kit, program, "uSoft", percent(in, k::Softness, 20, 100));
        });
    add(out, "glitch", Glitch, [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
        setFloat(kit, program, "uStrength", percent(in, k::Strength, 50, 100));
    });
    add(out, "camera_shake_transition", CameraShake,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setFloat(kit, program, "uSpeed", std::clamp(numberParam(in, k::Speed, 12), 1.0, 50.0));
            setFloat(kit, program, "uStrength", percent(in, k::Strength, 30, 100));
        });
    add(out, "circles", Circles,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setInt(
                kit, program, "uCount", int(std::clamp(numberParam(in, k::Count, 8), 1.0, 50.0)));
        });
    add(out, "flip_3d", Flip3d);
    add(out, "slice_push", SlicePush,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setInt(
                kit, program, "uCount", int(std::clamp(numberParam(in, k::Count, 6), 2.0, 40.0)));
        });
    const Setter strength = [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
        setFloat(kit, program, "uStrength", percent(in, k::Strength, 50, 100));
    };
    add(out, "warp", Warp, strength);
    add(out, "drop_warp", DropWarp, strength);
    add(out, "tunnel_of_light", Tunnel,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setFloat(kit, program, "uGlow", percent(in, k::Glow, 100, 300));
            setFloat(kit, program, "uSize", std::max(0.01, percent(in, k::Size, 20, 100)));
        });
    add(out, "burn_away", BurnAway,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setFloat(
                kit, program, "uAngle", numberParam(in, k::Angle, 0) * std::numbers::pi / 180.0);
            setFloat(kit, program, "uStrength", percent(in, k::Raggedness, 50, 100));
            setFloat(kit, program, "uGlow", percent(in, k::Glow, 100, 300));
            setFloat(kit, program, "uChar", percent(in, k::Char, 40, 100));
        });
}

} // namespace kk::render
