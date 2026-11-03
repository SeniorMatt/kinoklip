// SPDX-License-Identifier: GPL-3.0-or-later
// GlKeyers — the keying effects on the GPU: Chroma Key (green / blue screen), Luma Key and
// HSL Key. The formulas are the twins of render/EffectMath.cpp (the CPU reference).
//
// Alpha: the layer is premultiplied RGBA16F. A keyer computes a matte m (1 = keep) from the
// straight colour and outputs premultiplied (colour · a · m, a · m), so the keyed clip
// composites over the lower tracks with the compositor's normal premultiplied "over" (GPU
// preview and render share this path). The clip's masks are applied after its effects
// (GlRenderer::drawClip), so a mask multiplies the keyed alpha: a garbage matte.
#include "core/VideoEffectKeys.h"
#include "render/GlEffectBase.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

// ---- chroma_key ----

// Modes: 0 = everything in one pass; 1 = write the raw matte (all channels) for the matte
// processing passes; 2 = final output with the processed matte from uSecond.
const char* const kChromaKeyFragment = R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;
uniform int uMode;
uniform vec3 uKey;          // straight key colour
uniform float uTolerance;   // 0..1
uniform float uSoftness;    // 0..1, > 0
uniform float uClipBlack;
uniform float uClipWhite;
uniform int uInvert;
uniform float uSpill;       // 0..1
uniform int uSpillMax;      // 1 = limit to max of the other channels, 0 = their mean
uniform int uSpillChannel;  // 1 = green, 2 = blue
uniform int uView;

vec2 chromaOf(vec3 c)
{
    float y = luma(c);
    return vec2((c.b - y) / 1.8556, (c.r - y) / 1.5748) / (y + 0.1);
}

float chromaDistance(vec3 c)
{
    vec2 k = chromaOf(uKey);
    float kl = max(length(k), 0.05);
    vec2 dir = k / kl;
    vec2 cc = chromaOf(c);
    float proj = dot(cc, dir);
    float along = proj / kl;
    float across = length(cc - proj * dir) / kl;
    float shortfall = max(0.0, 1.0 - along);
    return sqrt(across * across + shortfall * shortfall);
}

float matte(vec3 c)
{
    float m = smoothstep(uTolerance, uTolerance + uSoftness, chromaDistance(c));
    m = clamp((m - uClipBlack) / max(uClipWhite - uClipBlack, 1e-3), 0.0, 1.0);
    return uInvert == 1 ? 1.0 - m : m;
}

vec3 despill(vec3 c)
{
    if (uSpillChannel == 2) {
        float limit = uSpillMax == 1 ? max(c.r, c.g) : 0.5 * (c.r + c.g);
        c.b -= uSpill * max(0.0, c.b - limit);
    } else {
        float limit = uSpillMax == 1 ? max(c.r, c.b) : 0.5 * (c.r + c.b);
        c.g -= uSpill * max(0.0, c.g - limit);
    }
    return c;
}

void main()
{
    vec4 t = texture(uTex, vUv);
    vec3 c = straight(t);
    if (uMode == 1) {
        fragColor = vec4(matte(c));
        return;
    }
    float m = uMode == 2 ? texture(uSecond, vUv).r : matte(c);
    fragColor = keyOutput(uView, despill(c), t.a, m);
}
)";

class ChromaKey final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "chroma_key", kChromaKeyFragment, why, true))
            return false;
        m_mode = uniform(kit, "uMode");
        m_key = uniform(kit, "uKey");
        m_tolerance = uniform(kit, "uTolerance");
        m_softness = uniform(kit, "uSoftness");
        m_clipBlack = uniform(kit, "uClipBlack");
        m_clipWhite = uniform(kit, "uClipWhite");
        m_invert = uniform(kit, "uInvert");
        m_spill = uniform(kit, "uSpill");
        m_spillMax = uniform(kit, "uSpillMax");
        m_spillChannel = uniform(kit, "uSpillChannel");
        m_view = uniform(kit, "uView");
        return true;
    }

    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const fxmath::ChromaKeyParams p = fxmath::chromaKeyParams(*in.params, in.localFrame);
        const double chokePx = p.shrinkGrowPx * in.scale;
        const double blurSigma = p.edgeBlurPx * in.scale * kSigmaPerRadius;
        const bool processMatte = std::abs(chokePx) >= 0.05 || blurSigma >= 0.25;
        if (!processMatte) {
            PooledTarget out = begin(kit, *in.layer);
            setUniforms(kit, p, 0);
            draw(kit, *in.layer);
            return out;
        }
        // Matte pass, then shrink / grow (erode / dilate) and edge blur on the matte only, so
        // the colour is never smeared; the final pass multiplies the processed matte in.
        PooledTarget matte = begin(kit, *in.layer);
        setUniforms(kit, p, 1);
        draw(kit, *in.layer);
        if (PooledTarget choked = kit.morphology(*matte, std::abs(chokePx), chokePx > 0))
            matte = std::move(choked);
        if (PooledTarget blurred = kit.gaussianBlur(*matte, blurSigma))
            matte = std::move(blurred);
        PooledTarget out = begin(kit, *in.layer);
        setUniforms(kit, p, 2);
        draw(kit, *in.layer, matte->texture);
        return out;
    }

