// SPDX-License-Identifier: GPL-3.0-or-later
// GlTemporalEffects — Motion Trails, Smear, Frame Replacer and Motion Blur (spec 60). All four
// read other frames of their clip through EffectInput::history (the frames timefx::framesRead()
// lists, decoded by GlRenderer before drawing), so the viewer, playback and export draw the
// same picture for the same frame; nothing accumulates across frames. A neighbour that could not
// be rendered is replaced by the nearest available frame (FrameFetch).
//
// Motion Trails and Smear add the neighbouring layers up (weights that sum to 1, or a per-channel
// maximum for the Lighten trails). Frame Replacer mixes the layer with its neighbours. Motion
// Blur estimates the motion with the optical-flow search of GlRetime (forward to the next frame,
// backward from the previous one) and averages the layer along it.
#include "core/VideoEffectKeys.h"
#include "render/EffectMathTemporal.h"
#include "render/GlRetime.h"
#include "render/GlTemporalBase.h"
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

/// Base of the effects that need no program of their own.
class BlitEffect : public GlEffect
{
public:
    bool initialize(GlKit&, QString*) override { return true; }
    void destroy(GlKit&) override { }
};

/// The frames @p type reads for @p in and their layers.
std::vector<int64_t> framesOf(const char* type, const EffectInput& in)
{
    std::vector<int64_t> frames;
    timefx::framesRead(QLatin1String(type), *in.params, in.localFrame, &frames);
    return frames;
}

// ---- motion_trails ----

// Lighten trails: the newest light wins. A trail frame counts as its own colour mixed toward the
// current frame by its weight, so an old trail fades into the picture and not into black.
const char* const kLighten = R"(
uniform sampler2D uAcc;
uniform sampler2D uTrail;
uniform sampler2D uCur;
uniform float uWeight;
void main()
{
    fragColor = max(texture(uAcc, vUv), mix(texture(uCur, vUv), texture(uTrail, vUv), uWeight));
}
)";

class MotionTrails final : public GlEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        GlFunctions* gl = kit.gl();
        if (!m_lighten.build(gl, "motion_trails_lighten", kLighten, why))
            return false;
        gl->glUseProgram(m_lighten.id);
        gl->glUniform1i(m_lighten.uniform(gl, "uAcc"), 0);
        gl->glUniform1i(m_lighten.uniform(gl, "uTrail"), 1);
        gl->glUniform1i(m_lighten.uniform(gl, "uCur"), 2);
        m_weight = m_lighten.uniform(gl, "uWeight");
        gl->glUseProgram(0);
        return true;
    }
    void destroy(GlKit& kit) override { m_lighten.destroy(kit.gl()); }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Opacity, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const std::vector<int64_t> frames = framesOf(fxid::MotionTrails, in);
        if (frames.empty())
            return {};
        FrameFetch fetch(in, frames);
        const ParamSet& p = *in.params;
        const double fade = val(p, k::Decay, in.localFrame, 35) / 100.0;
        const double opacity = std::clamp(val(p, k::Opacity, in.localFrame, 100) / 100.0, 0.0, 1.0);
        const bool lighten
            = fxmath::choiceParam(p, k::TrailMode, fxchoice::Lighten) != fxchoice::Average;
        const std::vector<double> w
            = fxmath::trailWeights(int(fetch.size()), fade, opacity, !lighten);
        if (!lighten) { // weights that sum to 1: a still picture stays as it is
            std::vector<const RenderTarget*> layers {in.layer};
            std::vector<float> weights {float(w[0])};
            for (size_t i = 0; i < fetch.size(); ++i) {
                layers.push_back(fetch.at(i));
                weights.push_back(float(w[i + 1]));
            }
            return weightedLayers(kit, layers, weights);
        }
        GlFunctions* gl = kit.gl();
        PooledTarget acc; // empty: the first pass reads the layer itself
        for (size_t i = 0; i < fetch.size(); ++i) {
            PooledTarget out = kit.acquire(in.layer->size, in.layer->format);
            kit.bindTarget(*out);
            kit.setBlendOver(false);
            gl->glUseProgram(m_lighten.id);
            kit.setFullscreen(m_lighten);
            gl->glUniform1f(m_weight, float(w[i + 1]));
            kit.bindTexture(0, acc ? acc->texture : in.layer->texture);
            kit.bindTexture(1, fetch.at(i)->texture);
            kit.bindTexture(2, in.layer->texture);
            kit.drawQuad();
            acc = std::move(out);
        }
        return acc;
    }

private:
    GlProgram m_lighten;
    GLint m_weight = -1;
};

// ---- smear ----

class Smear final : public BlitEffect
{
public:
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const std::vector<int64_t> frames = framesOf(fxid::Smear, in);
        if (frames.empty())
            return {};
        FrameFetch fetch(in, frames);
        const ParamSet& p = *in.params;
        std::vector<int> distances;
        for (const int64_t f : frames)
            distances.push_back(int(f - in.localFrame));
        const std::vector<double> w
            = fxmath::smearWeights(distances, val(p, k::Decay, in.localFrame, 30) / 100.0,
                std::clamp(val(p, k::Amount, in.localFrame, 100) / 100.0, 0.0, 1.0));
        std::vector<const RenderTarget*> layers {in.layer};
        std::vector<float> weights {float(w[0])};
        for (size_t i = 0; i < fetch.size(); ++i) {
            layers.push_back(fetch.at(i));
            weights.push_back(float(w[i + 1]));
        }
        return weightedLayers(kit, layers, weights);
    }
};

