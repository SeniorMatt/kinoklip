// SPDX-License-Identifier: GPL-3.0-or-later
// GlStylizeEffects — Glow, Film Grain, Chromatic Aberration, Edge Detect, Emboss and Stroke
// on the GPU. Costs: glow = 2 passes + a Gaussian blur (bounded, see GlKit); stroke = the
// separable morphology (2 × width taps per pixel) + 1 pass; the others are one pass.
#include "core/VideoEffectKeys.h"
#include "render/GlEffectBase.h"

#include <QColor>

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

// ---- glow ----

class Glow final : public GlEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        GlFunctions* gl = kit.gl();
        // Bright pass: the premultiplied picture where its luma is above the threshold (a
        // soft knee of ±0.1 around it).
        if (!m_bright.build(gl, "glow_bright",
                (std::string(kGlslCommon) + R"(
uniform sampler2D uTex;
uniform float uThreshold;
void main()
{
    vec4 t = texture(uTex, vUv);
    fragColor = t * smoothstep(uThreshold - 0.1, uThreshold + 0.1, luma(straight(t)));
}
)")
                    .c_str(),
                why))
            return false;
        // Combine: add the tinted, blurred light; outside the clip's alpha it adds coverage.
        if (!m_combine.build(gl, "glow_combine", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform vec3 uTint; // colour × intensity
void main()
{
    vec4 s = texture(uTex, vUv);
    vec3 g = texture(uSecond, vUv).rgb * uTint;
    float a = s.a + (1.0 - s.a) * clamp(max(g.r, max(g.g, g.b)), 0.0, 1.0);
    fragColor = vec4(min(s.rgb + g, vec3(a)), a);
}
)",
                why))
            return false;
        m_threshold = m_bright.uniform(gl, "uThreshold");
        m_tint = m_combine.uniform(gl, "uTint");
        gl->glUseProgram(m_bright.id);
        gl->glUniform1i(m_bright.uniform(gl, "uTex"), 0);
        gl->glUseProgram(m_combine.id);
        gl->glUniform1i(m_combine.uniform(gl, "uTex"), 0);
        gl->glUniform1i(m_combine.uniform(gl, "uSecond"), 1);
        gl->glUseProgram(0);
        return true;
    }
    void destroy(GlKit& kit) override
    {
        m_bright.destroy(kit.gl());
        m_combine.destroy(kit.gl());
    }
    double paddingPx(const ParamSet& p, int64_t f) const override
    {
        return 3.0 * kSigmaPerRadius * val(p, k::Radius, f, 20) + 1.0; // visible extent 3σ
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Intensity, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        GlFunctions* gl = kit.gl();
        PooledTarget bright = kit.acquire(in.layer->size, in.layer->format);
        kit.bindTarget(*bright);
        kit.setBlendOver(false);
        gl->glUseProgram(m_bright.id);
        kit.setFullscreen(m_bright);
        gl->glUniform1f(m_threshold, float(val(p, k::Threshold, in.localFrame, 70) / 100.0));
        kit.bindTexture(0, in.layer->texture);
        kit.drawQuad();
        const double sigma = val(p, k::Radius, in.localFrame, 20) * in.scale * kSigmaPerRadius;
        PooledTarget blurred = kit.gaussianBlur(*bright, sigma);
        const RenderTarget& light = blurred ? *blurred : *bright;

        const fxmath::Rgb tint = colorParam(p, k::Color, {1.f, 1.f, 1.f});
        const float intensity
            = float(std::max(0.0, val(p, k::Intensity, in.localFrame, 100)) / 100.0);
        PooledTarget out = kit.acquire(in.layer->size, in.layer->format);
        kit.bindTarget(*out);
        kit.setBlendOver(false);
        gl->glUseProgram(m_combine.id);
        kit.setFullscreen(m_combine);
        gl->glUniform3f(m_tint, tint.r * intensity, tint.g * intensity, tint.b * intensity);
        kit.bindTexture(0, in.layer->texture);
        kit.bindTexture(1, light.texture);
        kit.drawQuad();
        return out;
    }

private:
    GlProgram m_bright, m_combine;
    GLint m_threshold = -1, m_tint = -1;
};

// ---- film_grain ----

// Value noise from an integer hash (the twin of fxmath::grainNoise()), added to the straight
// colour. Pixel (x, y) = floor(gl_FragCoord.xy): framebuffer row 0 is the picture's top.
const char* const kGrainFragment = R"(
uniform sampler2D uTex;
uniform float uCell;    // grain size in layer pixels
uniform float uAmount;  // noise scale
uniform uvec3 uSeed;    // per channel
uniform int uMono;

