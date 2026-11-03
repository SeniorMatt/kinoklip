// SPDX-License-Identifier: GPL-3.0-or-later
// GlBlurEffects — Box, Directional, Radial (spin) and Zoom blur on the GPU.
//
// Cost is bounded for every parameter value: the box blur uses GlKit::boxBlur() (separable,
// downscaled for large radii); the path blurs average kPathTaps bilinear samples along a
// line, an arc or a ray. When the samples would be more than kMaxTapSpacingPx apart, the
// blur runs as two passes: the first averages over 1/kPathTaps of the path, the second over
// the whole path with kPathTaps samples, which together equal a continuous average over the
// path (a box convolved with a comb of the box's width). So at most 2 × 24 taps per pixel.
#include "core/VideoEffectKeys.h"
#include "render/GlEffectBase.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

/// Samples per path-blur pass.
constexpr int kPathTaps = 24;
/// Above this distance between neighbouring samples (layer px) the two-pass path is used.
constexpr double kMaxTapSpacingPx = 1.5;
/// Largest zoom blur amount (100 % would sample the centre point only).
constexpr double kMaxZoomAmount = 0.95;

// ---- box_blur ----

class BoxBlur final : public GlEffect
{
public:
    bool initialize(GlKit&, QString*) override { return true; } // uses GlKit::boxBlur
    void destroy(GlKit&) override { }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return p.value(k::Radius, f, 5.0) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const double radius = in.params->value(k::Radius, in.localFrame, 5.0) * in.scale;
        if (radius < 0.25)
            return {};
        return kit.boxBlur(*in.layer, radius);
    }
};

// ---- directional / radial / zoom: one path-sampling shader ----

const char* const kPathBlurFragment = R"(
uniform sampler2D uTex;
uniform int uKind;        // 0 = line, 1 = arc (spin), 2 = ray (zoom)
uniform int uTaps;
uniform vec2 uSpan;       // line: whole path in uv (centred on the pixel)
uniform float uAngle;     // arc: whole angle in radians (centred)
uniform float uLogScale;  // ray: log of the smallest scale (one-sided towards the centre)
uniform vec2 uCentre;     // arc / ray centre in uv
uniform vec2 uSize;       // layer size in pixels (keeps arcs round)
void main()
{
    vec4 sum = vec4(0.0);
    vec2 p = (vUv - uCentre) * uSize;
    for (int i = 0; i < uTaps; ++i) {
        float t = (float(i) + 0.5) / float(uTaps);
        vec2 uv;
        if (uKind == 0) {
            uv = vUv + uSpan * (t - 0.5);
        } else if (uKind == 1) {
            float a = uAngle * (t - 0.5);
            float c = cos(a);
            float s = sin(a);
            uv = uCentre + vec2(c * p.x - s * p.y, s * p.x + c * p.y) / uSize;
        } else {
            uv = uCentre + p * exp(uLogScale * t) / uSize;
        }
        sum += texture(uTex, uv);
    }
    fragColor = sum / float(uTaps);
}
)";

/// Base of the three path blurs. Subclasses describe the path (pathLengthPx() for the
/// spacing test, setPath() for the uniforms of one pass with a fraction of the path).
class PathBlur : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "path_blur", kPathBlurFragment, why))
            return false;
        m_kind = uniform(kit, "uKind");
        m_taps = uniform(kit, "uTaps");
        m_span = uniform(kit, "uSpan");
        m_angle = uniform(kit, "uAngle");
        m_logScale = uniform(kit, "uLogScale");
        m_centre = uniform(kit, "uCentre");
        m_size = uniform(kit, "uSize");
        return true;
    }

    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const double lengthPx = pathLengthPx(in);
        if (lengthPx < kMinRadiusPx)
            return {};
        const bool twoPass = lengthPx / kPathTaps > kMaxTapSpacingPx;
        PooledTarget first;
        const RenderTarget* src = in.layer;
        if (twoPass) {
            first = pass(kit, in, *src, 1.0 / kPathTaps);
            src = first.get();
        }
        return pass(kit, in, *src, 1.0);
    }

