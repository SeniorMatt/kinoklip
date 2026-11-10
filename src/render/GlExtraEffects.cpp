// SPDX-License-Identifier: GPL-3.0-or-later
// GlExtraEffects — 1.2 spatial effects on the GPU: Colored Border, Alpha Matte Shrink and
// Grow, Transform, Dent, Fast Noise and Chromatic Aberration Fix (spec 56). One shader each
// over the layer; parameter pixels are timeline pixels, multiplied by EffectInput::scale.
// The noise code is shared with the Noise Gradient generator (render/TestPatterns.h).
#include "core/VideoEffectKeys.h"
#include "render/EffectMathExtra.h"
#include "render/GlEffectBase.h"
#include "render/TestPatterns.h"

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

// ---- colored_border ----

class ColoredBorder final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "colored_border", R"(
uniform sampler2D uTex;
uniform vec4 uRect;     // content rectangle, uv
uniform vec2 uSizePx;   // layer size, px
uniform vec4 uColor;    // straight colour, opacity
uniform vec3 uShape;    // width, corner radius, feather (layer px)
void main()
{
    vec4 t = texture(uTex, vUv);
    vec2 lo = uRect.xy * uSizePx, hi = uRect.zw * uSizePx;
    vec2 p = vUv * uSizePx - 0.5 * (lo + hi);
    vec2 inner = 0.5 * (hi - lo) - vec2(uShape.x);
    float r = clamp(uShape.y, 0.0, max(min(inner.x, inner.y), 0.0));
    vec2 d = abs(p) - inner + r;
    // Distance beyond the inner rounded rectangle (> 0 in the border); half a pixel of
    // anti-aliasing, or the feather when there is one.
    float sd = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
    float soft = max(uShape.z, 1.0);
    float cover = clamp(0.5 + sd / soft, 0.0, 1.0);
    vec2 q = abs(vUv * uSizePx - 0.5 * (lo + hi));
    float inside = (q.x <= 0.5 * (hi.x - lo.x) && q.y <= 0.5 * (hi.y - lo.y)) ? 1.0 : 0.0;
    float b = cover * uColor.a * inside;
    fragColor = vec4(uColor.rgb * b + t.rgb * (1.0 - b), b + t.a * (1.0 - b));
}
)",
                why))
            return false;
        m_rect = uniform(kit, "uRect");
        m_size = uniform(kit, "uSizePx");
        m_color = uniform(kit, "uColor");
        m_shape = uniform(kit, "uShape");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Opacity, f, 100) <= 0.0 || val(p, k::Width, f, 24) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const LayerGeometry g = layerGeometry(in);
        const fxmath::Rgb c = colorParam(p, k::Color, {0.95f, 0.69f, 0.2f});
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform4f(m_rect, g.x0, g.y0, g.x1, g.y1);
        gl->glUniform2f(m_size, g.width, g.height);
        gl->glUniform4f(m_color, c.r, c.g, c.b,
            float(std::clamp(val(p, k::Opacity, in.localFrame, 100), 0.0, 100.0) / 100.0));
        gl->glUniform3f(m_shape, float(val(p, k::Width, in.localFrame, 24) * in.scale),
            float(val(p, k::Radius, in.localFrame, 0) * in.scale),
            float(val(p, k::Feather, in.localFrame, 0) * in.scale));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_rect = -1, m_size = -1, m_color = -1, m_shape = -1;
};

// ---- alpha_matte_shrink_grow ----