// ---- frame_replacer ----

class FrameReplacer final : public BlitEffect
{
public:
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 100) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const std::vector<int64_t> frames = framesOf(fxid::FrameReplacer, in);
        if (frames.empty())
            return {};
        FrameFetch fetch(in, frames);
        const double amount
            = std::clamp(val(*in.params, k::Amount, in.localFrame, 100) / 100.0, 0.0, 1.0);
        std::vector<const RenderTarget*> layers {in.layer};
        std::vector<float> weights {float(1.0 - amount)};
        for (size_t i = 0; i < fetch.size(); ++i) {
            layers.push_back(fetch.at(i));
            weights.push_back(float(amount / double(fetch.size())));
        }
        return weightedLayers(kit, layers, weights);
    }
};

// ---- motion_blur ----

// Averages the layer along the motion: at time t of the shutter (t in [-s/2, s/2] frames) the
// picture shows the content t frames further along its motion, so the pixel collects the layer
// at p - v t, with the forward motion for t > 0 and the backward one for t < 0.
const char* const kMotionBlur = R"(
uniform sampler2D uTex;
uniform sampler2D uForward;
uniform sampler2D uBackward;
uniform vec2 uInvFlow;   // 1 / flow size
uniform float uShutter;  // exposure in frames
uniform int uSamples;
void main()
{
    // Motion under one flow pixel is below what the search can tell from noise: ignored.
    vec2 f = texture(uForward, vUv).rg;
    vec2 b = texture(uBackward, vUv).rg;
    f *= smoothstep(0.5, 1.0, length(f)) * uInvFlow;
    b *= smoothstep(0.5, 1.0, length(b)) * uInvFlow;
    vec4 sum = vec4(0.0);
    for (int i = 0; i < uSamples; ++i) {
        float t = ((float(i) + 0.5) / float(uSamples) - 0.5) * uShutter;
        vec2 offset = -(t > 0.0 ? f : b) * t;
        sum += texture(uTex, vUv + offset);
    }
    fragColor = sum / float(uSamples);
}
)";

class TemporalMotionBlur final : public GlEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!m_flow.initialize(kit, why))
            return false;
        GlFunctions* gl = kit.gl();
        if (!m_program.build(gl, "temporal_motion_blur", kMotionBlur, why))
            return false;
        gl->glUseProgram(m_program.id);
        gl->glUniform1i(m_program.uniform(gl, "uTex"), 0);
        gl->glUniform1i(m_program.uniform(gl, "uForward"), 1);
        gl->glUniform1i(m_program.uniform(gl, "uBackward"), 2);
        m_invFlow = m_program.uniform(gl, "uInvFlow");
        m_shutter = m_program.uniform(gl, "uShutter");
        m_samples = m_program.uniform(gl, "uSamples");
        gl->glUseProgram(0);
        return true;
    }
    void destroy(GlKit& kit) override
    {
        m_flow.destroy(kit);
        m_program.destroy(kit.gl());
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::ShutterAngle, f, 180) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const std::vector<int64_t> frames = framesOf(fxid::TemporalMotionBlur, in);
        if (frames.size() != 2)
            return {};
        FrameFetch fetch(in, frames); // frames[0] = previous, frames[1] = next
        PooledTarget forward = m_flow.estimateFlow(kit, *in.layer, *fetch.at(1), 0.f);
        PooledTarget backward = m_flow.estimateFlow(kit, *fetch.at(0), *in.layer, 1.f);
        const ParamSet& p = *in.params;
        GlFunctions* gl = kit.gl();
        PooledTarget out = kit.acquire(in.layer->size, in.layer->format);
        kit.bindTarget(*out);
        kit.setBlendOver(false);
        gl->glUseProgram(m_program.id);
        kit.setFullscreen(m_program);
        gl->glUniform2f(
            m_invFlow, 1.0f / float(forward->size.width()), 1.0f / float(forward->size.height()));
        gl->glUniform1f(m_shutter,
            float(std::clamp(val(p, k::ShutterAngle, in.localFrame, 180), 0.0, 720.0) / 360.0));
        gl->glUniform1i(
            m_samples, std::clamp(int(std::lround(val(p, k::Samples, in.localFrame, 12))), 2, 64));
        kit.bindTexture(0, in.layer->texture);
        kit.bindTexture(1, forward->texture);
        kit.bindTexture(2, backward->texture);
        kit.drawQuad();
        return out;
    }

private:
    GlRetime m_flow;
    GlProgram m_program;
    GLint m_invFlow = -1, m_shutter = -1, m_samples = -1;
};

const GlEffectFactory kTable[] = {
    {fxid::MotionTrails, &make<MotionTrails>},
    {fxid::Smear, &make<Smear>},
    {fxid::FrameReplacer, &make<FrameReplacer>},
    {fxid::TemporalMotionBlur, &make<TemporalMotionBlur>},
};

} // namespace

std::span<const GlEffectFactory> temporalGlEffects()
{
    return kTable;
}

} // namespace kk::render
