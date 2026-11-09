// SPDX-License-Identifier: GPL-3.0-or-later
// GlDissolveTransitions — the Dissolve folder beyond Cross Dissolve and Dip to Color:
// Additive Dissolve, Non-Additive Dissolve, Blur Dissolve (one shader, `uKind`) and Smooth Cut
// (optical-flow morph through the GlRetime interpolator).
#include "core/TransitionKeys.h"
#include "render/GlRetime.h"
#include "render/GlShaderTransition.h"

#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

namespace k = tkeys;

// t = share of B after the start / end ratio. Kinds: 0 additive (each picture at full strength
// from its half on, so the middle is the sum), 1 non-additive (the brighter of the two per
// channel), 2 blur (both pictures blurred by a disc that is widest in the middle).
const char* const kDissolveFamily = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uProgress;
uniform int uKind;
uniform float uStart;   // 0..1
uniform float uEnd;     // 0..1
uniform vec2 uBlur;     // largest blur radius in uv units
vec4 blurred(sampler2D tex, float radius)
{
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 32; ++i) {
        float r = sqrt((float(i) + 0.5) / 32.0);
        float a = float(i) * 2.399963; // golden angle: an even spread of taps over the disc
        sum += texture(tex, vUv + vec2(cos(a), sin(a)) * r * radius * uBlur);
    }
    return sum / 32.0;
}
void main()
{
    float t = mix(uStart, uEnd, uProgress);
    vec4 a;
    vec4 b;
    if (uKind == 2) {
        float radius = sin(3.14159265 * uProgress);
        a = blurred(uA, radius);
        b = blurred(uB, radius);
        fragColor = mix(a, b, t);
        return;
    }
    a = texture(uA, vUv) * min(1.0, 2.0 * (1.0 - t));
    b = texture(uB, vUv) * min(1.0, 2.0 * t);
    fragColor = uKind == 0 ? min(a + b, vec4(1.0)) : max(a, b);
}
)";

enum Kind {
    Additive = 0,
    NonAdditive = 1,
    Blur = 2
};

std::unique_ptr<GlTransition> makeFamily(const char* id, Kind kind)
{
    return std::make_unique<ShaderTransition>(id, kDissolveFamily,
        [kind](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
            setInt(kit, program, "uKind", kind);
            setFloat(kit, program, "uStart",
                std::clamp(numberParam(in, k::StartRatio, 0), 0.0, 100.0) / 100.0);
            setFloat(kit, program, "uEnd",
                std::clamp(numberParam(in, k::EndRatio, 100), 0.0, 100.0) / 100.0);
            setVec2(kit, program, "uBlur",
                std::clamp(numberParam(in, k::BlurH, 4), 0.0, 30.0) / 100.0,
                std::clamp(numberParam(in, k::BlurV, 4), 0.0, 30.0) / 100.0);
        });
}

/// Smooth Cut: the picture between A and B at the transition's progress, found by the optical
/// flow of GlRetime (the same interpolator as Retime Process > Optical Flow) between the
/// outgoing and the incoming frame of this very frame. A thing that moved between the two
/// shows up between its two places and not at both. Without the interpolator (it failed to
/// build) or when one side is missing (a fade at a track end) it is a plain dissolve.
class SmoothCut : public GlTransition
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        m_dissolve = std::make_unique<ShaderTransition>("smooth_cut_dissolve",
            "uniform sampler2D uA;\nuniform sampler2D uB;\nuniform float uProgress;\n"
            "void main() { fragColor = mix(texture(uA, vUv), texture(uB, vUv), uProgress); }\n",
            nullptr);
        return m_dissolve->initialize(kit, why);
    }
    void destroy(GlKit& kit) override { m_dissolve->destroy(kit); }
    void draw(GlKit& kit, const TransitionInput& in) override
    {
        const GLuint none = kit.transparentTexture();
        if (!in.retime || !in.target || in.a == none || in.b == none) {
            m_dissolve->draw(kit, in);
            return;
        }
        RenderTarget a;
        a.texture = in.a;
        a.size = in.size;
        a.format = GL_RGBA16F;
        RenderTarget b = a;
        b.texture = in.b;
        PooledTarget morph = in.retime->interpolate(kit, a, b, float(in.progress));
        kit.bindTarget(*in.target);
        kit.setBlendOver(true);
        kit.blit(morph->texture, GlKit::quadToNdc(QRectF(QPointF(0, 0), in.size), in.size));
    }

private:
    std::unique_ptr<ShaderTransition> m_dissolve;
};

} // namespace

void appendDissolveTransitions(std::vector<GlTransitionFactory>& out)
{
    out.push_back({QStringLiteral("additive_dissolve"),
        [] { return makeFamily("additive_dissolve", Additive); }});
    out.push_back({QStringLiteral("non_additive_dissolve"),
        [] { return makeFamily("non_additive_dissolve", NonAdditive); }});
    out.push_back(
        {QStringLiteral("blur_dissolve"), [] { return makeFamily("blur_dissolve", Blur); }});
    out.push_back({QStringLiteral("smooth_cut"), [] { return std::make_unique<SmoothCut>(); }});
}

} // namespace kk::render