class AlphaMatteShrinkGrow final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        // Mode 0: the alpha as a matte in every channel; mode 1: the picture with the
        // processed matte (uSecond) as its alpha. The colour is never smeared.
        if (!build(kit, "alpha_matte_shrink_grow", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform int uMode;
void main()
{
    vec4 t = texture(uTex, vUv);
    if (uMode == 0) {
        fragColor = vec4(t.a);
        return;
    }
    float m = texture(uSecond, vUv).r;
    fragColor = vec4(t.a > 0.0 ? t.rgb / t.a * m : vec3(0.0), m); // t may be the dilated layer
}
)",
                why))
            return false;
        m_mode = uniform(kit, "uMode");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::ShrinkGrow, f, 0) == 0.0 && val(p, k::EdgeBlur, f, 0) == 0.0;
    }
    double paddingPx(const ParamSet& p, int64_t f) const override
    {
        // Growing needs room beyond the picture's own edge.
        return std::max(0.0, val(p, k::ShrinkGrow, f, 0)) + 2.0 * val(p, k::EdgeBlur, f, 0);
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const double chokePx = val(p, k::ShrinkGrow, in.localFrame, 0) * in.scale;
        const double blurSigma = val(p, k::EdgeBlur, in.localFrame, 0) * in.scale * kSigmaPerRadius;
        PooledTarget matte = begin(kit, *in.layer);
        kit.gl()->glUniform1i(m_mode, 0);
        draw(kit, *in.layer);
        if (std::abs(chokePx) >= 0.05) {
            if (PooledTarget choked = kit.morphology(*matte, std::abs(chokePx), chokePx > 0))
                matte = std::move(choked);
        }
        if (blurSigma >= 0.25) {
            if (PooledTarget blurred = kit.gaussianBlur(*matte, blurSigma))
                matte = std::move(blurred);
        }
        // Growing needs colour where the picture had none: take it from a dilated copy of the
        // layer (the nearest picture colour), shrinking keeps the layer's own.
        PooledTarget grown;
        if (chokePx >= 0.05)
            grown = kit.morphology(*in.layer, chokePx, true);
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform1i(m_mode, 1);
        draw(kit, grown ? *grown : *in.layer, matte->texture);
        return out;
    }

private:
    GLint m_mode = -1;
};

// ---- transform ----

class TransformFx final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "transform_fx", R"(
uniform sampler2D uTex;
uniform vec2 uSizePx;    // layer size, px
uniform vec2 uOrigin;    // content centre, px from the layer's top left
uniform vec2 uOffset;    // position, px
uniform vec2 uPivot;     // pivot from the content centre, px
uniform vec2 uRotScale;  // cos / sin of the clockwise rotation (screen, y down)
uniform vec2 uScaleOp;   // 1 / scale, opacity
void main()
{
    vec2 out_ = vUv * uSizePx - uOrigin;
    // Inverse of: out = pivot + R * S * (src - pivot) + offset.
    vec2 d = (out_ - uOffset - uPivot) * uScaleOp.x;
    vec2 src = uPivot + vec2(uRotScale.x * d.x + uRotScale.y * d.y,
                             -uRotScale.y * d.x + uRotScale.x * d.y);
    vec2 uv = (src + uOrigin) / uSizePx;
    vec4 t = (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) ? vec4(0.0) : texture(uTex, uv);
    fragColor = t * uScaleOp.y;
}
)",
                why))
            return false;
        m_size = uniform(kit, "uSizePx");
        m_origin = uniform(kit, "uOrigin");
        m_offset = uniform(kit, "uOffset");
        m_pivot = uniform(kit, "uPivot");
        m_rot = uniform(kit, "uRotScale");
        m_scale = uniform(kit, "uScaleOp");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::OffsetX, f, 0) == 0.0 && val(p, k::OffsetY, f, 0) == 0.0
            && val(p, k::Scale, f, 100) == 100.0 && val(p, k::Rotation, f, 0) == 0.0
            && val(p, k::Opacity, f, 100) == 100.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const LayerGeometry g = layerGeometry(in);
        const double s = in.scale;
        const double a = val(p, k::Rotation, in.localFrame, 0) * std::numbers::pi / 180.0;
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(m_size, g.width, g.height);
        gl->glUniform2f(m_origin, 0.5f * (g.x0 + g.x1) * g.width, 0.5f * (g.y0 + g.y1) * g.height);
        gl->glUniform2f(m_offset, float(val(p, k::OffsetX, in.localFrame, 0) * s),
            float(val(p, k::OffsetY, in.localFrame, 0) * s));
        gl->glUniform2f(m_pivot, float(val(p, k::CenterX, in.localFrame, 0) * s),
            float(val(p, k::CenterY, in.localFrame, 0) * s));
        gl->glUniform2f(m_rot, float(std::cos(a)), float(std::sin(a)));
        gl->glUniform2f(m_scale, float(100.0 / std::max(1.0, val(p, k::Scale, in.localFrame, 100))),
            float(std::clamp(val(p, k::Opacity, in.localFrame, 100), 0.0, 100.0) / 100.0));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_size = -1, m_origin = -1, m_offset = -1, m_pivot = -1, m_rot = -1, m_scale = -1;
};

