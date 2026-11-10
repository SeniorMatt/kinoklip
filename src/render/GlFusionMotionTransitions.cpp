// SPDX-License-Identifier: GPL-3.0-or-later
// GlFusionMotionTransitions — the rest of the Fusion Transitions folder: Slide Left / Right /
// Up / Down, Pan Left / Right / Up / Down, Fall and Bounce, Film Strip, Foreground Wipe, Paint On
// and Round and Down. Designed from one-line descriptions of such transitions (clean room,
// docs/clean-room.md); one shader, `uKind` picks the picture. Every kind is exactly the outgoing
// picture at progress 0 and the incoming one at progress 1.
#include "core/TransitionKeys.h"
#include "render/GlShaderTransition.h"

#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

namespace k = tkeys;

const char* const kFusionMotionFamily = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uProgress;
uniform int uKind;
uniform float uAspect;
uniform vec2 uDir;        // slide, pan: the way the pictures travel; fall: the way B falls
uniform int uCurve;       // 0 linear, 1 easing, 2 custom
uniform float uPower;     // custom curve steepness
uniform float uBlur;      // pan: 0..1
uniform int uCount;       // paint on: strokes
uniform float uSoft;      // foreground wipe, paint on: shadow, 0..1
uniform vec2 uBorder;     // foreground wipe: x border width (uv of the width)
uniform vec4 uColor;      // film strip, foreground wipe: premultiplied
uniform float uZoom;      // film strip: share of the width a frame keeps when zoomed out
uniform float uFlag;      // foreground wipe: invert
uniform vec2 uPivot;      // round and down: pivot in uv
uniform float uAngle;     // round and down: radians

const float PI = 3.14159265;

