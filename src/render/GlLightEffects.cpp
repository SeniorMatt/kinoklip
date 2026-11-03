// SPDX-License-Identifier: GPL-3.0-or-later
// GlLightEffects — Light Rays, Bloom, Halation and Lens Flare on the GPU.
//
// Light Rays, Bloom and Halation share a bright pass (the premultiplied picture where its
// luma is above a soft threshold) and an additive combine that, like Glow, adds coverage
// outside the clip's alpha. Costs: rays = bright pass + at most 2 x 24 radial taps at half
// resolution + combine;
// bloom = bright pass + 3 Gaussian blurs (bounded, GlKit) + combine; halation = bright pass +
// 1 Gaussian blur + combine; lens flare = one procedural pass (no textures, no assets).
#include "core/VideoEffectKeys.h"
#include "render/GlEffectBase.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

double val(const ParamSet& p, const QString& key, int64_t f, double fallback)
{
    return p.value(key, f, fallback);
}

/// Taps of one radial (rays) pass; more spacing than kMaxTapSpacingPx uses two passes.
constexpr int kRayTaps = 24;
constexpr double kMaxTapSpacingPx = 1.5;
/// Longest rays (fraction of the distance to the centre).
constexpr double kMaxRayLength = 0.95;

/// Bright pass: premultiplied colour × smoothstep(threshold ± knee) of its luma.
const char* const kBrightFragment = R"(
uniform sampler2D uTex;
uniform vec2 uThreshold; // threshold, knee (> 0)
void main()
{
    vec4 t = texture(uTex, vUv);
    fragColor = t * smoothstep(uThreshold.x - uThreshold.y, uThreshold.x + uThreshold.y,
                               luma(straight(t)));
}
)";

/// Additive combine: uSecond / uThird / uFourth (light layers, weights uWeights) tinted by
/// uTint, saturation uSaturation; uMode 1 = halation (the light is kept off the bright core
/// in uFourth).
const char* const kCombineFragment = R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform sampler2D uThird;
uniform sampler2D uFourth;
uniform vec3 uWeights;
uniform vec3 uTint;
uniform float uSaturation;
uniform int uMode;
void main()
{
    vec4 s = texture(uTex, vUv);
    vec3 g;
    if (uMode == 1) {
        // Halation: only the halo's brightness, tinted, and not on the highlight itself.
        float halo = luma(texture(uSecond, vUv).rgb);
        float core = luma(straight(texture(uFourth, vUv)));
        g = uTint * halo * (1.0 - core);
    } else {
        vec3 light = texture(uSecond, vUv).rgb * uWeights.x + texture(uThird, vUv).rgb * uWeights.y
            + texture(uFourth, vUv).rgb * uWeights.z;
        g = max(mix(vec3(luma(light)), light, uSaturation), 0.0) * uTint;
    }
    float a = s.a + (1.0 - s.a) * clamp(max(g.r, max(g.g, g.b)), 0.0, 1.0);
    fragColor = vec4(min(s.rgb + g, vec3(a)), a);
}
)";

/// Radial accumulation towards uCentre (rays): the average of the picture along the segment
/// from the pixel towards the centre, scale exp(uLogScale · t).
const char* const kRaysFragment = R"(
uniform sampler2D uTex;
uniform vec2 uCentre;
uniform float uLogScale;
uniform int uTaps;
void main()
{
    vec4 sum = vec4(0.0);
    vec2 d = vUv - uCentre;
    for (int i = 0; i < uTaps; ++i) {
        float t = (float(i) + 0.5) / float(uTaps);
        sum += texture(uTex, uCentre + d * exp(uLogScale * t));
    }
    fragColor = sum / float(uTaps);
}
)";

