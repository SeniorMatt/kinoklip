// SPDX-License-Identifier: GPL-3.0-or-later
// GlGradeEffects — the second batch of pointwise colour effects on the GPU: Curves (a
// 256-entry composite table texture), HSL Adjust, Apply LUT (a 3D texture or a 1D table),
// Vibrance, Channel Mixer and Duotone. One pass each over the straight colour (alpha
// unchanged); formulas and parameter resolution are shared with render/EffectMathGrade.cpp.
#include "core/CurveParam.h"
#include "core/VideoEffectKeys.h"
#include "render/CubeLut.h"
#include "render/GlEffectBase.h"
#include "render/RenderLog.h"

#include <QSet>

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

/// GLSL twins of fxmath::toHsl() / fromHsl().
const char* const kGlslHsl = R"(
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
vec3 fromHsl(vec3 hsl)
{
    float h = mod(hsl.x, 360.0);
    float c = (1.0 - abs(2.0 * hsl.z - 1.0)) * hsl.y;
    float hp = h / 60.0;
    float x = c * (1.0 - abs(mod(hp, 2.0) - 1.0));
    vec3 r;
    if (hp < 1.0) r = vec3(c, x, 0.0);
    else if (hp < 2.0) r = vec3(x, c, 0.0);
    else if (hp < 3.0) r = vec3(0.0, c, x);
    else if (hp < 4.0) r = vec3(0.0, x, c);
    else if (hp < 5.0) r = vec3(x, 0.0, c);
    else r = vec3(c, 0.0, x);
    return r + (hsl.z - 0.5 * c);
}
)";

/// Creates a linear-filtered, edge-clamped texture of @p target type.
GLuint makeTableTexture(GlFunctions* gl, GLenum target)
{
    GLuint tex = 0;
    gl->glGenTextures(1, &tex);
    gl->glBindTexture(target, tex);
    gl->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (target == GL_TEXTURE_3D)
        gl->glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    gl->glBindTexture(target, 0);
    return tex;
}

// ---- curves ----

class Curves final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = std::string(kColorMain) + R"(
uniform sampler2D uSecond; // 256 x 1: composite curve per channel in r, g, b
uniform float uAmount;
float lookup(float x, int channel)
{
    // Texel centres: entry i of 256 at (i + 0.5) / 256 (linear filter = linear interpolation).
    vec4 t = texture(uSecond, vec2((clamp(x, 0.0, 1.0) * 255.0 + 0.5) / 256.0, 0.5));
    return channel == 0 ? t.r : (channel == 1 ? t.g : t.b);
}
vec3 apply(vec3 c)
{
    vec3 curved = vec3(lookup(c.r, 0), lookup(c.g, 1), lookup(c.b, 2));
    return mix(c, curved, uAmount);
}
)";
        if (!build(kit, "curves", source.c_str(), why, true))
            return false;
        m_amount = uniform(kit, "uAmount");
        m_table = makeTableTexture(kit.gl(), GL_TEXTURE_2D);
        return true;
    }
    void destroy(GlKit& kit) override
    {
        if (m_table)
            kit.gl()->glDeleteTextures(1, &m_table);
        m_table = 0;
        PassEffect::destroy(kit);
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        if (val(p, k::Amount, f, 100) <= 0.0)
            return true;
        for (const QString* key : {&k::MasterCurve, &k::RedCurve, &k::GreenCurve, &k::BlueCurve}) {
            if (!curve::isIdentity(curve::parse(p.staticValue(*key).toString())))
                return false;
        }
        return true;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        GlFunctions* gl = kit.gl();
        const ParamSet& p = *in.params;
        const QString key = p.staticValue(k::MasterCurve).toString() + QLatin1Char('|')
            + p.staticValue(k::RedCurve).toString() + QLatin1Char('|')
            + p.staticValue(k::GreenCurve).toString() + QLatin1Char('|')
            + p.staticValue(k::BlueCurve).toString();
        if (key != m_uploaded) {
            // The curves changed (or another clip's curves are drawn): rebuild the table.
            fxmath::CurvesParams cp;
            fxmath::curvesParams(p, in.localFrame, &cp);
            float rgba[fxmath::kCurveTableSize * 4];
            for (int i = 0; i < fxmath::kCurveTableSize; ++i) {
                rgba[i * 4 + 0] = cp.table[0][i];
                rgba[i * 4 + 1] = cp.table[1][i];
                rgba[i * 4 + 2] = cp.table[2][i];
                rgba[i * 4 + 3] = 1.f;
            }
            gl->glBindTexture(GL_TEXTURE_2D, m_table);
            gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, fxmath::kCurveTableSize, 1, 0, GL_RGBA,
                GL_FLOAT, rgba);
            gl->glBindTexture(GL_TEXTURE_2D, 0);
            m_uploaded = key;
        }
        PooledTarget out = begin(kit, *in.layer);
        gl->glUniform1f(
            m_amount, float(std::clamp(val(p, k::Amount, in.localFrame, 100) / 100.0, 0.0, 1.0)));
        draw(kit, *in.layer, m_table);
        return out;
    }