private:
    void setUniforms(GlKit& kit, const fxmath::ChromaKeyParams& p, int mode) const
    {
        GlFunctions* gl = kit.gl();
        gl->glUniform1i(m_mode, mode);
        gl->glUniform3f(m_key, p.key.r, p.key.g, p.key.b);
        gl->glUniform1f(m_tolerance, p.tolerance);
        gl->glUniform1f(m_softness, std::max(p.softness, 1e-3f));
        gl->glUniform1f(m_clipBlack, p.clipBlack);
        gl->glUniform1f(m_clipWhite, p.clipWhite);
        gl->glUniform1i(m_invert, p.invert ? 1 : 0);
        gl->glUniform1f(m_spill, p.spillAmount);
        gl->glUniform1i(m_spillMax, p.spillMaximum ? 1 : 0);
        gl->glUniform1i(m_spillChannel, p.spillChannel);
        gl->glUniform1i(m_view, int(p.view));
    }

    GLint m_mode = -1, m_key = -1, m_tolerance = -1, m_softness = -1, m_clipBlack = -1,
          m_clipWhite = -1, m_invert = -1, m_spill = -1, m_spillMax = -1, m_spillChannel = -1,
          m_view = -1;
};

// ---- luma_key ----

const char* const kLumaKeyFragment = R"(
uniform sampler2D uTex;
uniform float uLow;
uniform float uHigh;
uniform float uSoftness; // > 0
uniform int uInvert;
uniform int uView;
void main()
{
    vec4 t = texture(uTex, vUv);
    vec3 c = straight(t);
    float y = luma(c);
    float m = smoothstep(uLow - uSoftness, uLow, y) * (1.0 - smoothstep(uHigh, uHigh + uSoftness, y));
    if (uInvert == 1)
        m = 1.0 - m;
    fragColor = keyOutput(uView, c, t.a, m);
}
)";

class LumaKey final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "luma_key", kLumaKeyFragment, why, true))
            return false;
        m_low = uniform(kit, "uLow");
        m_high = uniform(kit, "uHigh");
        m_softness = uniform(kit, "uSoftness");
        m_invert = uniform(kit, "uInvert");
        m_view = uniform(kit, "uView");
        return true;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const fxmath::LumaKeyParams p = fxmath::lumaKeyParams(*in.params, in.localFrame);
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform1f(m_low, p.low);
        gl->glUniform1f(m_high, p.high);
        gl->glUniform1f(m_softness, std::max(p.softness, 1e-3f));
        gl->glUniform1i(m_invert, p.invert ? 1 : 0);
        gl->glUniform1i(m_view, int(p.view));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_low = -1, m_high = -1, m_softness = -1, m_invert = -1, m_view = -1;
};

// ---- hsl_key ----

const char* const kHslKeyFragment = R"(
uniform sampler2D uTex;
uniform float uHue;        // degrees
uniform float uHalfWidth;  // degrees
uniform float uHueSoft;    // degrees, > 0
uniform vec3 uSat;         // low, high, softness (> 0)
uniform vec3 uLum;         // low, high, softness (> 0)
uniform int uInvert;
uniform int uView;

vec3 toHsl(vec3 c)
{
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float d = mx - mn;
    float l = 0.5 * (mx + mn);
    float s = d <= 1e-6 ? 0.0 : d / max(1.0 - abs(2.0 * l - 1.0), 1e-6);
    float h = 0.0;
    if (d > 1e-6) {
        if (mx == c.r)
            h = mod((c.g - c.b) / d + 6.0, 6.0);
        else if (mx == c.g)
            h = (c.b - c.r) / d + 2.0;
        else
            h = (c.r - c.g) / d + 4.0;
    }
    return vec3(h * 60.0, s, l);
}

float rangeMask(vec3 r, float x)
{
    return smoothstep(r.x - r.z, r.x, x) * (1.0 - smoothstep(r.y, r.y + r.z, x));
}

void main()
{
    vec4 t = texture(uTex, vUv);
    vec3 c = straight(t);
    vec3 hsl = toHsl(c);
    float dh = abs(mod(hsl.x - uHue + 540.0, 360.0) - 180.0);
    float qh = 1.0 - smoothstep(uHalfWidth, uHalfWidth + uHueSoft, dh);
    float m = 1.0 - qh * rangeMask(uSat, hsl.y) * rangeMask(uLum, hsl.z);
    if (uInvert == 1)
        m = 1.0 - m;
    fragColor = keyOutput(uView, c, t.a, m);
}
)";

class HslKey final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "hsl_key", kHslKeyFragment, why, true))
            return false;
        m_hue = uniform(kit, "uHue");
        m_halfWidth = uniform(kit, "uHalfWidth");
        m_hueSoft = uniform(kit, "uHueSoft");
        m_sat = uniform(kit, "uSat");
        m_lum = uniform(kit, "uLum");
        m_invert = uniform(kit, "uInvert");
        m_view = uniform(kit, "uView");
        return true;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const fxmath::HslKeyParams p = fxmath::hslKeyParams(*in.params, in.localFrame);
        constexpr float kMin = 1e-3f; // smoothstep needs a ramp (as fxmath's kMinRamp)
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform1f(m_hue, p.hue);
        gl->glUniform1f(m_halfWidth, 0.5f * p.hueWidth);
        gl->glUniform1f(m_hueSoft, std::max(p.hueSoftness, kMin));
        gl->glUniform3f(m_sat, p.satLow, p.satHigh, std::max(p.satSoftness, kMin));
        gl->glUniform3f(m_lum, p.lumLow, p.lumHigh, std::max(p.lumSoftness, kMin));
        gl->glUniform1i(m_invert, p.invert ? 1 : 0);
        gl->glUniform1i(m_view, int(p.view));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_hue = -1, m_halfWidth = -1, m_hueSoft = -1, m_sat = -1, m_lum = -1, m_invert = -1,
          m_view = -1;
};

const GlEffectFactory kKeyerTable[] = {
    {fxid::ChromaKey, &make<ChromaKey>},
    {fxid::LumaKey, &make<LumaKey>},
    {fxid::HslKey, &make<HslKey>},
};

} // namespace

std::span<const GlEffectFactory> keyerGlEffects()
{
    return kKeyerTable;
}

} // namespace kk::render
