// SPDX-License-Identifier: GPL-3.0-or-later
// GlBackgroundRemoval — the pixels of the Background Removal effect (spec 78). The matte is made by
// the AI model in the background (matting/MatteService.h); this pass multiplies the layer by it:
// threshold and softness shape the matte, Shrink / Grow moves its edge (minimum / maximum over a
// ring of taps), Feather blurs it. When the matte of the frame is not ready, or the AI tool is
// unavailable, the layer is left as it is (an export waits for the matte first).
#include "core/VideoEffectKeys.h"
#include "matting/MatteService.h"
#include "render/GlEffectBase.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

const char* const kShader = R"(
uniform sampler2D uTex;
uniform sampler2D uSecond;   // the matte, red channel, row 0 = top
uniform vec4 uContent;       // content rectangle of the layer in uv
uniform int uMode;           // 0 foreground, 1 matte only, 2 background only
uniform float uThreshold;
uniform float uWidth;        // half width of the soft step
uniform vec2 uGrow;          // signed shrink / grow radius in content units (x, y)
uniform vec2 uFeather;       // feather radius in content units
float matteAt(vec2 t) { return texture(uSecond, clamp(t, vec2(0.0), vec2(1.0))).r; }
const vec2 kRing[12] = vec2[](vec2(1.0, 0.0), vec2(0.866, 0.5), vec2(0.5, 0.866), vec2(0.0, 1.0),
    vec2(-0.5, 0.866), vec2(-0.866, 0.5), vec2(-1.0, 0.0), vec2(-0.866, -0.5), vec2(-0.5, -0.866),
    vec2(0.0, -1.0), vec2(0.5, -0.866), vec2(0.866, -0.5));
void main()
{
    vec4 c = texture(uTex, vUv);
    vec2 t = (vUv - uContent.xy) / (uContent.zw - uContent.xy);
    float m = matteAt(t);
    if (uGrow.x != 0.0 || uGrow.y != 0.0) {
        bool grow = uGrow.x + uGrow.y > 0.0;
        vec2 r = abs(uGrow);
        for (int i = 0; i < 12; ++i) {
            float a = matteAt(t + kRing[i] * r);
            float b = matteAt(t + kRing[i] * r * 0.5);
            m = grow ? max(m, max(a, b)) : min(m, min(a, b));
        }
    }
    if (uFeather.x > 0.0 || uFeather.y > 0.0) {
        float sum = m;
        for (int i = 0; i < 12; ++i)
            sum += matteAt(t + kRing[i] * uFeather) + matteAt(t + kRing[i] * uFeather * 0.5);
        m = sum / 25.0;
    }
    float a = smoothstep(uThreshold - uWidth, uThreshold + uWidth, m);
    if (uMode == 1)
        fragColor = vec4(vec3(a) * c.a, c.a);
    else if (uMode == 2)
        fragColor = c * (1.0 - a);
    else
        fragColor = c * a;
}
)";

class BackgroundRemoval final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "background_removal", kShader, why))
            return false;
        m_content = uniform(kit, "uContent");
        m_mode = uniform(kit, "uMode");
        m_threshold = uniform(kit, "uThreshold");
        m_width = uniform(kit, "uWidth");
        m_grow = uniform(kit, "uGrow");
        m_feather = uniform(kit, "uFeather");
        return true;
    }

    void destroy(GlKit& kit) override
    {
        if (m_texture)
            kit.gl()->glDeleteTextures(1, &m_texture);
        m_texture = 0;
        m_uploaded.reset();
        PassEffect::destroy(kit);
    }

    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const bool full = choiceParam(p, k::BgQuality, fxchoice::BgPreview) == fxchoice::BgFull;
        const matting::MatteModel model
            = matting::resolveModel(choiceParam(p, k::BgModel, fxchoice::BgModelAuto),
                matting::isStillImagePath(in.mediaPath));
        const matting::MattePtr matte = matting::MatteService::instance().matte(in.mediaPath,
            in.sourceSeconds, full ? matting::MatteQuality::Full : matting::MatteQuality::Preview,
            in.waitForAi, model);
        if (!matte || matte->isNull())
            return {}; // not ready (or unavailable): the picture stays as it is
        upload(kit, matte);

        const QString output = choiceParam(p, k::BgOutput, fxchoice::BgForeground);
        const LayerGeometry g = layerGeometry(in);
        const double w = std::max(1.0, double(g.width) * (g.x1 - g.x0));
        const double h = std::max(1.0, double(g.height) * (g.y1 - g.y0));
        const double shrinkGrow = p.value(k::BgShrinkGrow, in.localFrame, 0.0) * in.scale;
        const double feather = p.value(k::BgFeather, in.localFrame, 0.0) * in.scale;
        const double softness = std::clamp(p.value(k::BgSoftness, in.localFrame, 50.0), 0.0, 100.0);

        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform4f(m_content, g.x0, g.y0, g.x1, g.y1);
        gl->glUniform1i(m_mode,
            output == fxchoice::BgMatte            ? 1
                : output == fxchoice::BgBackground ? 2
                                                   : 0);
        gl->glUniform1f(m_threshold,
            float(std::clamp(p.value(k::BgThreshold, in.localFrame, 50.0), 0.0, 100.0) / 100.0));
        gl->glUniform1f(m_width, float(std::max(softness / 200.0, 1.0 / 512.0)));
        gl->glUniform2f(m_grow, float(shrinkGrow / w), float(shrinkGrow / h));
        gl->glUniform2f(m_feather, float(feather / w), float(feather / h));
        draw(kit, *in.layer, m_texture);
        return out;
    }

private:
    void upload(GlKit& kit, const matting::MattePtr& matte)
    {
        if (m_uploaded == matte && m_texture)
            return;
        GlFunctions* gl = kit.gl();
        if (!m_texture) {
            gl->glGenTextures(1, &m_texture);
            gl->glBindTexture(GL_TEXTURE_2D, m_texture);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        gl->glBindTexture(GL_TEXTURE_2D, m_texture);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(matte->bytesPerLine()));
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, matte->width(), matte->height(), 0, GL_RED,
            GL_UNSIGNED_BYTE, matte->constBits());
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        m_uploaded = matte;
    }

    GLuint m_texture = 0;
    matting::MattePtr m_uploaded;
    GLint m_content = -1, m_mode = -1, m_threshold = -1, m_width = -1, m_grow = -1, m_feather = -1;
};

const GlEffectFactory kTable[] = {
    {fxid::BackgroundRemoval, &make<BackgroundRemoval>},
};

} // namespace

std::span<const GlEffectFactory> backgroundRemovalGlEffects()
{
    return kTable;
}

} // namespace kk::render
