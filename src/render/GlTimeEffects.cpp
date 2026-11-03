// SPDX-License-Identifier: GPL-3.0-or-later
// GlTimeEffects — Echo, Strobe and Flicker on the GPU.
//
// Echo and Strobe read the clip at other frames through EffectInput::history (GlRenderer
// decodes the frames render/TimeEffects.h lists before drawing, so viewer, playback and
// render agree; nothing accumulates across frames). Costs: Strobe = one layer at the held
// frame; Echo = one layer per echo (at most 4: a decode, the effects above it and one
// additive blit each); Flicker = one pointwise pass with a per-frame gain.
#include "core/VideoEffectKeys.h"
#include "render/GlEffectBase.h"
#include "render/TimeEffects.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

double val(const ParamSet& p, const QString& key, int64_t f, double fallback)
{
    return p.value(key, f, fallback);
}

// ---- strobe ----

class Strobe final : public GlEffect
{
public:
    bool initialize(GlKit&, QString*) override { return true; }
    void destroy(GlKit&) override { }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return timefx::strobeFrame(p, f) == f;
    }
    PooledTarget apply(GlKit&, const EffectInput& in) override
    {
        const int64_t held = timefx::strobeFrame(*in.params, in.localFrame);
        if (held == in.localFrame || !in.history)
            return {};
        return in.history->layerAt(held);
    }
};

// ---- echo ----

class Echo final : public GlEffect
{
public:
    bool initialize(GlKit&, QString*) override { return true; }
    void destroy(GlKit&) override { }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Decay, f, 50) >= 100.0 || f <= 0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        if (!in.history)
            return {};
        // Local scratch (not members): an Echo can run inside another Echo's layerAt().
        std::vector<int64_t> frames;
        frames.reserve(4);
        timefx::echoFrames(*in.params, in.localFrame, &frames);
        if (frames.empty())
            return {};
        // Echo i weighs (1 - decay)^i; the weights are normalised, so a still picture
        // stays unchanged and moving parts leave fading trails.
        const double keep
            = 1.0 - std::clamp(val(*in.params, k::Decay, in.localFrame, 50) / 100.0, 0.0, 1.0);
        PooledTarget layers[4];
        double weights[4] = {0, 0, 0, 0};
        int count = 0;
        double total = 1.0;
        double w = 1.0;
        for (const int64_t f : frames) {
            w *= keep;
            if (count == 4)
                break;
            PooledTarget layer = in.history->layerAt(f);
            if (!layer)
                continue;
            layers[count] = std::move(layer);
            weights[count++] = w;
            total += w;
        }
        if (count == 0)
            return {};
        PooledTarget out = kit.acquire(in.layer->size, in.layer->format);
        kit.bindTarget(*out);
        kit.clearTransparent();
        kit.setBlendOver(true);
        kit.gl()->glBlendFunc(GL_ONE, GL_ONE); // additive: the weighted sum
        const QTransform full(2, 0, 0, 2, -1, -1); // unit quad -> the whole target
        kit.blit(in.layer->texture, full, float(1.0 / total));
        for (int i = 0; i < count; ++i)
            kit.blit(layers[i]->texture, full, float(weights[i] / total));
        kit.gl()->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // the renderer's default
        kit.setBlendOver(false);
        return out;
    }
};

// ---- flicker ----

const char* const kFlicker = R"(
uniform float uGain;
vec3 apply(vec3 c) { return c * uGain; }
)";

std::unique_ptr<GlEffect> makeFlicker()
{
    return std::make_unique<ColorEffect>(
        "flicker", kFlicker, ColorEffect::Names {"uGain"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            kit.gl()->glUniform1f(loc[0], fxmath::flickerGain(*in.params, in.localFrame, in.fps));
        },
        [](const ParamSet& p, int64_t f) { return val(p, k::Amount, f, 30) <= 0.0; });
}

const GlEffectFactory kTimeTable[] = {
    {fxid::Echo, &make<Echo>},
    {fxid::Strobe, &make<Strobe>},
    {fxid::Flicker, &makeFlicker},
};

} // namespace

std::span<const GlEffectFactory> timeGlEffects()
{
    return kTimeTable;
}

} // namespace kk::render
