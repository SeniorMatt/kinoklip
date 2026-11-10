// SPDX-License-Identifier: GPL-3.0-or-later
// GlTransitions — the first four built-in transitions (cross dissolve, dip to color, wipe, push)
// as single GLSL passes, and the table that collects every family.
#include "render/GlTransitions.h"

#include "core/TransitionKeys.h"
#include "render/GlShaderTransition.h"

#include <QColor>
#include <QHash>

#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

const QString kDirection = QStringLiteral("direction");
const QString kSoftness = QStringLiteral("softness");
const QString kAngle = QStringLiteral("angle");
const QString kColor = QStringLiteral("color");

// ---- Shaders ----

const char* const kDissolve = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uProgress;
void main() { fragColor = mix(texture(uA, vUv), texture(uB, vUv), uProgress); }
)";

const char* const kDip = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uProgress;
uniform vec4 uColor; // premultiplied
void main()
{
    // First half: A → colour; second half: colour → B.
    fragColor = uProgress < 0.5 ? mix(texture(uA, vUv), uColor, uProgress * 2.0)
                                : mix(uColor, texture(uB, vUv), uProgress * 2.0 - 1.0);
}
)";

// s ∈ [0, 1] = how late the edge reaches this pixel (0 at the side the wipe starts from).
// B covers pixels with s < edge; the edge travels from −softness to 1 so both ends are pure.
const char* const kWipe = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uProgress;
uniform vec2 uDir;       // unit travel direction of the edge, y down
uniform float uSoftness; // width of the blend band, 0..1 of the travel distance
uniform float uAspect;   // width / height (the edge stays straight at any angle)
uniform float uBorder;   // border width, 0..1 of the travel distance (0 = none)
uniform float uReverse;  // 1 = the edge starts at the far side
uniform vec4 uBorderColor; // premultiplied
void main()
{
    vec2 p = (vUv - 0.5) * vec2(uAspect, 1.0);
    float extent = 0.5 * (abs(uDir.x) * uAspect + abs(uDir.y));
    float s = 0.5 + dot(p, uDir) / (2.0 * extent);
    s = mix(s, 1.0 - s, uReverse);
    float w = max(uSoftness, 1e-4);
    float edge = uProgress * (1.0 + w + uBorder);
    float b = 1.0 - smoothstep(edge - w, edge, s);
    vec4 colour = mix(texture(uA, vUv), texture(uB, vUv), b);
    // The border: a crisp strip on the outgoing side of the edge; it is outside the picture at
    // both ends of the transition.
    if (uBorder > 0.0) {
        const float aa = 0.003;
        float m = smoothstep(edge - uBorder - aa, edge - uBorder, s) * (1.0 - smoothstep(edge - aa, edge, s));
        colour = mix(colour, uBorderColor, m);
    }
    fragColor = colour;
}
)";

// A moves out by d·t, B follows from the opposite side: screen point p shows A(p − d·t) and
// B(p − d·(t − 1)); samples outside the picture are transparent.
const char* const kPush = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uProgress;
uniform vec2 uDir;
uniform vec2 uBorder;    // x: border width in uv units of the travel axis (0 = none), y: feather 0..1
uniform vec4 uBorderColor; // premultiplied
vec4 inside(sampler2D t, vec2 uv)
{
    vec2 ok = step(vec2(0.0), uv) * step(uv, vec2(1.0));
    return texture(t, uv) * (ok.x * ok.y);
}
void main()
{
    vec2 q = vUv - uDir * uProgress;
    vec4 colour = inside(uA, q) + inside(uB, vUv - uDir * (uProgress - 1.0));
    // A border on the incoming side of the edge, swelling and shrinking with sin(pi t) so
    // both ends of the transition stay untouched.
    float w = uBorder.x * sin(3.14159265 * uProgress);
    float d = max(max(-q.x, q.x - 1.0), max(-q.y, q.y - 1.0));
    if (w > 1e-6 && d > 0.0)
        colour = mix(colour, uBorderColor,
            1.0 - clamp((d - w * (1.0 - uBorder.y)) / max(w * uBorder.y, 1e-6), 0.0, 1.0));
    fragColor = colour;
}
)";

// ---- Uniform setters ----

/// Travel direction: optional numeric "angle" (degrees, 0 = towards the right, clockwise)
/// overrides the "direction" choice.
QPointF travelDirection(const TransitionInput& in)
{
    if (in.params->values.contains(kAngle)) {
        const double a = in.params->value(kAngle, in.localFrame, 0.0) * std::numbers::pi / 180.0;
        return QPointF(std::cos(a), std::sin(a));
    }
    return directionVector(staticChoice(*in.params, kDirection));
}

