// SPDX-License-Identifier: GPL-3.0-or-later
// GlOpticsEffects — 1.2 batch 4, lens artefacts on the GPU (spec 57): Aperture Diffraction
// (starburst spikes from bright lights) and Lens Reflections (dim coloured copies of bright
// lights mirrored through the lens centre). Both add light to the picture: the added colour is
// premultiplied and the alpha rises with it, so it also shows over transparent areas.
#include "core/VideoEffectKeys.h"
#include "render/GlShaderEffect.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

/// GLSL to finish both effects: premultiplied picture @c t plus added light @c add.
constexpr char kAddLight[] = R"(
vec4 addLight(vec4 t, vec3 add)
{
    float a = clamp(t.a + max(add.r, max(add.g, add.b)), 0.0, 1.0);
    vec3 rgb = t.rgb + add;
    return vec4(min(rgb, vec3(a)), a);
}
)";

std::unique_ptr<GlEffect> makeAperture()
{
    static const std::string source = std::string(kAddLight) + R"(
uniform sampler2D uTex;
uniform vec4 uP;   // spikes, length px, threshold, intensity
uniform vec4 uQ;   // tint rgb, rotation (radians)
void main()
{
    vec4 t = texture(uTex, vUv);
    vec2 tx = 1.0 / vec2(textureSize(uTex, 0));
    const int S = 40;
    int n = int(uP.x);
    vec3 acc = vec3(0.0);
    float scale = 1.0 / max(1.0 - uP.z, 0.02);
    for (int i = 0; i < n; ++i) {
        float a = uQ.w + 6.2831853 * float(i) / float(n);
        vec2 dir = vec2(cos(a), sin(a));
        for (int s = 1; s <= S; ++s) {
            float u = float(s) / float(S);
            // Light at p - dir * d spreads along +dir: the spike has its brightest part next to
            // the light and fades to nothing at the full length.
            vec3 src = straight(texture(uTex, vUv - dir * (u * uP.y) * tx));
            vec3 bright = max(src - uP.z, vec3(0.0)) * scale;
            float w = (1.0 - u) * (1.0 - u);
            acc += bright * w;
        }
    }
    // Scaled by the sample spacing, so a light of a given size gives the same spike length-wise.
    acc *= uP.w * 0.1 * (uP.y / float(S));
    fragColor = addLight(t, acc * uQ.rgb);
}
)";
    return std::make_unique<ShaderEffect>(
        ShaderEffect::Spec {"aperture_diffraction", source.c_str(), {"uP", "uQ"},
            [](GlKit& kit, const GLint* loc, const EffectInput& in) {
                const fxmath::Rgb tint = colorParam(*in.params, k::Color, {1.f, 1.f, 1.f});
                kit.gl()->glUniform4f(loc[0], float(std::clamp(pv(in, k::Points, 6), 2.0, 16.0)),
                    float(std::max(1.0, pv(in, k::Length, 120) * in.scale)),
                    float(std::clamp(pv(in, k::Threshold, 80) / 100.0, 0.0, 0.99)),
                    float(pv(in, k::Intensity, 100) / 100.0));
                kit.gl()->glUniform4f(loc[1], tint.r, tint.g, tint.b,
                    float(pv(in, k::Rotation, 0) * std::numbers::pi / 180.0));
            },
            [](const ParamSet& p, int64_t f) { return p.value(k::Intensity, f, 100) <= 0.0; },
            nullptr});
}

std::unique_ptr<GlEffect> makeLensReflections()
{
    static const std::string source = std::string(kAddLight) + R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;  // softened copy: the reflections are out of focus
uniform vec4 uP;   // reflections, threshold, intensity, spread
uniform vec4 uQ;   // tint rgb
uniform vec2 uC;   // lens centre, uv
void main()
{
    vec4 t = texture(uTex, vUv);
    int n = int(uP.x);
    float scale = 1.0 / max(1.0 - uP.y, 0.02);
    vec3 acc = vec3(0.0);
    for (int k = 1; k <= 8; ++k) {
        if (k > n)
            break;
        // A light at L is reflected to C - (L - C) * f: this pixel looks back at that light.
        float f = max(uP.w * float(k) / float(n), 0.05);
        vec2 src = uC - (vUv - uC) / f;
        if (src.x < 0.0 || src.x > 1.0 || src.y < 0.0 || src.y > 1.0)
            continue;
        // Only a light counts: a spot brighter than the threshold AND brighter than its
        // surroundings (a broad bright area is not a light), faded out toward the frame edges so
        // the frame border never shows up in a reflection.
        vec3 s = straight(texture(uSecond, src));
        float l = luma(s);
        float around = 0.0;
        for (int i = 0; i < 8; ++i) {
            float a = 0.7853982 * float(i);
            around += luma(straight(texture(uSecond, src + 0.06 * vec2(cos(a), sin(a)))));
        }
        around /= 8.0;
        vec2 e = smoothstep(vec2(0.0), vec2(0.1), min(src, 1.0 - src));
        float h = max(l - max(uP.y, around + 0.1), 0.0) * scale * e.x * e.y;
        vec3 bright = s * (h / max(l, 1e-3));
        vec3 hue = 0.5 + 0.5 * cos(6.2831853 * (vec3(0.0, 0.33, 0.67) + float(k) * 0.17));
        acc += bright * mix(vec3(1.0), hue, 0.6) / (1.0 + 0.5 * float(k));
    }
    fragColor = addLight(t, acc * uQ.rgb * uP.z);
}
)";
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"lens_reflections", source.c_str(),
        {"uP", "uQ", "uC"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const fxmath::Rgb tint = colorParam(*in.params, k::Color, {1.f, 0.85f, 0.69f});
            kit.gl()->glUniform4f(loc[0], float(std::clamp(pv(in, k::Ghosts, 4), 1.0, 8.0)),
                float(std::clamp(pv(in, k::Threshold, 70) / 100.0, 0.0, 0.99)),
                float(pv(in, k::Intensity, 80) / 100.0), float(pv(in, k::Distance, 100) / 100.0));
            kit.gl()->glUniform4f(loc[1], tint.r, tint.g, tint.b, 1.f);
            const double w = in.layer->size.width(), h = in.layer->size.height();
            kit.gl()->glUniform2f(loc[2], float(0.5 + pv(in, k::CenterX, 0) * in.scale / w),
                float(0.5 + pv(in, k::CenterY, 0) * in.scale / h));
        },
        [](const ParamSet& p, int64_t f) { return p.value(k::Intensity, f, 80) <= 0.0; },
        [](const ParamSet& p, int64_t f, double s) {
            return std::max(kMinRadiusPx, p.value(k::Softness, f, 12) * s) * kSigmaPerRadius;
        }});
}

const GlEffectFactory kTable[] = {
    {fxid::ApertureDiffraction, &makeAperture},
    {fxid::LensReflections, &makeLensReflections},
};

} // namespace

std::span<const GlEffectFactory> opticsGlEffects()
{
    return kTable;
}

} // namespace kk::render
