// SPDX-License-Identifier: GPL-3.0-or-later
// GlMotionTransitions — the Motion folder beyond Push: Barn Door, Slide, Split. One shader
// that draws the pictures at a moved position; Motion Blur averages it over a short span of
// progress around the frame (eight samples, the span shrinks to nothing at both ends so the
// first and last pictures stay untouched).
#include "core/TransitionKeys.h"
#include "render/GlShaderTransition.h"

namespace kk::render {

namespace {

namespace k = tkeys;

const char* const kMotionFamily = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uProgress;
uniform int uKind;       // 0 barn door, 1 slide, 2 split
uniform float uFlag;     // barn door / split: 1 = reverse; slide: 1 = slide out
uniform float uAxis;     // barn door / split: 1 = horizontal preset
uniform vec2 uDir;       // slide: the way the picture travels
uniform float uBlur;     // 0..1

vec4 inside(sampler2D tex, vec2 uv)
{
    vec2 ok = step(vec2(0.0), uv) * step(uv, vec2(1.0));
    return texture(tex, uv) * (ok.x * ok.y);
}
vec4 over(vec4 top, vec4 bottom) { return top + bottom * (1.0 - top.a); }

// The two halves of the picture, moved apart by h (uv units) along the axis, and nothing in
// the gap.
vec4 halves(sampler2D tex, float h)
{
    bool horizontalPreset = uAxis > 0.5;
    float c = horizontalPreset ? vUv.y : vUv.x;
    float moved = c < 0.5 - h ? c + h : (c > 0.5 + h ? c - h : -1.0);
    if (moved < 0.0)
        return vec4(0.0);
    return texture(tex, horizontalPreset ? vec2(vUv.x, moved) : vec2(moved, vUv.y));
}

vec4 frameAt(float t)
{
    if (uKind == 0) {
        if (uFlag < 0.5)
            return over(halves(uA, 0.5 * t), texture(uB, vUv));
        return over(halves(uB, 0.5 * (1.0 - t)), texture(uA, vUv));
    }
    if (uKind == 1) {
        if (uFlag < 0.5)
            return over(inside(uB, vUv - uDir * (t - 1.0)), texture(uA, vUv));
        return over(inside(uA, vUv - uDir * t), texture(uB, vUv));
    }
    // Split: the first half of the screen pushes one way, the second the other.
    bool first = (uAxis > 0.5 ? vUv.y : vUv.x) < 0.5;
    vec2 d = uAxis > 0.5 ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    d *= (first ? -1.0 : 1.0) * (uFlag > 0.5 ? -1.0 : 1.0);
    return inside(uA, vUv - d * t) + inside(uB, vUv - d * (t - 1.0));
}

void main()
{
    if (uBlur <= 0.0) {
        fragColor = frameAt(uProgress);
        return;
    }
    float span = uBlur * 0.2 * 4.0 * uProgress * (1.0 - uProgress);
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 8; ++i)
        sum += frameAt(clamp(uProgress + (float(i) / 7.0 - 0.5) * span, 0.0, 1.0));
    fragColor = sum / 8.0;
}
)";

enum Kind {
    Barn = 0,
    Slide = 1,
    Split = 2
};

std::unique_ptr<GlTransition> make(const char* id, Kind kind)
{
    return std::make_unique<ShaderTransition>(
        id, kMotionFamily, [kind](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            const ParamSet& p = *in.params;
            setInt(kit, program, "uKind", kind);
            setFloat(kit, program, "uBlur",
                std::clamp(numberParam(in, k::MotionBlur, 0), 0.0, 100.0) / 100.0);
            const bool horizontal = staticChoice(p, k::Preset) == QLatin1String("horizontal");
            // Barn Door: "vertical" doors split along a vertical line (halves move sideways).
            // Split: "horizontal" cuts the picture into top and bottom, moving sideways.
            setFloat(kit, program, "uAxis", horizontal ? 1.0 : 0.0);
            if (kind == Slide) {
                const QPointF d
                    = directionVector(staticChoice(p, k::Direction, QStringLiteral("left")));
                setVec2(kit, program, "uDir", d.x(), d.y());
                setFloat(kit, program, "uFlag",
                    staticChoice(p, k::Mode, QStringLiteral("in")) == QLatin1String("out") ? 1 : 0);
            } else {
                setFloat(kit, program, "uFlag", staticBool(p, k::Reverse, false) ? 1 : 0);
            }
        });
}

} // namespace

void appendMotionTransitions(std::vector<GlTransitionFactory>& out)
{
    out.push_back({QStringLiteral("barn_door"), [] { return make("barn_door", Barn); }});
    out.push_back({QStringLiteral("slide"), [] { return make("slide", Slide); }});
    out.push_back({QStringLiteral("split"), [] { return make("split", Split); }});
}

} // namespace kk::render