vec4 inside(sampler2D tex, vec2 uv)
{
    vec2 ok = step(vec2(0.0), uv) * step(uv, vec2(1.0));
    return texture(tex, uv) * (ok.x * ok.y);
}
vec4 over(vec4 top, vec4 bottom) { return top + bottom * (1.0 - top.a); }
float outside(vec2 q) { return max(max(-q.x, q.x - 1.0), max(-q.y, q.y - 1.0)); }
vec4 darken(vec4 c, float amount) { return vec4(c.rgb * (1.0 - amount), c.a); }
float hash(float n) { return fract(sin(n * 127.1 + 311.7) * 43758.5453); }
float noise1(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i.x + 57.0 * i.y);
    float b = hash(i.x + 1.0 + 57.0 * i.y);
    float c = hash(i.x + 57.0 * (i.y + 1.0));
    float d = hash(i.x + 1.0 + 57.0 * (i.y + 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// The curve of the movement: 0 -> 0 and 1 -> 1 for all three.
float curve(float t)
{
    if (uCurve == 0)
        return t;
    if (uCurve == 1)
        return t * t * (3.0 - 2.0 * t);
    float a = pow(t, uPower);
    float b = pow(1.0 - t, uPower);
    return a / max(a + b, 1e-6);
}

// A ball dropped on the floor: 0 -> 1 with three bounces off 1.
float bounceOut(float t)
{
    if (t < 1.0 / 2.75)
        return 7.5625 * t * t;
    if (t < 2.0 / 2.75) {
        t -= 1.5 / 2.75;
        return 7.5625 * t * t + 0.75;
    }
    if (t < 2.5 / 2.75) {
        t -= 2.25 / 2.75;
        return 7.5625 * t * t + 0.9375;
    }
    t -= 2.625 / 2.75;
    return 7.5625 * t * t + 0.984375;
}

vec4 slide(float t)
{
    float e = curve(t);
    vec2 q = vUv - uDir * (e - 1.0);
    float d = outside(q);
    // A soft shadow of the sliding picture on the one beneath, gone at both ends.
    float shadow = d > 0.0 ? 0.45 * sin(PI * t) * (1.0 - smoothstep(0.0, 0.1, d)) : 0.0;
    return over(inside(uB, q), darken(texture(uA, vUv), shadow));
}

vec4 pan(float t)
{
    float e = curve(t);
    float len = uBlur * 0.3 * sin(PI * t);
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 8; ++i) {
        float s = e + (float(i) / 7.0 - 0.5) * len;
        sum += inside(uA, vUv - uDir * s) + inside(uB, vUv - uDir * (s - 1.0));
    }
    return sum / 8.0;
}

vec4 fallAndBounce(float t)
{
    vec2 q = vUv + uDir * (1.0 - bounceOut(t));
    float d = outside(q);
    float shadow = d > 0.0 ? 0.5 * sin(PI * t) * (1.0 - smoothstep(0.0, 0.08, d)) : 0.0;
    return over(inside(uB, q), darken(texture(uA, vUv), shadow));
}

vec4 filmStrip(float t)
{
    const float margin = 0.12;  // film edge with the holes, in frame widths
    const float gap = 0.15;     // space between two frames, in frame heights
    float pitch = 1.0 + gap;
    float z = mix(1.0, uZoom, sin(PI * t));
    float u = (vUv.x - 0.5) / z + 0.5;
    float yy = (vUv.y - 0.5) / z + curve(t) * pitch;
    bool onFilm = u > -margin && u < 1.0 + margin;
    if (!onFilm)
        return uColor * 0.35 + vec4(0.0, 0.0, 0.0, 0.65);
    if (u >= 0.0 && u <= 1.0) {
        if (abs(yy) <= 0.5)
            return texture(uA, vec2(u, yy + 0.5));
        if (abs(yy - pitch) <= 0.5)
            return texture(uB, vec2(u, yy - pitch + 0.5));
    }
    // Film base, with a row of holes along both edges.
    float side = u < 0.5 ? -0.5 * margin : 1.0 + 0.5 * margin;
    float hole = step(abs(u - side), 0.035) * step(0.3, fract(yy * 8.0)) * step(fract(yy * 8.0), 0.7);
    return mix(uColor, vec4(0.92, 0.92, 0.92, 1.0), hole);
}

vec4 foregroundWipe(float t)
{
    const float halfWidth = 0.14;
    const float zoomed = 1.35;
    float ss = 0.02 + 0.2 * uSoft;
    float reach = halfWidth + uBorder.x + ss + 0.01;
    float x = uFlag > 0.5 ? 1.0 - vUv.x : vUv.x;
    float c = mix(-reach, 1.0 + reach, t);
    float d = abs(x - c) - halfWidth;
    if (d < 0.0) { // the column: the incoming picture, magnified about the column's centre
        float sx = c + (x - c) / zoomed;
        return texture(uB, vec2(uFlag > 0.5 ? 1.0 - sx : sx, 0.5 + (vUv.y - 0.5) / zoomed));
    }
    if (d < uBorder.x)
        return uColor;
    vec4 base = x < c ? texture(uB, vUv) : texture(uA, vUv);
    return darken(base, 0.5 * (1.0 - smoothstep(0.0, ss, d - uBorder.x)));
}

vec4 paintOn(float t)
{
    float n = float(uCount);
    float row = floor(vUv.y * n);
    float fy = fract(vUv.y * n);
    // Every stroke starts at its own time; all are finished at t = 1.
    float p = clamp((t - 0.5 * hash(row)) / 0.5, 0.0, 1.0);
    float head = p * 1.25 - 0.1;
    float rounded = 0.06 * (1.0 - sqrt(max(sin(PI * fy), 0.0)));
    float edgeX = head - rounded + 0.015 * (noise1(vec2(vUv.y * 40.0, row)) - 0.5);
    float painted = 1.0 - smoothstep(edgeX - 0.004, edgeX + 0.004, vUv.x);
    float shadow = uSoft * 0.5 * (1.0 - smoothstep(0.0, 0.05, vUv.x - edgeX));
    return mix(darken(texture(uA, vUv), shadow), texture(uB, vUv), painted);
}

vec4 roundAndDown(float t)
{
    float e = curve(t);
    vec2 d = (vUv - uPivot) * vec2(uAspect, 1.0);
    float a = uAngle * e; // the picture turns clockwise about the pivot
    vec2 src = vec2(cos(a) * d.x + sin(a) * d.y, -sin(a) * d.x + cos(a) * d.y);
    // The swinging picture fades out over the full incoming picture.
    return over(inside(uA, uPivot + src / vec2(uAspect, 1.0)) * (1.0 - t), texture(uB, vUv));
}

void main()
{
    float t = uProgress;
    if (uKind == 0)
        fragColor = slide(t);
    else if (uKind == 1)
        fragColor = pan(t);
    else if (uKind == 2)
        fragColor = fallAndBounce(t);
    else if (uKind == 3)
        fragColor = filmStrip(t);
    else if (uKind == 4)
        fragColor = foregroundWipe(t);
    else if (uKind == 5)
        fragColor = paintOn(t);
    else
        fragColor = roundAndDown(t);
}
)";

enum Kind {
    Slide = 0,
    Pan,
    FallAndBounce,
    FilmStrip,
    ForegroundWipe,
    PaintOn,
    RoundAndDown
};

constexpr double kDegToRad = std::numbers::pi / 180.0;

