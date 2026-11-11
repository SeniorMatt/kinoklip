// SPDX-License-Identifier: GPL-3.0-or-later
// GlEffects — the built-in video effects as GLSL passes, and their registration table.
//
// All effects work on premultiplied RGBA16F layers. Colour adjustments that are not linear in
// the colour (contrast, brightness) un-premultiply first; linear ones (saturation, vignette)
// work on premultiplied values directly, which is exact and keeps transparent pixels black.
#include "render/GlEffects.h"

#include "render/GlEffectBase.h"

#include <QColor>
#include <QHash>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace kk::render {

namespace {

using fx::boolParam;
using fx::make;
using fx::PassEffect;

// Parameter keys (must match the descriptors in core/BuiltinEffects.cpp). QStringLiteral
// keeps lookups allocation-free.
const QString kRadius = QStringLiteral("radius");
const QString kBrightness = QStringLiteral("brightness");
const QString kContrast = QStringLiteral("contrast");
const QString kSaturation = QStringLiteral("saturation");
const QString kAmount = QStringLiteral("amount");
const QString kHorizontal = QStringLiteral("horizontal");
const QString kVertical = QStringLiteral("vertical");
const QString kSize = QStringLiteral("size");
const QString kSoftness = QStringLiteral("softness");
const QString kBlockSize = QStringLiteral("blockSize");
const QString kDistance = QStringLiteral("distance");
const QString kAngle = QStringLiteral("angle");
const QString kBlur = QStringLiteral("blur");
const QString kOpacity = QStringLiteral("opacity");
const QString kColor = QStringLiteral("color");

/// Gaussian sigma for a "radius" parameter: the visible extent of a Gaussian is about 2σ, so
/// radius = 2σ makes "radius 10 px" blur roughly 10 px each way.
constexpr double kSigmaPerRadius = 0.5;
/// Layer pixels below which a blur/sharpen radius has no visible effect.
constexpr double kMinRadiusPx = 0.5;

// ---- gaussian_blur ----

class GaussianBlur final : public GlEffect
{
public:
    bool initialize(GlKit&, QString*) override { return true; } // uses GlKit::gaussianBlur
    void destroy(GlKit&) override { }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return p.value(kRadius, f, 5.0) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const double radius = in.params->value(kRadius, in.localFrame, 5.0) * in.scale;
        if (radius < kMinRadiusPx)
            return {};
        return kit.gaussianBlur(*in.layer, radius * kSigmaPerRadius);
    }
};

// ---- brightness_contrast ----

class BrightnessContrast final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "brightness_contrast", R"(
uniform sampler2D uTex;
uniform float uBrightness; // added to straight (un-premultiplied) RGB
uniform float uContrast;   // slope around mid grey
void main()
{
    vec4 c = texture(uTex, vUv);
    if (c.a <= 0.0) { fragColor = vec4(0.0); return; }
    vec3 rgb = (c.rgb / c.a - 0.5) * uContrast + 0.5 + uBrightness;
    fragColor = vec4(clamp(rgb, 0.0, 1.0) * c.a, c.a);
}
)",
                why))
            return false;
        m_brightness = uniform(kit, "uBrightness");
        m_contrast = uniform(kit, "uContrast");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return p.value(kBrightness, f, 0.0) == 0.0 && p.value(kContrast, f, 0.0) == 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        // Brightness ±100 → ±0.5 offset. Contrast −100..0 → slope 0..1, 0..+100 → 1..4.
        const double b = in.params->value(kBrightness, in.localFrame, 0.0) / 200.0;
        const double c = in.params->value(kContrast, in.localFrame, 0.0) / 100.0;
        const double slope = c >= 0 ? 1.0 + 3.0 * c : 1.0 + c;
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform1f(m_brightness, float(b));
        kit.gl()->glUniform1f(m_contrast, float(slope));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_brightness = -1;
    GLint m_contrast = -1;
};

// ---- saturation ----

class Saturation final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "saturation", R"(
uniform sampler2D uTex;
uniform float uSaturation; // 0 = grey, 1 = unchanged, 2 = double
void main()
{
    vec4 c = texture(uTex, vUv);
    float luma = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)); // BT.709 weights
    fragColor = vec4(clamp(mix(vec3(luma), c.rgb, uSaturation), 0.0, c.a), c.a);
}
)",
                why))
            return false;
        m_saturation = uniform(kit, "uSaturation");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return p.value(kSaturation, f, 100.0) == 100.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform1f(
            m_saturation, float(in.params->value(kSaturation, in.localFrame, 100.0) / 100.0));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_saturation = -1;
};

// ---- sharpen (unsharp mask: src + k·(src − blur(src))) ----