protected:
    /// Longest distance a sample travels along the path (layer px).
    [[nodiscard]] virtual double pathLengthPx(const EffectInput& in) const = 0;
    /// Sets uKind and the path uniforms for @p fraction of the full path.
    virtual void setPath(GlKit& kit, const EffectInput& in, double fraction) = 0;

    PooledTarget pass(GlKit& kit, const EffectInput& in, const RenderTarget& src, double fraction)
    {
        PooledTarget out = begin(kit, src);
        kit.gl()->glUniform1i(m_taps, kPathTaps);
        kit.gl()->glUniform2f(m_size, float(src.size.width()), float(src.size.height()));
        setPath(kit, in, fraction);
        draw(kit, src);
        return out;
    }

    /// Farthest content corner from uv centre (@p u, @p v), in layer pixels.
    static double maxRadiusPx(const EffectInput& in, float u, float v)
    {
        const double w = in.layer->size.width(), h = in.layer->size.height();
        const double dx = std::max(u, 1.f - u) * w, dy = std::max(v, 1.f - v) * h;
        return std::hypot(dx, dy);
    }

    GLint m_kind = -1, m_taps = -1, m_span = -1, m_angle = -1, m_logScale = -1, m_centre = -1,
          m_size = -1;
};

class DirectionalBlur final : public PathBlur
{
public:
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return p.value(k::Length, f, 20.0) <= 0.0;
    }

protected:
    double pathLengthPx(const EffectInput& in) const override
    {
        return in.params->value(k::Length, in.localFrame, 20.0) * in.scale;
    }
    void setPath(GlKit& kit, const EffectInput& in, double fraction) override
    {
        const double length = pathLengthPx(in) * fraction;
        const double a = in.params->value(k::Angle, in.localFrame, 0.0) * std::numbers::pi / 180;
        // Angle 0 = horizontal, positive counter-clockwise on screen (y down → −sin).
        kit.gl()->glUniform1i(m_kind, 0);
        kit.gl()->glUniform2f(m_span, float(std::cos(a) * length / in.layer->size.width()),
            float(-std::sin(a) * length / in.layer->size.height()));
    }
};

class RadialBlur final : public PathBlur
{
public:
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return p.value(k::Angle, f, 10.0) <= 0.0;
    }

protected:
    double pathLengthPx(const EffectInput& in) const override
    {
        float u = 0, v = 0;
        centre(in, &u, &v);
        return angle(in) * maxRadiusPx(in, u, v);
    }
    void setPath(GlKit& kit, const EffectInput& in, double fraction) override
    {
        float u = 0, v = 0;
        centre(in, &u, &v);
        kit.gl()->glUniform1i(m_kind, 1);
        kit.gl()->glUniform1f(m_angle, float(angle(in) * fraction));
        kit.gl()->glUniform2f(m_centre, u, v);
    }

private:
    static double angle(const EffectInput& in)
    {
        return in.params->value(k::Angle, in.localFrame, 10.0) * std::numbers::pi / 180.0;
    }
    static void centre(const EffectInput& in, float* u, float* v)
    {
        layerGeometry(in).centre(in, in.params->value(k::CenterX, in.localFrame, 0.0),
            in.params->value(k::CenterY, in.localFrame, 0.0), u, v);
    }
};

class ZoomBlur final : public PathBlur
{
public:
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return p.value(k::Amount, f, 20.0) <= 0.0;
    }

protected:
    double pathLengthPx(const EffectInput& in) const override
    {
        float u = 0, v = 0;
        centre(in, &u, &v);
        return amount(in) * maxRadiusPx(in, u, v);
    }
    void setPath(GlKit& kit, const EffectInput& in, double fraction) override
    {
        float u = 0, v = 0;
        centre(in, &u, &v);
        kit.gl()->glUniform1i(m_kind, 2);
        kit.gl()->glUniform1f(m_logScale, float(std::log(1.0 - amount(in)) * fraction));
        kit.gl()->glUniform2f(m_centre, u, v);
    }

private:
    static double amount(const EffectInput& in)
    {
        return std::clamp(
            in.params->value(k::Amount, in.localFrame, 20.0) / 100.0, 0.0, kMaxZoomAmount);
    }
    static void centre(const EffectInput& in, float* u, float* v)
    {
        layerGeometry(in).centre(in, in.params->value(k::CenterX, in.localFrame, 0.0),
            in.params->value(k::CenterY, in.localFrame, 0.0), u, v);
    }
};

const GlEffectFactory kBlurTable[] = {
    {fxid::BoxBlur, &make<BoxBlur>},
    {fxid::DirectionalBlur, &make<DirectionalBlur>},
    {fxid::RadialBlur, &make<RadialBlur>},
    {fxid::ZoomBlur, &make<ZoomBlur>},
};

} // namespace

std::span<const GlEffectFactory> blurGlEffects()
{
    return kBlurTable;
}

} // namespace kk::render