/// Writes the ratio-mapped position (ratioRange()) into uProgress, so every dissolve shader
/// keeps its own maths.
void setRatioProgress(GlKit& kit, const GlProgram& program, const TransitionInput& in)
{
    const auto [start, end] = ratioRange(in);
    setFloat(kit, program, "uProgress", start + (end - start) * in.progress);
}

void setDip(GlKit& kit, const GlProgram& program, const TransitionInput& in)
{
    setRatioProgress(kit, program, in);
    const QColor c = staticColor(*in.params, kColor, QColor(Qt::black));
    const float a = float(c.alphaF());
    kit.gl()->glUniform4f(program.uniform(kit.gl(), "uColor"), float(c.redF()) * a,
        float(c.greenF()) * a, float(c.blueF()) * a, a);
}

void setWipe(GlKit& kit, const GlProgram& program, const TransitionInput& in)
{
    GlFunctions* gl = kit.gl();
    const QPointF d = travelDirection(in);
    // Border: pixels, as a share of the distance the edge travels (in picture heights * 2).
    const double aspect = double(in.size.width()) / double(in.size.height());
    const double extent = 0.5 * (std::abs(d.x()) * aspect + std::abs(d.y()));
    setFloat(kit, program, "uBorder",
        std::clamp(numberParam(in, tkeys::Border, 0), 0.0, 200.0)
            / (2.0 * extent * double(in.size.height())));
    setFloat(kit, program, "uReverse", staticBool(*in.params, tkeys::Reverse, false) ? 1.0 : 0.0);
    setColor(kit, program, "uBorderColor", staticColor(*in.params, kColor, QColor(Qt::white)));
    gl->glUniform2f(program.uniform(gl, "uDir"), float(d.x()), float(d.y()));
    gl->glUniform1f(program.uniform(gl, "uSoftness"),
        float(std::clamp(in.params->value(kSoftness, in.localFrame, 10.0), 0.0, 100.0) / 100.0));
    gl->glUniform1f(
        program.uniform(gl, "uAspect"), float(in.size.width()) / float(in.size.height()));
}

void setPush(GlKit& kit, const GlProgram& program, const TransitionInput& in)
{
    const QPointF d = travelDirection(in);
    setBorder(kit, program, in,
        1.0 / double(std::abs(d.x()) >= std::abs(d.y()) ? in.size.width() : in.size.height()));
    kit.gl()->glUniform2f(program.uniform(kit.gl(), "uDir"), float(d.x()), float(d.y()));
}

std::unique_ptr<GlTransition> makeDissolve()
{
    return std::make_unique<ShaderTransition>("cross_dissolve", kDissolve, &setRatioProgress);
}
std::unique_ptr<GlTransition> makeDip()
{
    return std::make_unique<ShaderTransition>("dip_to_color", kDip, &setDip);
}
std::unique_ptr<GlTransition> makeWipe()
{
    return std::make_unique<ShaderTransition>("wipe", kWipe, &setWipe);
}
std::unique_ptr<GlTransition> makePush()
{
    return std::make_unique<ShaderTransition>("push", kPush, &setPush);
}

} // namespace

const std::vector<GlTransitionFactory>& builtinGlTransitions()
{
    static const std::vector<GlTransitionFactory> table = [] {
        std::vector<GlTransitionFactory> t;
        t.push_back({QStringLiteral("cross_dissolve"), &makeDissolve});
        t.push_back({QStringLiteral("dip_to_color"), &makeDip});
        t.push_back({QStringLiteral("wipe"), &makeWipe});
        t.push_back({QStringLiteral("push"), &makePush});
        appendDissolveTransitions(t);
        appendShapeTransitions(t);
        appendMotionTransitions(t);
        appendFusionTransitions(t);
        appendFusionMotionTransitions(t);
        return t;
    }();
    return table;
}

const GlTransitionFactory* findGlTransitionFactory(const QString& id)
{
    static const QHash<QString, const GlTransitionFactory*> byId = [] {
        QHash<QString, const GlTransitionFactory*> h;
        for (const GlTransitionFactory& f : builtinGlTransitions())
            h.insert(f.id, &f);
        return h;
    }();
    return byId.value(id, nullptr);
}

QPointF directionVector(const QString& direction)
{
    if (direction == QLatin1String("right"))
        return {1, 0};
    if (direction == QLatin1String("up"))
        return {0, -1};
    if (direction == QLatin1String("down"))
        return {0, 1};
    return {-1, 0};
}

} // namespace kk::render