/// Shared programs of the bright-pass light effects.
class LightBase : public GlEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        GlFunctions* gl = kit.gl();
        const std::string bright = std::string(kGlslCommon) + kBrightFragment;
        const std::string combine = std::string(kGlslCommon) + kCombineFragment;
        if (!m_bright.build(gl, "light_bright", bright.c_str(), why)
            || !m_combine.build(gl, "light_combine", combine.c_str(), why)
            || !m_rays.build(gl, "light_rays", kRaysFragment, why))
            return false;
        m_threshold = m_bright.uniform(gl, "uThreshold");
        m_weights = m_combine.uniform(gl, "uWeights");
        m_tint = m_combine.uniform(gl, "uTint");
        m_saturation = m_combine.uniform(gl, "uSaturation");
        m_mode = m_combine.uniform(gl, "uMode");
        m_rayCentre = m_rays.uniform(gl, "uCentre");
        m_rayLogScale = m_rays.uniform(gl, "uLogScale");
        m_rayTaps = m_rays.uniform(gl, "uTaps");
        gl->glUseProgram(m_bright.id);
        gl->glUniform1i(m_bright.uniform(gl, "uTex"), 0);
        gl->glUseProgram(m_rays.id);
        gl->glUniform1i(m_rays.uniform(gl, "uTex"), 0);
        gl->glUseProgram(m_combine.id);
        gl->glUniform1i(m_combine.uniform(gl, "uTex"), 0);
        gl->glUniform1i(m_combine.uniform(gl, "uSecond"), 1);
        gl->glUniform1i(m_combine.uniform(gl, "uThird"), 2);
        gl->glUniform1i(m_combine.uniform(gl, "uFourth"), 3);
        gl->glUseProgram(0);
        return true;
    }
    void destroy(GlKit& kit) override
    {
        m_bright.destroy(kit.gl());
        m_combine.destroy(kit.gl());
        m_rays.destroy(kit.gl());
    }

protected:
    /// Program @p program on a new target of @p size with @p src at unit 0.
    PooledTarget beginPass(GlKit& kit, const GlProgram& program, const RenderTarget& src)
    {
        PooledTarget out = kit.acquire(src.size, src.format);
        kit.bindTarget(*out);
        kit.setBlendOver(false);
        kit.gl()->glUseProgram(program.id);
        kit.setFullscreen(program);
        kit.bindTexture(0, src.texture);
        return out;
    }

    PooledTarget brightPass(GlKit& kit, const RenderTarget& src, double threshold, double knee)
    {
        PooledTarget out = beginPass(kit, m_bright, src);
        kit.gl()->glUniform2f(m_threshold, float(threshold), float(std::max(knee, 1e-3)));
        kit.drawQuad();
        return out;
    }

    /// Adds @p a (+ @p b, @p c with @p weights) to @p src.
    PooledTarget combine(GlKit& kit, const RenderTarget& src, const RenderTarget& a,
        const RenderTarget* b, const RenderTarget* c, const float weights[3],
        const fxmath::Rgb& tint, float saturation, int mode)
    {
        PooledTarget out = beginPass(kit, m_combine, src);
        GlFunctions* gl = kit.gl();
        gl->glUniform3f(m_weights, weights[0], weights[1], weights[2]);
        gl->glUniform3f(m_tint, tint.r, tint.g, tint.b);
        gl->glUniform1f(m_saturation, saturation);
        gl->glUniform1i(m_mode, mode);
        kit.bindTexture(1, a.texture);
        kit.bindTexture(2, b ? b->texture : a.texture);
        kit.bindTexture(3, c ? c->texture : a.texture);
        kit.drawQuad();
        kit.bindTexture(3, 0);
        kit.bindTexture(2, 0);
        gl->glActiveTexture(GL_TEXTURE0);
        return out;
    }

    /// Radial blur of @p src towards uv (@p u, @p v) over @p length (0..1) of the distance.
    PooledTarget rays(
        GlKit& kit, const RenderTarget& src, float u, float v, double length, double maxRadiusPx)
    {
        const double logScale = std::log(1.0 - std::clamp(length, 0.0, kMaxRayLength));
        auto pass = [&](const RenderTarget& from, double fraction) {
            PooledTarget out = beginPass(kit, m_rays, from);
            kit.gl()->glUniform2f(m_rayCentre, u, v);
            kit.gl()->glUniform1f(m_rayLogScale, float(logScale * fraction));
            kit.gl()->glUniform1i(m_rayTaps, kRayTaps);
            kit.drawQuad();
            return out;
        };
        // Same two-pass trick as the path blurs (GlBlurEffects.cpp): a short pass over
        // 1 / taps of the path, then the full path, together a continuous average.
        if (length * maxRadiusPx / kRayTaps > kMaxTapSpacingPx) {
            PooledTarget first = pass(src, 1.0 / kRayTaps);
            return pass(*first, 1.0);
        }
        return pass(src, 1.0);
    }

    GlProgram m_bright, m_combine, m_rays;
    GLint m_threshold = -1, m_weights = -1, m_tint = -1, m_saturation = -1, m_mode = -1,
          m_rayCentre = -1, m_rayLogScale = -1, m_rayTaps = -1;
};

// ---- light_rays ----