private:
    GLuint m_table = 0;
    QString m_uploaded; ///< curve strings of the table in m_table
    GLint m_amount = -1;
};

// ---- apply_lut ----

class ApplyLut final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = std::string(kColorMain) + R"(
uniform sampler2D uSecond;  // 1D table: N x 1
uniform sampler3D uLut3d;   // 3D table: N x N x N (unit 2)
uniform int uIs3d;
uniform float uN;           // entries per axis
uniform vec3 uDomainMin;
uniform vec3 uDomainSize;   // max - min
uniform float uAmount;
vec3 apply(vec3 c)
{
    vec3 u = clamp((c - uDomainMin) / uDomainSize, 0.0, 1.0);
    vec3 t = (u * (uN - 1.0) + 0.5) / uN; // texel centres
    vec3 r;
    if (uIs3d == 1) {
        r = texture(uLut3d, t).rgb;
    } else {
        r = vec3(texture(uSecond, vec2(t.r, 0.5)).r, texture(uSecond, vec2(t.g, 0.5)).g,
            texture(uSecond, vec2(t.b, 0.5)).b);
    }
    return mix(c, r, uAmount);
}
)";
        if (!build(kit, "apply_lut", source.c_str(), why, true))
            return false;
        GlFunctions* gl = kit.gl();
        gl->glUseProgram(m_program.id);
        gl->glUniform1i(uniform(kit, "uLut3d"), 2);
        gl->glUseProgram(0);
        m_is3d = uniform(kit, "uIs3d");
        m_n = uniform(kit, "uN");
        m_domainMin = uniform(kit, "uDomainMin");
        m_domainSize = uniform(kit, "uDomainSize");
        m_amount = uniform(kit, "uAmount");
        m_tex1d = makeTableTexture(gl, GL_TEXTURE_2D);
        m_tex3d = makeTableTexture(gl, GL_TEXTURE_3D);
        return true;
    }
    void destroy(GlKit& kit) override
    {
        GlFunctions* gl = kit.gl();
        for (GLuint* t : {&m_tex1d, &m_tex3d}) {
            if (*t)
                gl->glDeleteTextures(1, t);
            *t = 0;
        }
        m_uploaded.reset();
        PassEffect::destroy(kit);
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 100) <= 0.0 || p.staticValue(k::LutFile).toString().isEmpty();
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const QString path = in.params->staticValue(k::LutFile).toString();
        QString why;
        const std::shared_ptr<const CubeLut> lut = loadCubeLut(path, &why);
        if (!lut) {
            if (!m_warned.contains(path)) {
                m_warned.insert(path);
                qCWarning(lcRender) << "Apply LUT:" << why;
            }
            return {}; // missing / broken file: the picture passes unchanged
        }
        GlFunctions* gl = kit.gl();
        if (lut != m_uploaded)
            upload(gl, lut); // a new or changed file, or another clip's LUT
        PooledTarget out = begin(kit, *in.layer);
        gl->glUniform1i(m_is3d, lut->is3d() ? 1 : 0);
        gl->glUniform1f(m_n, float(lut->size()));
        const float* mn = lut->domainMin();
        const float* mx = lut->domainMax();
        gl->glUniform3f(m_domainMin, mn[0], mn[1], mn[2]);
        gl->glUniform3f(m_domainSize, mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
        gl->glUniform1f(m_amount,
            float(std::clamp(val(*in.params, k::Amount, in.localFrame, 100) / 100.0, 0.0, 1.0)));
        gl->glActiveTexture(GL_TEXTURE2);
        gl->glBindTexture(GL_TEXTURE_3D, m_tex3d);
        draw(kit, *in.layer, m_tex1d);
        gl->glActiveTexture(GL_TEXTURE2);
        gl->glBindTexture(GL_TEXTURE_3D, 0);
        gl->glActiveTexture(GL_TEXTURE0);
        return out;
    }