using Setter = std::function<void(GlKit&, const GlProgram&, const TransitionInput&)>;

/// Curve choice and its steepness (linear is the default of a missing parameter).
void setCurve(GlKit& kit, const GlProgram& program, const TransitionInput& in)
{
    const QString curve = staticChoice(*in.params, k::Curve, QStringLiteral("linear"));
    setInt(kit, program, "uCurve",
        curve == QLatin1String("easing") ? 1 : (curve == QLatin1String("custom") ? 2 : 0));
    setFloat(kit, program, "uPower", std::clamp(numberParam(in, k::Power, 2), 0.2, 5.0));
}

std::unique_ptr<GlTransition> make(const char* id, Kind kind, Setter extra)
{
    return std::make_unique<ShaderTransition>(id, kFusionMotionFamily,
        [kind, extra = std::move(extra)](
            GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setInt(kit, program, "uKind", kind);
            setFloat(kit, program, "uAspect", double(in.size.width()) / double(in.size.height()));
            // Neutral values for what a kind does not set.
            setInt(kit, program, "uCurve", 0);
            setFloat(kit, program, "uPower", 2);
            setVec2(kit, program, "uDir", -1, 0);
            if (extra)
                extra(kit, program, in);
        });
}

void add(std::vector<GlTransitionFactory>& out, const char* id, Kind kind, Setter extra)
{
    out.push_back({QLatin1String(id), [=] { return make(id, kind, extra); }});
}

void addDirections(std::vector<GlTransitionFactory>& out, Kind kind, const char* const ids[4])
{
    static const char* const kNames[4] = {"left", "right", "up", "down"};
    for (int i = 0; i < 4; ++i) {
        const QPointF d = directionVector(QLatin1String(kNames[i]));
        add(out, ids[i], kind,
            [d, kind](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
                setCurve(kit, program, in);
                setVec2(kit, program, "uDir", d.x(), d.y());
                if (kind == Pan) {
                    setFloat(kit, program, "uBlur",
                        std::clamp(numberParam(in, k::MotionBlur, 50), 0.0, 100.0) / 100.0);
                }
            });
    }
}

} // namespace

void appendFusionMotionTransitions(std::vector<GlTransitionFactory>& out)
{
    static const char* const kSlides[4] = {"slide_left", "slide_right", "slide_up", "slide_down"};
    static const char* const kPans[4] = {"pan_left", "pan_right", "pan_up", "pan_down"};
    addDirections(out, Slide, kSlides);
    addDirections(out, Pan, kPans);

    add(out, "fall_and_bounce", FallAndBounce,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            // Fall Angle: the way B falls, in degrees (90 = straight down the frame).
            const double a = numberParam(in, k::Angle, 90) * kDegToRad;
            setVec2(kit, program, "uDir", std::cos(a), std::sin(a));
        });
    add(out, "film_strip", FilmStrip,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setColor(kit, program, "uColor", staticColor(*in.params, k::Color, QColor(28, 28, 32)));
            setFloat(kit, program, "uZoom",
                std::clamp(numberParam(in, k::Zoom, 55), 10.0, 100.0) / 100.0);
            setCurve(kit, program, in);
        });
    add(out, "foreground_wipe", ForegroundWipe,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setColor(kit, program, "uColor", staticColor(*in.params, k::Color, QColor(Qt::white)));
            setVec2(kit, program, "uBorder",
                std::clamp(numberParam(in, k::Border, 4), 0.0, 200.0) / double(in.size.width()), 0);
            setFloat(kit, program, "uSoft",
                std::clamp(numberParam(in, k::Softness, 50), 0.0, 100.0) / 100.0);
            setFloat(kit, program, "uFlag", staticBool(*in.params, k::Invert, false) ? 1 : 0);
        });
    add(out, "paint_on", PaintOn,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setInt(
                kit, program, "uCount", int(std::clamp(numberParam(in, k::Count, 8), 2.0, 40.0)));
            setFloat(kit, program, "uSoft",
                std::clamp(numberParam(in, k::Shadow, 50), 0.0, 100.0) / 100.0);
        });
    add(out, "round_and_down", RoundAndDown,
        [](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setCurve(kit, program, in);
            setFloat(kit, program, "uAngle", numberParam(in, k::Angle, 90) * kDegToRad);
            // The pivot is given from the middle of the bottom edge (default: bottom left).
            setVec2(kit, program, "uPivot", 0.5 + numberParam(in, k::OffsetX, -50) / 100.0,
                1.0 + numberParam(in, k::OffsetY, 0) / 100.0);
        });
}

} // namespace kk::render