uint hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float lattice(ivec2 p, uint seed)
{
    uint h = hash(uint(p.x) + hash(uint(p.y) + hash(seed)));
    return float(h & 0xffffffu) / 16777215.0 - 0.5;
}
float grain(vec2 px, uint seed)
{
    vec2 g = px / uCell;
    vec2 f = floor(g);
    ivec2 i = ivec2(f);
    vec2 t = g - f;
    t = t * t * (3.0 - 2.0 * t);
    float a = lattice(i, seed);
    float b = lattice(i + ivec2(1, 0), seed);
    float c = lattice(i + ivec2(0, 1), seed);
    float d = lattice(i + ivec2(1, 1), seed);
    return mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
}
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    vec2 px = floor(gl_FragCoord.xy);
    vec3 n;
    n.r = grain(px, uSeed.r);
    n.g = uMono == 1 ? n.r : grain(px, uSeed.g);
    n.b = uMono == 1 ? n.r : grain(px, uSeed.b);
    fragColor = vec4(clamp(straight(t) + n * uAmount, 0.0, 1.0) * t.a, t.a);
}
)";

class FilmGrain final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "film_grain", kGrainFragment, why, true))
            return false;
        m_cell = uniform(kit, "uCell");
        m_amount = uniform(kit, "uAmount");
        m_seed = uniform(kit, "uSeed");
        m_mono = uniform(kit, "uMono");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 30) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        constexpr float kGrainScale = 0.4f; // noise scale at Amount 100 % (as fxmath)
        const ParamSet& p = *in.params;
        const bool animated = boolParam(p, k::Animated, true);
        const auto base = uint32_t(animated ? std::max<int64_t>(0, in.localFrame) * 3 : 0);
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform1f(
            m_cell, float(std::max(0.5, val(p, k::Size, in.localFrame, 1.5) * in.scale)));
        gl->glUniform1f(
            m_amount, float(val(p, k::Amount, in.localFrame, 30) / 100.0) * kGrainScale);
        gl->glUniform3ui(m_seed, base, base + 1, base + 2);
        gl->glUniform1i(m_mono, boolParam(p, k::Monochrome, true) ? 1 : 0);
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_cell = -1, m_amount = -1, m_seed = -1, m_mono = -1;
};

// ---- chromatic_aberration ----

class ChromaticAberration final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "chromatic_aberration", R"(
uniform sampler2D uTex;
uniform vec2 uCentre;
uniform float uK; // relative scale difference of the red / blue samples
void main()
{
    vec2 d = vUv - uCentre;
    vec4 r = texture(uTex, uCentre + d * (1.0 - uK));
    vec4 g = texture(uTex, vUv);
    vec4 b = texture(uTex, uCentre + d * (1.0 + uK));
    fragColor = vec4(r.r, g.g, b.b, max(g.a, max(r.a, b.a)));
}
)",
                why))
            return false;
        m_centre = uniform(kit, "uCentre");
        m_k = uniform(kit, "uK");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 5) == 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const LayerGeometry g = layerGeometry(in);
        float u = 0, v = 0;
        g.centre(in, 0, 0, &u, &v);
        // Amount = colour shift (timeline px) at the content corners.
        const double halfDiagonal
            = 0.5 * std::hypot((g.x1 - g.x0) * g.width, (g.y1 - g.y0) * g.height);
        const double kk
            = val(*in.params, k::Amount, in.localFrame, 5) * in.scale / std::max(1.0, halfDiagonal);
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform2f(m_centre, u, v);
        kit.gl()->glUniform1f(m_k, float(kk));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_centre = -1, m_k = -1;
};

// ---- edge_detect ----

class EdgeDetect final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "edge_detect", R"(
uniform sampler2D uTex;
uniform vec2 uTexel;
uniform float uStrength;
uniform float uAmount;
uniform int uInvert;
float L(float x, float y) { return luma(texture(uTex, vUv + vec2(x, y) * uTexel).rgb); }
void main()
{
    // Sobel gradient of the (premultiplied) luma: alpha edges count as edges too.
    float gx = -L(-1.0, -1.0) - 2.0 * L(-1.0, 0.0) - L(-1.0, 1.0)
             + L(1.0, -1.0) + 2.0 * L(1.0, 0.0) + L(1.0, 1.0);
    float gy = -L(-1.0, -1.0) - 2.0 * L(0.0, -1.0) - L(1.0, -1.0)
             + L(-1.0, 1.0) + 2.0 * L(0.0, 1.0) + L(1.0, 1.0);
    float e = clamp(length(vec2(gx, gy)) * uStrength, 0.0, 1.0);
    if (uInvert == 1)
        e = 1.0 - e;
    vec4 c = texture(uTex, vUv);
    fragColor = vec4(mix(c.rgb, vec3(e) * c.a, uAmount), c.a);
}
)",
                why, true))
            return false;
        m_texel = uniform(kit, "uTexel");
        m_strength = uniform(kit, "uStrength");
        m_amount = uniform(kit, "uAmount");
        m_invert = uniform(kit, "uInvert");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(
            m_texel, 1.0f / float(in.layer->size.width()), 1.0f / float(in.layer->size.height()));
        gl->glUniform1f(m_strength, float(val(p, k::Strength, in.localFrame, 100) / 100.0));
        gl->glUniform1f(
            m_amount, float(std::clamp(val(p, k::Amount, in.localFrame, 100) / 100.0, 0.0, 1.0)));
        gl->glUniform1i(m_invert, boolParam(p, k::Invert, false) ? 1 : 0);
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_texel = -1, m_strength = -1, m_amount = -1, m_invert = -1;
};