private:
    void upload(GlFunctions* gl, const std::shared_ptr<const CubeLut>& lut)
    {
        // RGB rows as float textures; 3D tables are trilinear-filtered by the hardware.
        const int n = lut->size();
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (lut->is3d()) {
            gl->glBindTexture(GL_TEXTURE_3D, m_tex3d);
            gl->glTexImage3D(
                GL_TEXTURE_3D, 0, GL_RGB32F, n, n, n, 0, GL_RGB, GL_FLOAT, lut->data().constData());
            gl->glBindTexture(GL_TEXTURE_3D, 0);
        } else {
            gl->glBindTexture(GL_TEXTURE_2D, m_tex1d);
            gl->glTexImage2D(
                GL_TEXTURE_2D, 0, GL_RGB32F, n, 1, 0, GL_RGB, GL_FLOAT, lut->data().constData());
            gl->glBindTexture(GL_TEXTURE_2D, 0);
        }
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        m_uploaded = lut;
    }

    GLuint m_tex1d = 0, m_tex3d = 0;
    std::shared_ptr<const CubeLut> m_uploaded; ///< the table in the textures (kept alive)
    QSet<QString> m_warned; ///< paths already reported (one warning each)
    GLint m_is3d = -1, m_n = -1, m_domainMin = -1, m_domainSize = -1, m_amount = -1;
};

// ---- hsl_adjust ----

const char* const kHslAdjust = R"(
uniform vec4 uRange;  // all (1 / 0), centre, half width, softness (degrees)
uniform vec3 uAdjust; // hue shift (degrees), saturation, lightness (-1..1)
float weight(vec3 hsl)
{
    if (uRange.x > 0.5)
        return 1.0;
    float dh = abs(mod(hsl.x - uRange.y + 540.0, 360.0) - 180.0);
    return (1.0 - smoothstep(uRange.z, uRange.z + uRange.w, dh)) * smoothstep(0.02, 0.12, hsl.y);
}
vec3 apply(vec3 c)
{
    vec3 hsl = toHsl(c);
    float w = weight(hsl);
    if (w <= 0.0)
        return c;
    hsl.x += uAdjust.x * w;
    hsl.y = clamp(hsl.y * (1.0 + uAdjust.y * w), 0.0, 1.0);
    float dl = uAdjust.z * w;
    hsl.z = dl >= 0.0 ? hsl.z + (1.0 - hsl.z) * dl : hsl.z * (1.0 + dl);
    return fromHsl(hsl);
}
)";

std::unique_ptr<GlEffect> makeHslAdjust()
{
    static const std::string body = std::string(kGlslHsl) + kHslAdjust;
    return std::make_unique<ColorEffect>(
        "hsl_adjust", body.c_str(), ColorEffect::Names {"uRange", "uAdjust"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const fxmath::HslAdjustParams p = fxmath::hslAdjustParams(*in.params, in.localFrame);
            kit.gl()->glUniform4f(loc[0], p.all ? 1.f : 0.f, p.centre, p.halfWidth, p.softness);
            kit.gl()->glUniform3f(loc[1], p.hueShift, p.saturation, p.lightness);
        },
        [](const ParamSet& p, int64_t f) {
            return val(p, k::HueShift, f, 0) == 0.0 && val(p, k::Saturation, f, 0) == 0.0
                && val(p, k::Lightness, f, 0) == 0.0;
        });
}

