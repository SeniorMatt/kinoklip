// SPDX-License-Identifier: GPL-3.0-or-later
// GlTemporalBase — building blocks of the effects that read other frames of their clip (spec
// 60; internal to src/render): FrameFetch collects the neighbouring layers an effect asked for
// through EffectInput::history and stands in the nearest available frame for one that could not
// be rendered; the helpers reduce a layer to a small picture, measure its mean colour into a
// strip of texels, and add layers up with weights.
#pragma once

#include "render/GlEffectBase.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>

namespace kk::render::fx {

/// The layers of @p frames (clip-local frames, as listed by timefx::framesRead()), fetched
/// through EffectInput::history. A frame that cannot be rendered (not decoded in time, no
/// history in this context) is replaced by the nearest available frame in time, and by the
/// effect's own layer when none is available. The frame the effect draws (EffectInput::
/// localFrame) is the effect's layer itself, without a second render.
class FrameFetch
{
public:
    FrameFetch(const EffectInput& in, const std::vector<int64_t>& frames)
        : m_frames(frames)
        , m_own(frames.size())
        , m_layer(frames.size(), nullptr)
        , m_exact(frames.size(), false)
    {
        for (size_t i = 0; i < frames.size(); ++i) {
            if (frames[i] == in.localFrame) {
                m_layer[i] = in.layer;
                m_exact[i] = true;
                continue;
            }
            for (size_t j = 0; j < i; ++j) { // the same frame twice: one render
                if (frames[j] == frames[i] && m_exact[j]) {
                    m_layer[i] = m_layer[j];
                    m_exact[i] = true;
                    break;
                }
            }
            if (m_layer[i] || !in.history)
                continue;
            m_own[i] = in.history->layerAt(frames[i]);
            if (m_own[i] && m_own[i]->size == in.layer->size) {
                m_layer[i] = m_own[i].get();
                m_exact[i] = true;
            }
        }
        for (size_t i = 0; i < frames.size(); ++i) {
            if (m_layer[i])
                continue;
            int64_t best = std::numeric_limits<int64_t>::max();
            m_layer[i] = in.layer;
            for (size_t j = 0; j < frames.size(); ++j) {
                const int64_t d = std::llabs(frames[j] - frames[i]);
                if (m_exact[j] && d < best) {
                    best = d;
                    m_layer[i] = m_layer[j];
                }
            }
        }
    }
    FrameFetch(const FrameFetch&) = delete;
    FrameFetch& operator=(const FrameFetch&) = delete;

    [[nodiscard]] size_t size() const { return m_layer.size(); }
    /// The layer of frames[i] (never null).
    [[nodiscard]] const RenderTarget* at(size_t i) const { return m_layer[i]; }
    /// True if frames[i] itself was rendered (not stood in for).
    [[nodiscard]] bool exact(size_t i) const { return m_exact[i]; }

private:
    std::vector<int64_t> m_frames;
    std::vector<PooledTarget> m_own;
    std::vector<const RenderTarget*> m_layer;
    std::vector<bool> m_exact;
};

/// Halves @p src (2 x 2 box, one bilinear tap per step) until both edges are under
/// @p maxEdge; the result is @p src itself (empty handle) when it is small already.
[[nodiscard]] inline PooledTarget reduceTo(GlKit& kit, const RenderTarget& src, int maxEdge = 128)
{
    PooledTarget scaled;
    const RenderTarget* from = &src;
    while (from->size.width() >= maxEdge || from->size.height() >= maxEdge) {
        const QSize half(std::max(1, from->size.width() / 2), std::max(1, from->size.height() / 2));
        PooledTarget next = kit.resample(*from, half);
        scaled = std::move(next);
        from = scaled.get();
    }
    return scaled;
}

/// Sum of layers: out = sum of weights[i] * layers[i] (premultiplied, so weights that add up
/// to 1 keep a still picture unchanged). New target of the first layer's size.
[[nodiscard]] inline PooledTarget weightedLayers(
    GlKit& kit, const std::vector<const RenderTarget*>& layers, const std::vector<float>& weights)
{
    PooledTarget out = kit.acquire(layers.front()->size, layers.front()->format);
    kit.bindTarget(*out);
    kit.clearTransparent();
    kit.setBlendOver(true);
    kit.gl()->glBlendFunc(GL_ONE, GL_ONE);
    const QTransform full(2, 0, 0, 2, -1, -1); // unit quad -> the whole target
    for (size_t i = 0; i < layers.size(); ++i) {
        if (weights[i] > 0.f)
            kit.blit(layers[i]->texture, full, weights[i]);
    }
    kit.gl()->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // the renderer's default
    kit.setBlendOver(false);
    return out;
}

/// GLSL of the analysis pass below: the mean premultiplied colour (rgb) and alpha of the
/// texels of uTex inside the uv rectangle uRegion (x0, y0, x1, y1).
inline constexpr char kMeanShader[] = R"(
uniform sampler2D uTex;
uniform vec4 uRegion;
void main()
{
    ivec2 size = textureSize(uTex, 0);
    ivec2 lo = clamp(ivec2(floor(uRegion.xy * vec2(size))), ivec2(0), size - 1);
    ivec2 hi = clamp(ivec2(ceil(uRegion.zw * vec2(size))), lo + 1, size);
    vec4 sum = vec4(0.0);
    for (int y = lo.y; y < hi.y; ++y)
        for (int x = lo.x; x < hi.x; ++x)
            sum += texelFetch(uTex, ivec2(x, y), 0);
    fragColor = sum / float((hi.x - lo.x) * (hi.y - lo.y));
}
)";

/// Measures the mean colour of layers: one texel per layer in a strip target.
class MeanProbe
{
public:
    bool initialize(GlKit& kit, QString* why)
    {
        if (!m_program.build(kit.gl(), "temporal_mean", kMeanShader, why))
            return false;
        kit.gl()->glUseProgram(m_program.id);
        kit.gl()->glUniform1i(m_program.uniform(kit.gl(), "uTex"), 0);
        m_region = m_program.uniform(kit.gl(), "uRegion");
        kit.gl()->glUseProgram(0);
        return true;
    }
    void destroy(GlKit& kit) { m_program.destroy(kit.gl()); }

    /// Texel i of the result = mean premultiplied rgba of @p layers[i] inside @p region (uv of
    /// the layer, y down like the textures). The layers are reduced to a small picture first.
    [[nodiscard]] PooledTarget measure(
        GlKit& kit, const std::vector<const RenderTarget*>& layers, const QRectF& region)
    {
        GlFunctions* gl = kit.gl();
        PooledTarget strip = kit.acquire(QSize(int(layers.size()), 1), GL_RGBA16F);
        for (size_t i = 0; i < layers.size(); ++i) {
            PooledTarget small = reduceTo(kit, *layers[i]);
            const RenderTarget& src = small ? *small : *layers[i];
            kit.bindTarget(*strip);
            gl->glViewport(int(i), 0, 1, 1);
            kit.setBlendOver(false);
            gl->glUseProgram(m_program.id);
            kit.setFullscreen(m_program);
            gl->glUniform4f(m_region, float(region.left()), float(region.top()),
                float(region.right()), float(region.bottom()));
            kit.bindTexture(0, src.texture);
            kit.drawQuad();
        }
        return strip;
    }

private:
    GlProgram m_program;
    GLint m_region = -1;
};

} // namespace kk::render::fx