// ---- dent ----

class Dent final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "dent", R"(
uniform sampler2D uTex;
uniform vec2 uSizePx;
uniform vec2 uOrigin;    // content centre, px from the layer's top left
uniform vec2 uCentre;    // dent centre from the content centre, px
uniform vec3 uShape;     // size (radius) px, strength 0..0.9, type 0..5
uniform int uEdges;      // 0 transparent, 1 repeat, 2 mirrored
void main()
{
    vec2 d = vUv * uSizePx - uOrigin - uCentre;
    float R = uShape.x;
    int type = int(uShape.z + 0.5);
    float s = uShape.y;
    vec2 src = d;
    if (type == 0 || type == 1 || type == 4 || type == 5) {
        float r = (type >= 4 ? max(abs(d.x), abs(d.y)) : length(d)) / R;
        float w = r < 1.0 ? 1.0 - r * r : 0.0;
        // Bulge samples nearer the centre (magnifies), pinch farther out.
        float f = (type == 0 || type == 4) ? 1.0 - s * w : 1.0 / (1.0 - s * w);
        src = d * f;
    } else if (type == 2) {
        float w = max(0.0, 1.0 - (d.y * d.y) / (R * R));
        src.x = d.x * (1.0 - s * w);
    } else {
        float w = max(0.0, 1.0 - (d.x * d.x) / (R * R));
        src.y = d.y * (1.0 - s * w);
    }
    vec2 uv = (src + uCentre + uOrigin) / uSizePx;
    if (uEdges == 0) {
        bool outside = uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0;
        fragColor = outside ? vec4(0.0) : texture(uTex, uv);
    } else if (uEdges == 1) {
        fragColor = texture(uTex, clamp(uv, 0.0, 1.0));
    } else {
        fragColor = texture(uTex, 1.0 - abs(1.0 - mod(uv, 2.0)));
    }
}
)",
                why))
            return false;
        m_size = uniform(kit, "uSizePx");
        m_origin = uniform(kit, "uOrigin");
        m_centre = uniform(kit, "uCentre");
        m_shape = uniform(kit, "uShape");
        m_edges = uniform(kit, "uEdges");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 50) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const LayerGeometry g = layerGeometry(in);
        const QString mode = p.staticValue(k::Mode).toString();
        const QString edges = p.staticValue(k::Edges).toString();
        static const QString kTypes[] = {fxchoice::Bulge, fxchoice::Pinch, fxchoice::BowHorizontal,
            fxchoice::BowVertical, fxchoice::SquareBulge, fxchoice::SquarePinch};
        int type = 0;
        for (int i = 0; i < 6; ++i)
            type = mode == kTypes[i] ? i : type;
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(m_size, g.width, g.height);
        gl->glUniform2f(m_origin, 0.5f * (g.x0 + g.x1) * g.width, 0.5f * (g.y0 + g.y1) * g.height);
        gl->glUniform2f(m_centre, float(val(p, k::CenterX, in.localFrame, 0) * in.scale),
            float(val(p, k::CenterY, in.localFrame, 0) * in.scale));
        gl->glUniform3f(m_shape,
            float(std::max(1.0, val(p, k::Size, in.localFrame, 400) * in.scale)),
            float(0.9 * std::clamp(val(p, k::Amount, in.localFrame, 50), 0.0, 100.0) / 100.0),
            float(type));
        gl->glUniform1i(
            m_edges, edges == fxchoice::Transparent ? 0 : (edges == fxchoice::Mirrored ? 2 : 1));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_size = -1, m_origin = -1, m_centre = -1, m_shape = -1, m_edges = -1;
};

