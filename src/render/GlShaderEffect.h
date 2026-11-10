// SPDX-License-Identifier: GPL-3.0-or-later
// GlShaderEffect — a generic one-fragment-program effect for the 1.2 batch-4 files (internal to
// src/render): the shader, its uniform names, a setter that fills them from the ParamSet and an
// optional Gaussian blur of the layer bound as `uSecond`. Also the GLSL hash shared by the
// effects with randomness: it depends only on the pixel, the seed and the clip-local frame, so
// exports repeat exactly.
#pragma once

#include "render/GlEffectBase.h"

#include <string>
#include <vector>

namespace kk::render::fx {

/// GLSL integer hash (Wang-style) and value helpers, prepended to every ShaderEffect shader.
inline constexpr char kGlslHash[] = R"(
uint hashU(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float hash01(uint x) { return float(hashU(x) & 0xffffffu) / 16777215.0; }
float hash2(ivec2 p, uint s)
{
    return hash01(uint(p.x) * 73856093u ^ uint(p.y) * 19349663u ^ (s * 83492791u + 0x9e3779b9u));
}
float maxAbs(vec3 v) { return max(abs(v.x), max(abs(v.y), abs(v.z))); }
)";

class ShaderEffect final : public PassEffect
{
public:
    using Setter = void (*)(GlKit& kit, const GLint* loc, const EffectInput& in);
    using Identity = bool (*)(const ParamSet& p, int64_t frame);
    /// Gaussian sigma (layer pixels) of the blur bound as uSecond; <= 0 for none.
    using BlurSigma = double (*)(const ParamSet& p, int64_t frame, double scale);

    struct Spec
    {
        const char* name;
        const char* fragment;
        std::vector<const char*> uniforms;
        Setter setter;
        Identity identity = nullptr;
        BlurSigma blur = nullptr;
    };

    explicit ShaderEffect(Spec spec)
        : m_spec(std::move(spec))
    { }

    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = std::string(kGlslHash) + m_spec.fragment;
        if (!build(kit, m_spec.name, source.c_str(), why, true))
            return false;
        for (const char* name : m_spec.uniforms)
            m_loc.push_back(uniform(kit, name));
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return m_spec.identity && m_spec.identity(p, f);
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        PooledTarget blurred;
        if (m_spec.blur) {
            const double sigma = m_spec.blur(*in.params, in.localFrame, in.scale);
            if (sigma >= 0.25)
                blurred = kit.gaussianBlur(*in.layer, sigma);
        }
        PooledTarget out = begin(kit, *in.layer);
        m_spec.setter(kit, m_loc.data(), in);
        draw(kit, *in.layer, blurred ? blurred->texture : in.layer->texture);
        return out;
    }

private:
    Spec m_spec;
    std::vector<GLint> m_loc;
};

/// Parameter value at the effect's frame.
inline double pv(const EffectInput& in, const QString& key, double fallback)
{
    return in.params->value(key, in.localFrame, fallback);
}

} // namespace kk::render::fx
