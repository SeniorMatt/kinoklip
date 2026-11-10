// SPDX-License-Identifier: GPL-3.0-or-later
// GlPaintEffects — 1.2 batch 4, painterly looks on the GPU (spec 57): Watercolor and Stylize.
// Watercolor uses the generalised Kuwahara filter (an old published method: of four quadrants
// around a pixel, take the mean colour of the calmest one), then pools pigment at edges and
// adds a paper grain. Stylize is relief, ink outline or cutout built from a blurred copy.
#include "core/VideoEffectKeys.h"
#include "render/GlShaderEffect.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

std::unique_ptr<GlEffect> makeWatercolor()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"watercolor", R"(
uniform sampler2D uTex;
uniform vec4 uP;   // brush radius px, edge darkening, paper, saturation
uniform float uScale;
float vnoise(vec2 p, uint s)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash2(ivec2(i), s), b = hash2(ivec2(i) + ivec2(1, 0), s);
    float c = hash2(ivec2(i) + ivec2(0, 1), s), d = hash2(ivec2(i) + ivec2(1, 1), s);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec2 sz = vec2(textureSize(uTex, 0));
    vec2 tx = 1.0 / sz;
    float R = clamp(uP.x, 1.0, 40.0);
    int n = int(min(R, 7.0));
    float step_ = R / float(n);
    vec3 best = straight(t);
    float bestVar = 1e9;
    for (int q = 0; q < 4; ++q) {
        vec2 sg = vec2((q & 1) == 0 ? 1.0 : -1.0, (q & 2) == 0 ? 1.0 : -1.0);
        vec3 sum = vec3(0.0);
        float sum2 = 0.0;
        float cnt = 0.0;
        for (int j = 0; j <= n; ++j) {
            for (int i = 0; i <= n; ++i) {
                vec3 s = straight(texture(uTex, vUv + sg * vec2(i, j) * step_ * tx));
                sum += s;
                float l = luma(s);
                sum2 += l * l;
                cnt += 1.0;
            }
        }
        vec3 mean = sum / cnt;
        float ml = luma(mean);
        float v = sum2 / cnt - ml * ml;
        if (v < bestVar) { bestVar = v; best = mean; }
    }
    vec3 c = best;
    // Pigment pools at edges of the original picture: darker and more saturated there.
    float e = max(1.0, 0.5 * R);
    float gx = luma(straight(texture(uTex, vUv + vec2(e, 0.0) * tx)))
        - luma(straight(texture(uTex, vUv - vec2(e, 0.0) * tx)));
    float gy = luma(straight(texture(uTex, vUv + vec2(0.0, e) * tx)))
        - luma(straight(texture(uTex, vUv - vec2(0.0, e) * tx)));
    float ed = uP.y * smoothstep(0.04, 0.25, length(vec2(gx, gy)));
    c *= mix(vec3(1.0), c, ed);
    float y = luma(c);
    c = mix(vec3(y), c, 1.0 + uP.w);
    // Paper grain: fine fibres plus broad blotches, fixed to the picture.
    vec2 px = vUv * sz;
    float grain = (vnoise(px / (1.5 * uScale), 3u) - 0.5) * 0.14
        + (vnoise(px / (9.0 * uScale), 5u) - 0.5) * 0.10;
    c = c * (1.0 + grain * uP.z * 2.0) + vec3(0.04 * uP.z);
    fragColor = vec4(clamp(c, 0.0, 1.0) * t.a, t.a);
}
)",
        {"uP", "uScale"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            kit.gl()->glUniform4f(loc[0], float(pv(in, k::Radius, 5) * in.scale),
                float(pv(in, k::EdgeDarkening, 35) / 100.0), float(pv(in, k::Paper, 30) / 100.0),
                float(pv(in, k::Saturation, 15) / 100.0));
            kit.gl()->glUniform1f(loc[1], float(in.scale));
        },
        nullptr, nullptr});
}

std::unique_ptr<GlEffect> makeStylize()
{
    return std::make_unique<ShaderEffect>(ShaderEffect::Spec {"stylize", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;  // softened copy
uniform vec4 uP;   // style (0 relief, 1 ink, 2 cutout), amount, light angle (radians), levels
uniform float uStep;  // sample distance px for the gradient
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec2 tx = uStep / vec2(textureSize(uTex, 0));
    vec3 c = straight(t);
    vec3 b = straight(texture(uSecond, vUv));
    float gx = luma(straight(texture(uSecond, vUv + vec2(tx.x, 0.0))))
        - luma(straight(texture(uSecond, vUv - vec2(tx.x, 0.0))));
    float gy = luma(straight(texture(uSecond, vUv + vec2(0.0, tx.y))))
        - luma(straight(texture(uSecond, vUv - vec2(0.0, tx.y))));
    vec3 o;
    if (uP.x < 0.5) {
        float lit = dot(vec2(gx, gy), vec2(cos(uP.z), sin(uP.z)));
        o = b * clamp(1.0 + 4.0 * lit, 0.0, 2.2);
    } else if (uP.x < 1.5) {
        vec3 post = floor(b * uP.w + 0.5) / uP.w;
        o = mix(post, vec3(0.0), smoothstep(0.04, 0.14, length(vec2(gx, gy))));
    } else {
        o = floor(b * uP.w + 0.5) / uP.w;
    }
    o = mix(c, o, uP.y);
    fragColor = vec4(clamp(o, 0.0, 1.0) * t.a, t.a);
}
)",
        {"uP", "uStep"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const QString style = choiceParam(*in.params, k::Style, fxchoice::Relief);
            const float s = style == fxchoice::Ink ? 1.f : (style == fxchoice::Cutout ? 2.f : 0.f);
            kit.gl()->glUniform4f(loc[0], s, float(pv(in, k::Amount, 100) / 100.0),
                float(pv(in, k::Angle, 135) * std::numbers::pi / 180.0),
                float(std::clamp(pv(in, k::Levels, 5), 2.0, 16.0)));
            kit.gl()->glUniform1f(loc[1], float(std::max(1.0, pv(in, k::Size, 2) * in.scale)));
        },
        [](const ParamSet& p, int64_t f) { return p.value(k::Amount, f, 100) <= 0.0; },
        [](const ParamSet& p, int64_t f, double s) {
            return std::max(kMinRadiusPx, p.value(k::Size, f, 2) * s) * kSigmaPerRadius;
        }});
}

const GlEffectFactory kTable[] = {
    {fxid::Watercolor, &makeWatercolor},
    {fxid::Stylize, &makeStylize},
};

} // namespace

std::span<const GlEffectFactory> paintGlEffects()
{
    return kTable;
}

} // namespace kk::render
