// SPDX-License-Identifier: GPL-3.0-or-later
// GlTransitions — the built-in transitions as single GLSL passes, and their registration table.
#include "render/GlTransitions.h"

#include <QColor>

#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

const QString kDirection = QStringLiteral("direction");
const QString kSoftness = QStringLiteral("softness");
const QString kAngle = QStringLiteral("angle");
const QString kColor = QStringLiteral("color");

/// A transition whose whole behaviour is one fragment shader with uniforms uA, uB, uProgress
/// plus extra uniforms set by `setExtra`.
class ShaderTransition : public GlTransition
{
public:
    using SetExtra = void (*)(GlKit& kit, const GlProgram& program, const TransitionInput& in);

    ShaderTransition(const char* name, const char* fragment, SetExtra setExtra)
        : m_name(name)
        , m_fragment(fragment)
        , m_setExtra(setExtra)
    { }

    bool initialize(GlKit& kit, QString* why) override
    {
        if (!m_program.build(kit.gl(), m_name, m_fragment, why))
            return false;
        GlFunctions* gl = kit.gl();
        gl->glUseProgram(m_program.id);
        gl->glUniform1i(m_program.uniform(gl, "uA"), 0);
        gl->glUniform1i(m_program.uniform(gl, "uB"), 1);
        m_progress = m_program.uniform(gl, "uProgress");
        gl->glUseProgram(0);
        return true;
    }
    void destroy(GlKit& kit) override { m_program.destroy(kit.gl()); }
    void draw(GlKit& kit, const TransitionInput& in) override
    {
        GlFunctions* gl = kit.gl();
        gl->glUseProgram(m_program.id);
        kit.setFullscreen(m_program);
        gl->glUniform1f(m_progress, float(in.progress));
        if (m_setExtra)
            m_setExtra(kit, m_program, in);
        kit.bindTexture(0, in.a);
        kit.bindTexture(1, in.b);
        kit.drawQuad();
    }

private:
    const char* m_name;
    const char* m_fragment;
    SetExtra m_setExtra;
    GlProgram m_program;
    GLint m_progress = -1;
};

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
void main()
{
    vec2 p = (vUv - 0.5) * vec2(uAspect, 1.0);
    float extent = 0.5 * (abs(uDir.x) * uAspect + abs(uDir.y));
    float s = 0.5 + dot(p, uDir) / (2.0 * extent);
    float w = max(uSoftness, 1e-4);
    float edge = uProgress * (1.0 + w);
    float b = 1.0 - smoothstep(edge - w, edge, s);
    fragColor = mix(texture(uA, vUv), texture(uB, vUv), b);
}
)";

// A moves out by d·t, B follows from the opposite side: screen point p shows A(p − d·t) and
// B(p − d·(t − 1)); samples outside the picture are transparent.
const char* const kPush = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uProgress;
uniform vec2 uDir;
vec4 inside(sampler2D t, vec2 uv)
{
    vec2 ok = step(vec2(0.0), uv) * step(uv, vec2(1.0));
    return texture(t, uv) * (ok.x * ok.y);
}
void main()
{
    fragColor = inside(uA, vUv - uDir * uProgress) + inside(uB, vUv - uDir * (uProgress - 1.0));
}
)";

// ---- Uniform setters ----

QString choice(const ParamSet& p, const QString& key)
{
    const auto it = p.statics.constFind(key);
    return it == p.statics.constEnd() ? QString() : it->toString();
}

/// Travel direction: optional numeric "angle" (degrees, 0 = towards the right, clockwise)
/// overrides the "direction" choice.
QPointF travelDirection(const TransitionInput& in)
{
    if (in.params->values.contains(kAngle)) {
        const double a = in.params->value(kAngle, in.localFrame, 0.0) * std::numbers::pi / 180.0;
        return QPointF(std::cos(a), std::sin(a));
    }
    return directionVector(choice(*in.params, kDirection));
}

void setDip(GlKit& kit, const GlProgram& program, const TransitionInput& in)
{
    const auto it = in.params->statics.constFind(kColor);
    const QColor c = it != in.params->statics.constEnd() ? it->value<QColor>() : QColor(Qt::black);
    const float a = float(c.alphaF());
    kit.gl()->glUniform4f(program.uniform(kit.gl(), "uColor"), float(c.redF()) * a,
        float(c.greenF()) * a, float(c.blueF()) * a, a);
}

void setWipe(GlKit& kit, const GlProgram& program, const TransitionInput& in)
{
    GlFunctions* gl = kit.gl();
    const QPointF d = travelDirection(in);
    gl->glUniform2f(program.uniform(gl, "uDir"), float(d.x()), float(d.y()));
    gl->glUniform1f(program.uniform(gl, "uSoftness"),
        float(std::clamp(in.params->value(kSoftness, in.localFrame, 10.0), 0.0, 100.0) / 100.0));
    gl->glUniform1f(
        program.uniform(gl, "uAspect"), float(in.size.width()) / float(in.size.height()));
}

void setPush(GlKit& kit, const GlProgram& program, const TransitionInput& in)
{
    const QPointF d = travelDirection(in);
    kit.gl()->glUniform2f(program.uniform(kit.gl(), "uDir"), float(d.x()), float(d.y()));
}

std::unique_ptr<GlTransition> makeDissolve()
{
    return std::make_unique<ShaderTransition>("cross_dissolve", kDissolve, nullptr);
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

/// THE registration table: descriptor id → implementation. Ids must match BuiltinEffects.cpp.
const GlTransitionFactory kTransitionTable[] = {
    {"cross_dissolve", &makeDissolve},
    {"dip_to_color", &makeDip},
    {"wipe", &makeWipe},
    {"push", &makePush},
};

} // namespace

std::span<const GlTransitionFactory> builtinGlTransitions()
{
    return kTransitionTable;
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