// ---- fast_noise ----

class FastNoise final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = std::string(testpattern::kGlslNoise) + R"(
uniform sampler2D uTex;
uniform vec2 uSizePx;
uniform vec2 uOrigin;     // content top left, px from the layer's top left
uniform vec3 uColor1;
uniform vec3 uColor2;
uniform vec4 uNoise;      // scale px, octaves, contrast factor, evolution
uniform uint uSeed;
uniform vec2 uBlend;      // mode 0..4, amount
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    float n = fractalNoise(vUv * uSizePx - uOrigin, uNoise.x, int(uNoise.y), uNoise.w, uSeed);
    vec3 nc = mix(uColor1, uColor2, clamp((n - 0.5) * uNoise.z + 0.5, 0.0, 1.0));
    vec3 b = t.rgb / t.a;
    int mode = int(uBlend.x + 0.5);
    vec3 r = nc;
    if (mode == 1) r = b + nc;
    else if (mode == 2) r = b * nc;
    else if (mode == 3) r = 1.0 - (1.0 - b) * (1.0 - nc);
    else if (mode == 4) r = mix(2.0 * b * nc, 1.0 - 2.0 * (1.0 - b) * (1.0 - nc), step(0.5, b));
    r = mix(b, r, uBlend.y);
    fragColor = vec4(clamp(r, 0.0, 1.0) * t.a, t.a);
}
)";
        if (!build(kit, "fast_noise", source.c_str(), why))
            return false;
        m_size = uniform(kit, "uSizePx");
        m_origin = uniform(kit, "uOrigin");
        m_c1 = uniform(kit, "uColor1");
        m_c2 = uniform(kit, "uColor2");
        m_noise = uniform(kit, "uNoise");
        m_seed = uniform(kit, "uSeed");
        m_blend = uniform(kit, "uBlend");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 50) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const LayerGeometry g = layerGeometry(in);
        const fxmath::Rgb c1 = colorParam(p, k::ShadowColor, {0, 0, 0});
        const fxmath::Rgb c2 = colorParam(p, k::HighlightColor, {1, 1, 1});
        const QString mode = p.staticValue(k::Mode).toString();
        static const QString kModes[] = {fxchoice::Normal, fxchoice::Add, fxchoice::Multiply,
            fxchoice::Screen, fxchoice::Overlay};
        int blend = 4;
        for (int i = 0; i < 5; ++i)
            blend = mode == kModes[i] ? i : blend;
        // Drift: Speed is evolution units per second on top of the Evolution parameter.
        const double evolution = val(p, k::Evolution, in.localFrame, 0)
            + val(p, k::Speed, in.localFrame, 0) * double(in.localFrame) / std::max(1.0, in.fps);
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(m_size, g.width, g.height);
        gl->glUniform2f(m_origin, g.x0 * g.width, g.y0 * g.height);
        gl->glUniform3f(m_c1, c1.r, c1.g, c1.b);
        gl->glUniform3f(m_c2, c2.r, c2.g, c2.b);
        gl->glUniform4f(m_noise,
            float(std::max(1.0, val(p, k::Scale, in.localFrame, 200) * in.scale)),
            float(std::clamp(std::lround(val(p, k::Detail, in.localFrame, 4)), 1L, 8L)),
            float(std::max(0.0, val(p, k::Contrast, in.localFrame, 200) / 100.0)),
            float(evolution));
        gl->glUniform1ui(
            m_seed, uint32_t(std::max(0L, std::lround(val(p, k::Seed, in.localFrame, 1)))));
        gl->glUniform2f(m_blend, float(blend),
            float(std::clamp(val(p, k::Amount, in.localFrame, 50), 0.0, 100.0) / 100.0));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_size = -1, m_origin = -1, m_c1 = -1, m_c2 = -1, m_noise = -1, m_seed = -1, m_blend = -1;
};