class Sharpen final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "sharpen", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond; // blurred copy
uniform float uAmount;
void main()
{
    vec4 s = texture(uTex, vUv);
    vec4 b = texture(uSecond, vUv);
    fragColor = vec4(clamp(s.rgb + uAmount * (s.rgb - b.rgb), 0.0, s.a), s.a);
}
)",
                why))
            return false;
        m_amount = uniform(kit, "uAmount");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return p.value(kAmount, f, 50.0) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const double radius = in.params->value(kRadius, in.localFrame, 1.0) * in.scale;
        PooledTarget blurred
            = kit.gaussianBlur(*in.layer, std::max(radius, kMinRadiusPx) * kSigmaPerRadius);
        if (!blurred)
            return {};
        PooledTarget out = begin(kit, *in.layer);
        // Amount 100 % → k = 2 (a strong but common unsharp-mask setting).
        kit.gl()->glUniform1f(
            m_amount, float(in.params->value(kAmount, in.localFrame, 50.0) / 100.0 * 2.0));
        draw(kit, *in.layer, blurred->texture);
        return out;
    }

private:
    GLint m_amount = -1;
};

// ---- mirror (flip the layer) ----

class Mirror final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        return build(kit, "mirror", R"(
uniform sampler2D uTex;
void main() { fragColor = texture(uTex, vUv); }
)",
            why);
    }
    bool isIdentity(const ParamSet& p, int64_t) const override
    {
        return !boolParam(p, kHorizontal, true) && !boolParam(p, kVertical, false);
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const bool h = boolParam(*in.params, kHorizontal, true);
        const bool v = boolParam(*in.params, kVertical, false);
        PooledTarget out = begin(kit, *in.layer);
        // uv = flip(quad): u → 1 − u and/or v → 1 − v.
        kit.setMatrix(
            m_program.uvMatrix, QTransform(h ? -1 : 1, 0, 0, v ? -1 : 1, h ? 1 : 0, v ? 1 : 0));
        draw(kit, *in.layer);
        return out;
    }
};

// ---- vignette ----

class Vignette final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "vignette", R"(
uniform sampler2D uTex;
uniform float uAmount; // 0..1 darkening at the corners
uniform float uInner;  // normalised distance where darkening starts (0 = centre, 1 = corner)
uniform float uOuter;  // normalised distance of full darkening
uniform float uAspect; // width / height, so the vignette is round, not stretched
void main()
{
    vec4 c = texture(uTex, vUv);
    vec2 p = (vUv - 0.5) * vec2(uAspect, 1.0);
    float d = length(p) / length(vec2(uAspect, 1.0) * 0.5);
    float v = 1.0 - uAmount * smoothstep(uInner, uOuter, d);
    fragColor = vec4(c.rgb * v, c.a);
}
)",
                why))
            return false;
        m_amount = uniform(kit, "uAmount");
        m_inner = uniform(kit, "uInner");
        m_outer = uniform(kit, "uOuter");
        m_aspect = uniform(kit, "uAspect");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return p.value(kAmount, f, 50.0) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const double amount
            = std::clamp(in.params->value(kAmount, in.localFrame, 50.0), 0.0, 100.0);
        const double size = std::clamp(in.params->value(kSize, in.localFrame, 50.0), 0.0, 100.0);
        const double soft
            = std::clamp(in.params->value(kSoftness, in.localFrame, 50.0), 0.0, 100.0);
        // Size 100 % = no darkening inside the frame; softness widens the ramp up to 1.0.
        const double inner = size / 100.0;
        const double outer = inner + 0.01 + soft / 100.0;
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform1f(m_amount, float(amount / 100.0));
        gl->glUniform1f(m_inner, float(inner));
        gl->glUniform1f(m_outer, float(outer));
        gl->glUniform1f(m_aspect, float(in.layer->size.width()) / float(in.layer->size.height()));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_amount = -1, m_inner = -1, m_outer = -1, m_aspect = -1;
};

// ---- mosaic ----

class Mosaic final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "mosaic", R"(
uniform sampler2D uTex;
uniform vec2 uSize;   // layer size in pixels
uniform float uBlock; // block size in layer pixels
void main()
{
    vec2 centre = (floor(vUv * uSize / uBlock) + 0.5) * uBlock;
    fragColor = texture(uTex, min(centre, uSize - 0.5) / uSize);
}
)",
                why))
            return false;
        m_size = uniform(kit, "uSize");
        m_block = uniform(kit, "uBlock");
        return true;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const double block
            = std::max(1.0, in.params->value(kBlockSize, in.localFrame, 20.0) * in.scale);
        if (block <= 1.0)
            return {};
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform2f(
            m_size, float(in.layer->size.width()), float(in.layer->size.height()));
        kit.gl()->glUniform1f(m_block, float(block));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_size = -1, m_block = -1;
};

