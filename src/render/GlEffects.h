// SPDX-License-Identifier: GPL-3.0-or-later
// GlEffects — GPU implementations of the video effects of the EffectRegistry catalogue
// (gaussian_blur, brightness_contrast, saturation, sharpen, mirror, vignette, mosaic,
// drop_shadow). To add one: implement GlEffect and add a row to kEffectTable in GlEffects.cpp.
#pragma once

#include "core/Animation.h"
#include "render/GlKit.h"

#include <QString>

#include <cstdint>
#include <memory>
#include <span>

namespace kk::render {

/// Input of one effect application.
///
/// The "layer" is the clip's picture before its transform, at layer resolution: `scale`
/// layer pixels per timeline pixel. Effect parameters given in pixels are timeline pixels and
/// must be multiplied by `scale` (so a 10 px blur looks the same at every output size and
/// preview quality). The layer may carry a transparent margin (see GlEffect::paddingPx()).
struct EffectInput
{
    const RenderTarget* layer = nullptr; ///< premultiplied RGBA16F
    double scale = 1.0; ///< layer pixels per timeline pixel
    int64_t localFrame = 0; ///< clip-local frame for ParamSet::value()
    const ParamSet* params = nullptr;
};

/// One video effect, implemented as one or more fragment-shader passes over the layer.
/// Instances live on the render thread for the renderer's lifetime (stateless per frame).
class GlEffect
{
public:
    virtual ~GlEffect() = default;
    /// Compiles programs. @return false with a reason (the renderer then skips the effect).
    virtual bool initialize(GlKit& kit, QString* why) = 0;
    virtual void destroy(GlKit& kit) = 0;
    /// Transparent margin (timeline pixels) the effect needs around the content so it is not
    /// clipped (e.g. a drop shadow falling outside the clip). Default 0.
    [[nodiscard]] virtual double paddingPx(const ParamSet& params, int64_t localFrame) const
    {
        Q_UNUSED(params);
        Q_UNUSED(localFrame);
        return 0.0;
    }
    /// True if the effect leaves the picture unchanged with these parameters (skipped).
    [[nodiscard]] virtual bool isIdentity(const ParamSet& params, int64_t localFrame) const
    {
        Q_UNUSED(params);
        Q_UNUSED(localFrame);
        return false;
    }
    /// Renders the effect of `in.layer` into a new pooled target of the same size (or returns
    /// an empty handle to keep the input unchanged).
    [[nodiscard]] virtual PooledTarget apply(GlKit& kit, const EffectInput& in) = 0;
};

/// Registration row: descriptor id (EffectRegistry, BuiltinEffects.cpp) → factory.
struct GlEffectFactory
{
    const char* id;
    std::unique_ptr<GlEffect> (*create)();
};

/// All built-in GPU effects (the single registration point).
[[nodiscard]] std::span<const GlEffectFactory> builtinGlEffects();

} // namespace kk::render