// ---- chromatic_aberration_fix ----

class ChromaticAberrationFix final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "chromatic_aberration_fix", R"(
uniform sampler2D uTex;
uniform vec2 uCentre;     // uv
uniform vec2 uShift;      // red and blue scale change (0.01 = 1 %)
uniform float uDefringe;  // 0..1
uniform vec2 uTexel;     // one tap: the fringe width in uv
void main()
{
    vec2 d = vUv - uCentre;
    vec4 r = texture(uTex, uCentre + d * (1.0 + uShift.x));
    vec4 g = texture(uTex, vUv);
    vec4 b = texture(uTex, uCentre + d * (1.0 + uShift.y));
    vec4 o = vec4(r.r, g.g, b.b, max(g.a, max(r.a, b.a)));
    if (uDefringe > 0.0 && o.a > 0.0) {
        // Colour fringes sit on edges: where the luma changes quickly, pull the colour toward
        // grey by the amount (alpha-weighted so transparent neighbours do not count).
        float yl = luma(texture(uTex, vUv - vec2(uTexel.x, 0.0)).rgb);
        float yr = luma(texture(uTex, vUv + vec2(uTexel.x, 0.0)).rgb);
        float yu = luma(texture(uTex, vUv - vec2(0.0, uTexel.y)).rgb);
        float yd = luma(texture(uTex, vUv + vec2(0.0, uTexel.y)).rgb);
        float edge = smoothstep(0.04, 0.25, length(vec2(yr - yl, yd - yu)));
        float y = luma(o.rgb);
        o.rgb = mix(o.rgb, vec3(y), uDefringe * edge);
    }
    fragColor = o;
}
)",
                why, true))
            return false;
        m_centre = uniform(kit, "uCentre");
        m_shift = uniform(kit, "uShift");
        m_defringe = uniform(kit, "uDefringe");
        m_texel = uniform(kit, "uTexel");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::RedShift, f, 0) == 0.0 && val(p, k::BlueShift, f, 0) == 0.0
            && val(p, k::Defringe, f, 0) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const LayerGeometry g = layerGeometry(in);
        float u = 0, v = 0;
        g.centre(
            in, val(p, k::CenterX, in.localFrame, 0), val(p, k::CenterY, in.localFrame, 0), &u, &v);
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(m_centre, u, v);
        gl->glUniform2f(m_shift, float(val(p, k::RedShift, in.localFrame, 0) / 100.0),
            float(val(p, k::BlueShift, in.localFrame, 0) / 100.0));
        gl->glUniform1f(m_defringe,
            float(std::clamp(val(p, k::Defringe, in.localFrame, 0), 0.0, 100.0) / 100.0));
        // The fringes are as wide as the largest shift at the content corner (plus a pixel of
        // slack): look that far to each side for the luma edge they sit on.
        const double halfDiagonal
            = 0.5 * std::hypot((g.x1 - g.x0) * g.width, (g.y1 - g.y0) * g.height);
        const double tapPx = 1.5
            + std::max(std::abs(val(p, k::RedShift, in.localFrame, 0)),
                  std::abs(val(p, k::BlueShift, in.localFrame, 0)))
                / 100.0 * halfDiagonal;
        gl->glUniform2f(m_texel, float(tapPx) / g.width, float(tapPx) / g.height);
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_centre = -1, m_shift = -1, m_defringe = -1, m_texel = -1;
};

const GlEffectFactory kTable[] = {
    {fxid::ColoredBorder, &make<ColoredBorder>},
    {fxid::AlphaMatteShrinkGrow, &make<AlphaMatteShrinkGrow>},
    {fxid::Transform, &make<TransformFx>},
    {fxid::Dent, &make<Dent>},
    {fxid::FastNoise, &make<FastNoise>},
    {fxid::ChromaticAberrationFix, &make<ChromaticAberrationFix>},
};

} // namespace

std::span<const GlEffectFactory> extraGlEffects()
{
    return kTable;
}

} // namespace kk::render