class LightRays final : public LightBase
{
public:
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Intensity, f, 100) <= 0.0 || val(p, k::Length, f, 40) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        PooledTarget bright = brightPass(kit, *in.layer, val(p, k::Threshold, f, 60) / 100.0, 0.1);
        const LayerGeometry g = layerGeometry(in);
        float u = 0, v = 0;
        g.centre(in, val(p, k::CenterX, f, 0), val(p, k::CenterY, f, 0), &u, &v);
        const double maxRadius = std::hypot(
            std::max(u, 1.f - u) * double(g.width), std::max(v, 1.f - v) * double(g.height));
        // Rays are soft: they are traced at half resolution (a quarter of the cost) and the
        // combine pass upsamples them with the bilinear filter.
        PooledTarget half = kit.resample(*bright,
            QSize(std::max(1, (bright->size.width() + 1) / 2),
                std::max(1, (bright->size.height() + 1) / 2)));
        PooledTarget light
            = rays(kit, *half, u, v, val(p, k::Length, f, 40) / 100.0, 0.5 * maxRadius);
        // Rays are an average along a long path: intensity 100 % doubles them so they read.
        const float intensity = float(2.0 * std::max(0.0, val(p, k::Intensity, f, 100)) / 100.0);
        fxmath::Rgb tint = colorParam(p, k::Color, {1.f, 0.96f, 0.84f});
        tint = {tint.r * intensity, tint.g * intensity, tint.b * intensity};
        const float weights[3] = {1.f, 0.f, 0.f};
        return combine(kit, *in.layer, *light, nullptr, nullptr, weights, tint, 1.f, 0);
    }
};

// ---- bloom ----

class Bloom final : public LightBase
{
public:
    double paddingPx(const ParamSet& p, int64_t f) const override
    {
        // The widest layer (4 x radius sigma) fades over about 3 x radius.
        return 3.0 * val(p, k::Radius, f, 30) + 1.0;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Intensity, f, 80) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        PooledTarget bright = brightPass(kit, *in.layer, val(p, k::Threshold, f, 60) / 100.0, 0.2);
        const double sigma = val(p, k::Radius, f, 30) * in.scale * kSigmaPerRadius;
        // Three octaves: tight, medium and wide light (a smooth, long-tailed falloff).
        PooledTarget a = kit.gaussianBlur(*bright, sigma * 0.5);
        PooledTarget b = kit.gaussianBlur(*bright, sigma);
        PooledTarget c = kit.gaussianBlur(*bright, sigma * 2.0);
        const RenderTarget& la = a ? *a : *bright;
        const RenderTarget& lb = b ? *b : la;
        const RenderTarget& lc = c ? *c : lb;
        const float intensity = float(std::max(0.0, val(p, k::Intensity, f, 80)) / 100.0);
        const float weights[3] = {0.5f * intensity, 0.35f * intensity, 0.35f * intensity};
        return combine(kit, *in.layer, la, &lb, &lc, weights, {1.f, 1.f, 1.f},
            float(std::max(0.0, val(p, k::Saturation, f, 100)) / 100.0), 0);
    }
};

// ---- halation ----

class Halation final : public LightBase
{
public:
    double paddingPx(const ParamSet& p, int64_t f) const override
    {
        return 3.0 * kSigmaPerRadius * val(p, k::Radius, f, 8) + 1.0;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        PooledTarget bright = brightPass(kit, *in.layer, val(p, k::Threshold, f, 65) / 100.0, 0.1);
        const double sigma = val(p, k::Radius, f, 8) * in.scale * kSigmaPerRadius;
        PooledTarget halo = kit.gaussianBlur(*bright, std::max(sigma, 0.5));
        const RenderTarget& h = halo ? *halo : *bright;
        const float amount = float(std::max(0.0, val(p, k::Amount, f, 100)) / 100.0);
        fxmath::Rgb tint = colorParam(p, k::Color, {1.f, 0.28f, 0.1f});
        tint = {tint.r * amount, tint.g * amount, tint.b * amount};
        const float weights[3] = {1.f, 0.f, 0.f};
        return combine(kit, *in.layer, h, nullptr, bright.get(), weights, tint, 1.f, 1);
    }
};

// ---- lens_flare ----

