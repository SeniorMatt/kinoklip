// SPDX-License-Identifier: GPL-3.0-or-later
// GlEffectBase — shared building blocks of the GPU effect files (internal to src/render):
// PassEffect (one fragment program over the layer), parameter helpers, the factory template
// and GLSL snippets every v4 effect uses.
#pragma once

#include "render/EffectMath.h"
#include "render/GlEffects.h"

#include <QSizeF>

#include <array>
#include <memory>
#include <string>

namespace kk::render::fx {

using fxmath::boolParam;
using fxmath::choiceParam;
using fxmath::colorParam;

/// Gaussian sigma for a "radius" parameter (radius = 2σ, like gaussian_blur).
inline constexpr double kSigmaPerRadius = 0.5;
/// Layer pixels below which a blur radius or displacement has no visible effect.
inline constexpr double kMinRadiusPx = 0.5;

/// GLSL helpers prepended to the v4 effect shaders: BT.709 luma, straight colour and the
/// keyers' view output (the twin of fxmath::keyOutput()).
inline constexpr char kGlslCommon[] = R"(
const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
float luma(vec3 c) { return dot(c, kLuma); }
// Straight (un-premultiplied) colour of a premultiplied texel; black where transparent.
vec3 straight(vec4 c) { return c.a > 0.0 ? c.rgb / c.a : vec3(0.0); }
// Keyer views: 0 = final (premultiplied, alpha = a · matte), 1 = matte, 2 = status (opaque).
vec4 keyOutput(int view, vec3 c, float a, float matte)
{
    float k = a * matte;
    if (view == 1)
        return vec4(vec3(k), 1.0);
    if (view == 2) {
        float v = k >= 0.98 ? 1.0 : (k <= 0.02 ? 0.0 : 0.5);
        return vec4(vec3(v), 1.0);
    }
    return vec4(clamp(c, 0.0, 1.0) * k, k);
}
)";

/// Factory of GlEffectFactory rows.
template<typename T>
std::unique_ptr<GlEffect> make()
{
    return std::make_unique<T>();
}

/// Content rectangle of a layer in uv units (the layer minus EffectInput::paddingPx).
struct LayerGeometry
{
    float x0 = 0, y0 = 0, x1 = 1, y1 = 1; ///< content corners in uv
    float width = 1, height = 1; ///< layer size in pixels
    /// uv of the content centre moved by (@p dx, @p dy) timeline pixels.
    void centre(const EffectInput& in, double dx, double dy, float* u, float* v) const
    {
        *u = float(0.5 * (x0 + x1) + dx * in.scale / width);
        *v = float(0.5 * (y0 + y1) + dy * in.scale / height);
    }
};

/// Geometry of @p in's layer.
inline LayerGeometry layerGeometry(const EffectInput& in)
{
    LayerGeometry g;
    g.width = float(in.layer->size.width());
    g.height = float(in.layer->size.height());
    g.x0 = float(in.paddingPx / g.width);
    g.y0 = float(in.paddingPx / g.height);
    g.x1 = 1.f - g.x0;
    g.y1 = 1.f - g.y0;
    return g;
}

/// Effect with one program sampling the layer at unit 0 (and optionally a second texture at
/// unit 1). Subclasses set their uniforms between begin() and draw().
class PassEffect : public GlEffect
{
public:
    void destroy(GlKit& kit) override { m_program.destroy(kit.gl()); }

protected:
    /// Compiles @p fragment (with kGlslCommon first when @p common is true).
    bool build(
        GlKit& kit, const char* name, const char* fragment, QString* why, bool common = false)
    {
        const std::string source = common ? std::string(kGlslCommon) + fragment : fragment;
        if (!m_program.build(kit.gl(), name, source.c_str(), why))
            return false;
        kit.gl()->glUseProgram(m_program.id);
        kit.gl()->glUniform1i(m_program.uniform(kit.gl(), "uTex"), 0);
        kit.gl()->glUniform1i(m_program.uniform(kit.gl(), "uSecond"), 1);
        kit.gl()->glUseProgram(0);
        return true;
    }

    GLint uniform(GlKit& kit, const char* name) const { return m_program.uniform(kit.gl(), name); }

    /// Binds the program and a new target of the layer's size (call before setting uniforms).
    PooledTarget begin(GlKit& kit, const RenderTarget& src)
    {
        PooledTarget out = kit.acquire(src.size, src.format);
        kit.bindTarget(*out);
        kit.setBlendOver(false);
        kit.gl()->glUseProgram(m_program.id);
        kit.setFullscreen(m_program);
        return out;
    }

    /// Draws the pass with @p src at unit 0 and @p second (if any) at unit 1.
    void draw(GlKit& kit, const RenderTarget& src, GLuint second = 0)
    {
        kit.bindTexture(0, src.texture);
        if (second)
            kit.bindTexture(1, second);
        kit.drawQuad();
    }

    GlProgram m_program;
};

/// Shared main() of the pointwise colour effects: un-premultiply, apply(), clamp,
/// premultiply. Transparent stays transparent. `uSecond` (unit 1) is free for a look-up
/// texture, `uLut3d` (unit 2) for a 3D one.
inline constexpr char kColorMain[] = R"(
uniform sampler2D uTex;
vec3 apply(vec3 c);
void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    fragColor = vec4(clamp(apply(straight(t)), 0.0, 1.0) * t.a, t.a);
}
)";

/// A pointwise colour effect: GLSL `vec3 apply(vec3 c)` plus up to kMaxUniforms uniforms,
/// filled by a per-effect setter from the ParamSet (which may also bind textures).
class ColorEffect final : public PassEffect
{
public:
    static constexpr int kMaxUniforms = 6;
    using Names = std::array<const char*, kMaxUniforms>;
    using Setter = void (*)(GlKit& kit, const GLint* loc, const EffectInput& in);
    using Identity = bool (*)(const ParamSet& p, int64_t frame);

    ColorEffect(const char* name, const char* body, Names names, Setter setter, Identity identity)
        : m_name(name)
        , m_body(body)
        , m_names(names)
        , m_setter(setter)
        , m_identity(identity)
    { }

    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = std::string(kColorMain) + m_body;
        if (!build(kit, m_name, source.c_str(), why, true))
            return false;
        for (int i = 0; i < kMaxUniforms; ++i)
            m_loc[i] = m_names[size_t(i)] ? uniform(kit, m_names[size_t(i)]) : -1;
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return m_identity && m_identity(p, f);
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        PooledTarget out = begin(kit, *in.layer);
        m_setter(kit, m_loc, in);
        draw(kit, *in.layer);
        return out;
    }

private:
    const char* m_name;
    const char* m_body;
    Names m_names;
    Setter m_setter;
    Identity m_identity;
    GLint m_loc[kMaxUniforms] = {-1, -1, -1, -1, -1, -1};
};

} // namespace kk::render::fx
