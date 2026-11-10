// SPDX-License-Identifier: GPL-3.0-or-later
// GlEffects — GPU implementations of the video effects of the EffectRegistry catalogue
// (v0.1: gaussian_blur, brightness_contrast, saturation, sharpen, mirror, vignette, mosaic,
// drop_shadow; v4: the keyers, colour, blur, light, stylize and distort files). To add one:
// implement GlEffect (PassEffect in GlEffectBase.h helps) and add a row to a factory table.
#pragma once

#include "core/Animation.h"
#include "render/GlKit.h"

#include <QSizeF>
#include <QString>

#include <cstdint>
#include <memory>
#include <span>

namespace kk::render {

/// The clip's layer at other clip-local frames, for the effects that read other frames (Echo,
/// Strobe and the multi-frame effects of spec 60, see render/TimeEffects.h): the
/// clip's picture with the effects before the asking effect applied, rendered at the same
/// size and geometry as EffectInput::layer. Provided by the GPU renderer (GlRenderer), which
/// decodes the frames timefx::clipFrames() lists before drawing.
class LayerHistory
{
public:
    virtual ~LayerHistory() = default;
    /// The layer at @p localFrame; an empty handle if it cannot be rendered (not decoded).
    [[nodiscard]] virtual PooledTarget layerAt(int64_t localFrame) = 0;
};

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
    /// Transparent margin around the content on each side, in layer pixels (the sum of the
    /// active effects' paddingPx() times scale). The content is the layer minus this margin.
    double paddingPx = 0.0;
    double fps = 25.0; ///< timeline frame rate (animated effects: speed in cycles per second)
    /// Earlier frames of the clip (time effects); null where the renderer has none.
    LayerHistory* history = nullptr;
    /// The clip's source time at this frame, seconds (the key of stored tracks, spec 70).
    double sourceSeconds = 0.0;
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
    /// paddingPx() for content of @p contentPx (timeline px): effects whose margin depends
    /// on the clip's size (radial and zoom blurs) override this one. Default: paddingPx().
    [[nodiscard]] virtual double contentPaddingPx(
        const ParamSet& params, int64_t localFrame, QSizeF contentPx) const
    {
        Q_UNUSED(contentPx);
        return paddingPx(params, localFrame);
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

/// All built-in GPU effects (the single registration point: the v0.1 table in GlEffects.cpp
/// plus the tables of GlKeyers.cpp, GlColorEffects.cpp, GlBlurEffects.cpp,
/// GlStylizeEffects.cpp and GlDistortEffects.cpp).
[[nodiscard]] std::span<const GlEffectFactory> builtinGlEffects();

/// The factory of effect @p id, or nullptr when the id has none.
[[nodiscard]] const GlEffectFactory* findGlEffectFactory(const QString& id);

/// Registration tables of the v4 effect files (see builtinGlEffects()).
[[nodiscard]] std::span<const GlEffectFactory> keyerGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> colorGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> blurGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> stylizeGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> distortGlEffects();
/// Batch 2: GlGradeEffects.cpp, GlFocusEffects.cpp, GlLightEffects.cpp, GlLookEffects.cpp,
/// GlWarpEffects.cpp, GlTimeEffects.cpp.
[[nodiscard]] std::span<const GlEffectFactory> gradeGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> focusGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> lightGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> lookGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> warpGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> timeGlEffects();
/// 1.2 (spec 56): GlExtraColor.cpp, GlExtraEffects.cpp.
[[nodiscard]] std::span<const GlEffectFactory> extraColorGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> extraGlEffects();
/// 1.2 second part (spec 57): GlDetailEffects.cpp, GlDamageEffects.cpp, GlPaintEffects.cpp,
/// GlOpticsEffects.cpp, GlUtilityEffects.cpp.
[[nodiscard]] std::span<const GlEffectFactory> detailGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> damageGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> paintGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> opticsGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> utilityGlEffects();
/// 1.2 multi-frame effects (spec 60): GlTemporalEffects.cpp, GlRepairEffects.cpp.
[[nodiscard]] std::span<const GlEffectFactory> temporalGlEffects();
[[nodiscard]] std::span<const GlEffectFactory> repairGlEffects();
/// CCTV, Digital Glitch, Drone Overlay, DSLR, DVE, Video Call, Video Camera (GlTemplateLooks.cpp).
[[nodiscard]] std::span<const GlEffectFactory> templateLookGlEffects();
/// Tracking effects (GlMeshEffects.cpp, spec 70).
[[nodiscard]] std::span<const GlEffectFactory> meshGlEffects();

} // namespace kk::render