// ---- drop_shadow ----

class DropShadow final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "drop_shadow", R"(
uniform sampler2D uTex;
uniform sampler2D uSecond; // blurred layer (alpha used)
uniform vec2 uOffset;      // shadow displacement in uv
uniform vec4 uColor;       // premultiplied shadow colour × opacity
void main()
{
    vec4 src = texture(uTex, vUv);
    float a = texture(uSecond, vUv - uOffset).a;
    fragColor = src + uColor * a * (1.0 - src.a); // shadow goes *behind* the layer
}
)",
                why))
            return false;
        m_offset = uniform(kit, "uOffset");
        m_color = uniform(kit, "uColor");
        return true;
    }
    double paddingPx(const ParamSet& p, int64_t f) const override
    {
        // Displacement + visible blur extent (3σ) + a pixel for filtering.
        return std::abs(p.value(kDistance, f, 10.0))
            + 3.0 * kSigmaPerRadius * p.value(kBlur, f, 8.0) + 1.0;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return p.value(kOpacity, f, 60.0) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const double distance = p.value(kDistance, in.localFrame, 10.0) * in.scale;
        const double angle = p.value(kAngle, in.localFrame, 135.0) * std::numbers::pi / 180.0;
        const double blur = p.value(kBlur, in.localFrame, 8.0) * in.scale;
        const double opacity
            = std::clamp(p.value(kOpacity, in.localFrame, 60.0), 0.0, 100.0) / 100.0;
        const auto it = p.statics.constFind(kColor);
        const QColor color = it != p.statics.constEnd() ? it->value<QColor>() : QColor(Qt::black);

        PooledTarget blurred;
        if (blur >= kMinRadiusPx)
            blurred = kit.gaussianBlur(*in.layer, blur * kSigmaPerRadius);
        const GLuint shadowSource = blurred ? blurred->texture : in.layer->texture;

        // `angle` is the direction the light comes from (math convention, y up): 135° = from
        // the upper left, so the shadow falls down-right. In y-down pixels the shadow offset
        // is (−cos a, +sin a) · distance.
        const float ox = float(-std::cos(angle) * distance / in.layer->size.width());
        const float oy = float(std::sin(angle) * distance / in.layer->size.height());
        const float a = float(color.alphaF() * opacity);
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform2f(m_offset, ox, oy);
        kit.gl()->glUniform4f(m_color, float(color.redF()) * a, float(color.greenF()) * a,
            float(color.blueF()) * a, a);
        draw(kit, *in.layer, shadowSource);
        return out;
    }

private:
    GLint m_offset = -1, m_color = -1;
};

/// THE registration table: descriptor id → implementation. Ids must match BuiltinEffects.cpp.
const GlEffectFactory kEffectTable[] = {
    {"gaussian_blur", &make<GaussianBlur>},
    {"brightness_contrast", &make<BrightnessContrast>},
    {"saturation", &make<Saturation>},
    {"sharpen", &make<Sharpen>},
    {"mirror", &make<Mirror>},
    {"vignette", &make<Vignette>},
    {"mosaic", &make<Mosaic>},
    {"drop_shadow", &make<DropShadow>},
};

} // namespace

std::span<const GlEffectFactory> builtinGlEffects()
{
    // Built once (thread-safe static initialisation); the renderer reads it at start-up.
    static const std::vector<GlEffectFactory> all = [] {
        std::vector<GlEffectFactory> rows(std::begin(kEffectTable), std::end(kEffectTable));
        for (auto table : {keyerGlEffects(), colorGlEffects(), blurGlEffects(), stylizeGlEffects(),
                 distortGlEffects(), gradeGlEffects(), focusGlEffects(), lightGlEffects(),
                 lookGlEffects(), warpGlEffects(), timeGlEffects(), extraColorGlEffects(),
                 extraGlEffects(), detailGlEffects(), damageGlEffects(), paintGlEffects(),
                 opticsGlEffects(), utilityGlEffects(), temporalGlEffects(),
                 repairGlEffects(), templateLookGlEffects(), meshGlEffects(), backgroundRemovalGlEffects()})
            rows.insert(rows.end(), table.begin(), table.end());
        return rows;
    }();
    return all;
}

const GlEffectFactory* findGlEffectFactory(const QString& id)
{
    static const QHash<QString, const GlEffectFactory*> byId = [] {
        QHash<QString, const GlEffectFactory*> h;
        for (const GlEffectFactory& f : builtinGlEffects())
            h.insert(QLatin1String(f.id), &f);
        return h;
    }();
    return byId.value(id, nullptr);
}

} // namespace kk::render