class LensFlare final : public PassEffect
{
public:
    static constexpr int kGhosts = 5;

    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "lens_flare", R"(
uniform sampler2D uTex;
uniform vec2 uSize;      // layer px
uniform vec2 uLight;     // light position, layer px
uniform vec2 uAxis;      // frame centre, layer px (ghosts lie on the line light -> centre)
uniform float uFlare;    // size, px
uniform float uStreaks;  // streak count (0 = none)
uniform float uGhosts;   // ghost brightness
uniform vec3 uColor;     // colour x intensity
const int kGhosts = 5;
const float kGhostPos[kGhosts] = float[](0.45, 0.8, 1.15, 1.5, -0.35);
const float kGhostSize[kGhosts] = float[](0.25, 0.12, 0.4, 0.18, 0.15);
const vec3 kGhostTint[kGhosts] = vec3[](vec3(0.4, 0.8, 1.0), vec3(1.0, 0.6, 0.3),
    vec3(0.5, 1.0, 0.6), vec3(0.9, 0.5, 1.0), vec3(1.0, 0.9, 0.5));
void main()
{
    vec2 p = vUv * uSize;
    vec2 d = p - uLight;
    float r = length(d);
    float s = max(uFlare, 1.0);
    // Core: a bright centre with a long soft tail.
    float core = exp(-pow(r / (0.18 * s), 2.0)) + 0.35 * exp(-r / (0.6 * s));
    // Streaks: thin rays, uStreaks of them, fading with distance.
    float streak = 0.0;
    if (uStreaks > 0.5) {
        float a = atan(d.y, d.x);
        streak = pow(max(cos(uStreaks * a), 0.0), 60.0) * exp(-r / (2.5 * s)) * 0.8;
    }
    // Halo ring with a slight rainbow (red outside, blue inside).
    vec3 ring = vec3(exp(-pow((r - 1.55 * s) / (0.06 * s), 2.0)),
                     exp(-pow((r - 1.5 * s) / (0.06 * s), 2.0)),
                     exp(-pow((r - 1.45 * s) / (0.06 * s), 2.0))) * 0.18;
    vec3 light = uColor * (core + streak) + ring * uColor;
    // Ghosts: soft discs mirrored through the frame centre.
    vec2 axis = uAxis - uLight;
    for (int i = 0; i < kGhosts; ++i) {
        vec2 g = uLight + axis * (1.0 + kGhostPos[i]);
        float gr = kGhostSize[i] * s;
        float disc = 1.0 - smoothstep(gr * 0.75, gr, length(p - g));
        light += kGhostTint[i] * uColor * disc * 0.12 * uGhosts;
    }
    vec4 src = texture(uTex, vUv);
    float a = src.a + (1.0 - src.a) * clamp(max(light.r, max(light.g, light.b)), 0.0, 1.0);
    fragColor = vec4(min(src.rgb + light, vec3(a)), a);
}
)",
                why))
            return false;
        m_size = uniform(kit, "uSize");
        m_light = uniform(kit, "uLight");
        m_axis = uniform(kit, "uAxis");
        m_flare = uniform(kit, "uFlare");
        m_streaks = uniform(kit, "uStreaks");
        m_ghosts = uniform(kit, "uGhosts");
        m_color = uniform(kit, "uColor");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Intensity, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        const LayerGeometry g = layerGeometry(in);
        float lu = 0, lv = 0, cu = 0, cv = 0;
        g.centre(in, val(p, k::CenterX, f, -400), val(p, k::CenterY, f, -220), &lu, &lv);
        g.centre(in, 0, 0, &cu, &cv);
        const float intensity = float(std::max(0.0, val(p, k::Intensity, f, 100)) / 100.0);
        const fxmath::Rgb c = colorParam(p, k::Color, {1.f, 0.86f, 0.67f});
        GlFunctions* gl = kit.gl();
        PooledTarget out = begin(kit, *in.layer);
        gl->glUniform2f(m_size, g.width, g.height);
        gl->glUniform2f(m_light, lu * g.width, lv * g.height);
        gl->glUniform2f(m_axis, cu * g.width, cv * g.height);
        gl->glUniform1f(m_flare, float(val(p, k::Size, f, 160) * in.scale));
        gl->glUniform1f(m_streaks, float(std::round(val(p, k::Streaks, f, 6))));
        gl->glUniform1f(m_ghosts, float(std::max(0.0, val(p, k::Ghosts, f, 100)) / 100.0));
        gl->glUniform3f(m_color, c.r * intensity, c.g * intensity, c.b * intensity);
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_size = -1, m_light = -1, m_axis = -1, m_flare = -1, m_streaks = -1, m_ghosts = -1,
          m_color = -1;
};

const GlEffectFactory kLightTable[] = {
    {fxid::LightRays, &make<LightRays>},
    {fxid::Bloom, &make<Bloom>},
    {fxid::Halation, &make<Halation>},
    {fxid::LensFlare, &make<LensFlare>},
};

} // namespace

std::span<const GlEffectFactory> lightGlEffects()
{
    return kLightTable;
}

} // namespace kk::render