// ---- vibrance ----

const char* const kVibrance = R"(
uniform vec2 uVibrance; // vibrance, saturation (-1..1)
vec3 apply(vec3 c)
{
    float y = luma(c);
    float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    float dh = abs(mod(toHsl(c).x - 25.0 + 540.0, 360.0) - 180.0);
    float skin = 1.0 - smoothstep(0.0, 25.0, dh);
    float guard = uVibrance.x > 0.0 ? 1.0 - 0.5 * skin : 1.0;
    float k = (1.0 + uVibrance.x * (1.0 - sat) * guard) * (1.0 + uVibrance.y);
    return y + (c - y) * k;
}
)";

std::unique_ptr<GlEffect> makeVibrance()
{
    static const std::string body = std::string(kGlslHsl) + kVibrance;
    return std::make_unique<ColorEffect>(
        "vibrance", body.c_str(), ColorEffect::Names {"uVibrance"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const fxmath::VibranceParams p = fxmath::vibranceParams(*in.params, in.localFrame);
            kit.gl()->glUniform2f(loc[0], p.vibrance, p.saturation);
        },
        [](const ParamSet& p, int64_t f) {
            return val(p, k::Vibrance, f, 30) == 0.0 && val(p, k::Saturation, f, 0) == 0.0;
        });
}

// ---- channel_mixer ----

const char* const kChannelMixer = R"(
uniform mat3 uMix; // column-major upload of the row-major matrix, transposed on upload
vec3 apply(vec3 c) { return uMix * c; }
)";

std::unique_ptr<GlEffect> makeChannelMixer()
{
    return std::make_unique<ColorEffect>(
        "channel_mixer", kChannelMixer, ColorEffect::Names {"uMix"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const fxmath::ChannelMixerParams p
                = fxmath::channelMixerParams(*in.params, in.localFrame);
            kit.gl()->glUniformMatrix3fv(loc[0], 1, GL_TRUE, p.m); // rows -> transpose
        },
        [](const ParamSet& p, int64_t f) {
            const fxmath::ChannelMixerParams m = fxmath::channelMixerParams(p, f);
            const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            return std::equal(std::begin(m.m), std::end(m.m), std::begin(id));
        });
}

// ---- duotone ----

const char* const kDuotone = R"(
uniform vec3 uShadow;
uniform vec3 uHighlight;
uniform float uAmount;
vec3 apply(vec3 c)
{
    return mix(c, mix(uShadow, uHighlight, clamp(luma(c), 0.0, 1.0)), uAmount);
}
)";

std::unique_ptr<GlEffect> makeDuotone()
{
    return std::make_unique<ColorEffect>(
        "duotone", kDuotone, ColorEffect::Names {"uShadow", "uHighlight", "uAmount"},
        [](GlKit& kit, const GLint* loc, const EffectInput& in) {
            const fxmath::DuotoneParams p = fxmath::duotoneParams(*in.params, in.localFrame);
            kit.gl()->glUniform3f(loc[0], p.shadow.r, p.shadow.g, p.shadow.b);
            kit.gl()->glUniform3f(loc[1], p.highlight.r, p.highlight.g, p.highlight.b);
            kit.gl()->glUniform1f(loc[2], p.amount);
        },
        [](const ParamSet& p, int64_t f) { return val(p, k::Amount, f, 100) <= 0.0; });
}

const GlEffectFactory kGradeTable[] = {
    {fxid::Curves, &make<Curves>},
    {fxid::HslAdjust, &makeHslAdjust},
    {fxid::ApplyLut, &make<ApplyLut>},
    {fxid::Vibrance, &makeVibrance},
    {fxid::ChannelMixer, &makeChannelMixer},
    {fxid::Duotone, &makeDuotone},
};

} // namespace

std::span<const GlEffectFactory> gradeGlEffects()
{
    return kGradeTable;
}

} // namespace kk::render