// ---- emboss ----

class Emboss final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "emboss", R"(
uniform sampler2D uTex;
uniform vec2 uOffset; // towards the light, in uv
uniform float uAmount;
void main()
{
    vec4 c = texture(uTex, vUv);
    float v = 0.5 + 2.0 * (luma(texture(uTex, vUv + uOffset).rgb) - luma(texture(uTex, vUv - uOffset).rgb));
    fragColor = vec4(mix(c.rgb, vec3(clamp(v, 0.0, 1.0)) * c.a, uAmount), c.a);
}
)",
                why, true))
            return false;
        m_offset = uniform(kit, "uOffset");
        m_amount = uniform(kit, "uAmount");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const double a = val(p, k::Angle, in.localFrame, 135) * std::numbers::pi / 180.0;
        const double h = std::max(0.5, val(p, k::Height, in.localFrame, 2) * in.scale);
        PooledTarget out = begin(kit, *in.layer);
        // Light direction (math convention, y up) → y-down uv.
        kit.gl()->glUniform2f(m_offset, float(std::cos(a) * h / in.layer->size.width()),
            float(-std::sin(a) * h / in.layer->size.height()));
        kit.gl()->glUniform1f(
            m_amount, float(std::clamp(val(p, k::Amount, in.localFrame, 100) / 100.0, 0.0, 1.0)));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_offset = -1, m_amount = -1;
};

// ---- stroke ----

class Stroke final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "stroke", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond; // dilated (outside) or eroded (inside) layer: alpha used
uniform vec4 uColor;       // straight colour, opacity in .a
uniform int uInside;
void main()
{
    vec4 s = texture(uTex, vUv);
    float m = texture(uSecond, vUv).a;
    if (uInside == 1) {
        float band = clamp(s.a - m, 0.0, 1.0) * uColor.a;
        fragColor = vec4(uColor.rgb * band, band) + s * (1.0 - band);
    } else {
        float k = m * uColor.a;
        fragColor = s + vec4(uColor.rgb * k, k) * (1.0 - s.a);
    }
}
)",
                why))
            return false;
        m_color = uniform(kit, "uColor");
        m_inside = uniform(kit, "uInside");
        return true;
    }
    double paddingPx(const ParamSet& p, int64_t f) const override
    {
        // Outside: room for the stroke. Inside: one transparent pixel so the erosion sees the
        // edge of a full-frame clip.
        return inside(p) ? 1.0 : val(p, k::Width, f, 4) + 1.0;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Width, f, 4) <= 0.0 || val(p, k::Opacity, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const bool in_ = inside(p);
        const double width = val(p, k::Width, in.localFrame, 4) * in.scale;
        PooledTarget grown = kit.morphology(*in.layer, width, !in_);
        if (!grown)
            return {};
        const fxmath::Rgb c = colorParam(p, k::Color, {1.f, 1.f, 1.f});
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform4f(m_color, c.r, c.g, c.b,
            float(std::clamp(val(p, k::Opacity, in.localFrame, 100) / 100.0, 0.0, 1.0)));
        kit.gl()->glUniform1i(m_inside, in_ ? 1 : 0);
        draw(kit, *in.layer, grown->texture);
        return out;
    }

private:
    static bool inside(const ParamSet& p)
    {
        return choiceParam(p, k::Position, fxchoice::Outside) == fxchoice::Inside;
    }
    GLint m_color = -1, m_inside = -1;
};

const GlEffectFactory kStylizeTable[] = {
    {fxid::Glow, &make<Glow>},
    {fxid::FilmGrain, &make<FilmGrain>},
    {fxid::ChromaticAberration, &make<ChromaticAberration>},
    {fxid::EdgeDetect, &make<EdgeDetect>},
    {fxid::Emboss, &make<Emboss>},
    {fxid::Stroke, &make<Stroke>},
};

} // namespace

std::span<const GlEffectFactory> stylizeGlEffects()
{
    return kStylizeTable;
}

} // namespace kk::render
