// SPDX-License-Identifier: GPL-3.0-or-later
// GlColorManagement — the GPU path of Color Space Transform, ACES Transform, Gamut Mapping,
// Chromatic Adaptation and Color Compressor (spec 205). One shader runs the whole pipeline of
// cm::CstParams; its GLSL functions are the twins of core/ColorSpaces.cpp (tests compare them
// on the GPU against the CPU path).
#include "core/VideoEffectKeys.h"
#include "render/ColorManagementFx.h"
#include "render/GlEffectBase.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

using namespace fx;

const char* const kCmShader = R"(
uniform sampler2D uTex;
uniform mat3 uM1;
uniform mat3 uM2;
uniform mat3 uM3;
uniform mat3 uFitTo;
uniform mat3 uFitFrom;
uniform mat3 uFitToInv;
uniform mat3 uFitFromInv;
uniform vec4 uOpt1;    // input curve, output curve, tone mapping, gamut mapping
uniform vec4 uOpt2;    // peak in, peak out (units of 100 nits), forward OOTF, inverse OOTF
uniform vec4 uOpt3;    // rrt mode, gamut threshold, limit, power
uniform vec4 uOpt4;    // highlight threshold, limit (0 = off), power, amount
uniform vec3 uLumaIn;
uniform vec3 uLumaOut;

const float PQ_M1 = 0.1593017578125;
const float PQ_M2 = 78.84375;
const float PQ_C1 = 0.8359375;
const float PQ_C2 = 18.8515625;
const float PQ_C3 = 18.6875;

float tfDecode(int t, float x)
{
    if (t == 0) return x;
    if (t == 1) return pow(max(x, 0.0), 2.2);
    if (t == 2) return pow(max(x, 0.0), 2.4);
    if (t == 3) return pow(max(x, 0.0), 2.6);
    if (t == 4) return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
    if (t == 5) return x < 0.081 ? x / 4.5 : pow((x + 0.099) / 1.099, 1.0 / 0.45);
    if (t == 6) {
        float e = pow(max(x, 0.0), 1.0 / PQ_M2);
        return pow(max(e - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * e), 1.0 / PQ_M1) * 100.0;
    }
    if (t == 7) {
        if (x <= 0.5) return x * x / 3.0;
        return (exp((x - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
    }
    if (t == 8) {
        if (x < (9.72 - 15.0) / 17.52) return (exp2(x * 17.52 - 9.72) - exp2(-16.0)) * 2.0;
        return min(exp2(x * 17.52 - 9.72), 65504.0);
    }
    if (t == 9) {
        if (x <= 0.155251141552511) return (x - 0.0729055341958355) / 10.5402377416545;
        return min(exp2(x * 17.52 - 9.72), 65504.0);
    }
    if (t == 10) return x <= 0.02740668 ? x / 10.44426855 : exp2(x / 0.07329248 - 7.0) - 0.0075;
    if (t == 11) {
        if (x > 5.367655 * 0.010591 + 0.092809)
            return (pow(10.0, (x - 0.385537) / 0.247190) - 0.052272) / 5.555556;
        return (x - 0.092809) / 5.367655;
    }
    if (t == 12) {
        if (x >= 171.2102946929 / 1023.0)
            return pow(10.0, (x * 1023.0 - 420.0) / 261.5) * 0.19 - 0.01;
        return (x * 1023.0 - 95.0) * 0.01125 / (171.2102946929 - 95.0);
    }
    if (t == 13) {
        if (x < 0.181) return (x - 0.125) / 5.6;
        return pow(10.0, (x - 0.598206) / 0.241514) - 0.00873;
    }
    if (t == 14) {
        if (x < 0.0) return x / 15.1927 - 0.01;
        return (pow(10.0, x / 0.224282) - 1.0) / 155.975327 - 0.01;
    }
    return x;
}

float tfEncode(int t, float x)
{
    if (t == 0) return x;
    if (t == 1) return pow(max(x, 0.0), 1.0 / 2.2);
    if (t == 2) return pow(max(x, 0.0), 1.0 / 2.4);
    if (t == 3) return pow(max(x, 0.0), 1.0 / 2.6);
    if (t == 4) return x <= 0.0031308 ? x * 12.92 : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
    if (t == 5) return x < 0.018 ? x * 4.5 : 1.099 * pow(x, 0.45) - 0.099;
    if (t == 6) {
        float y = pow(clamp(x / 100.0, 0.0, 1.0), PQ_M1);
        return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), PQ_M2);
    }
    if (t == 7) {
        float s = max(x, 0.0);
        if (s <= 1.0 / 12.0) return sqrt(3.0 * s);
        return 0.17883277 * log(12.0 * s - 0.28466892) + 0.55991073;
    }
    if (t == 8) {
        if (x <= 0.0) return (-16.0 + 9.72) / 17.52;
        if (x < exp2(-15.0)) return (log2(exp2(-16.0) + x * 0.5) + 9.72) / 17.52;
        return (log2(x) + 9.72) / 17.52;
    }
    if (t == 9) {
        if (x <= 0.0078125) return 10.5402377416545 * x + 0.0729055341958355;
        return (log2(x) + 9.72) / 17.52;
    }
    if (t == 10) return x > 0.00262409 ? (log2(x + 0.0075) + 7.0) * 0.07329248 : x * 10.44426855;
    if (t == 11) return x > 0.010591 ? 0.247190 * log(5.555556 * x + 0.052272) / log(10.0) + 0.385537
                                       : 5.367655 * x + 0.092809;
    if (t == 12) {
        if (x >= 0.01125)
            return (420.0 + log((x + 0.01) / 0.19) / log(10.0) * 261.5) / 1023.0;
        return (x * (171.2102946929 - 95.0) / 0.01125 + 95.0) / 1023.0;
    }
    if (t == 13) return x < 0.01 ? 5.6 * x + 0.125 : 0.241514 * log(x + 0.00873) / log(10.0) + 0.598206;
    if (t == 14) {
        float v = x + 0.01;
        return v < 0.0 ? v * 15.1927 : 0.224282 * log(v * 155.975327 + 1.0) / log(10.0);
    }
    return x;
}

float compressDist(float x, float thr, float lim, float pw)
{
    if (x < thr) return x;
    lim = max(lim, 1.0001);
    thr = min(thr, 0.9999);
    float s = (lim - thr) / pow(pow((1.0 - thr) / (lim - thr), -pw) - 1.0, 1.0 / pw);
    float u = (x - thr) / s;
    return thr + s * u / pow(1.0 + pow(u, pw), 1.0 / pw);
}

float hlgGamma(float nits) { return 1.2 + 0.42 * log(max(nits, 1.0) / 1000.0) / log(10.0); }

vec3 fitCurve(vec3 v)
{
    return (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081);
}

vec3 fitCurveInverse(vec3 y)
{
    y = clamp(y, 0.0, 0.9999);
    vec3 a = 0.983729 * y - 1.0;
    vec3 b = 0.4329510 * y - 0.0245786;
    vec3 c = 0.238081 * y + 0.000090537;
    return (-b - sqrt(max(b * b - 4.0 * a * c, 0.0))) / (2.0 * a);
}

vec3 apply(vec3 c)
{
    int inTf = int(uOpt1.x + 0.5), outTf = int(uOpt1.y + 0.5);
    int tone = int(uOpt1.z + 0.5), gmap = int(uOpt1.w + 0.5);
    int rrt = int(uOpt3.x + 0.5);
    float peakIn = uOpt2.x, peakOut = uOpt2.y;
    vec3 lin = vec3(tfDecode(inTf, c.r), tfDecode(inTf, c.g), tfDecode(inTf, c.b));
    if (uOpt2.z > 0.5 && inTf == 7) {
        float ys = max(dot(uLumaIn, lin), 0.0);
        lin *= peakIn * pow(ys, hlgGamma(peakIn * 100.0) - 1.0);
    }
    vec3 t = uM1 * lin;
    if (rrt == 1) t = uFitFrom * fitCurve(uFitTo * t);
    else if (rrt == 2) t = uFitToInv * fitCurveInverse(uFitFromInv * t);
    t = uM2 * t;
    if (rrt == 1) t = clamp(t, 0.0, 1.0);
    if (uOpt4.y > 0.0) {
        for (int i = 0; i < 3; ++i)
            t[i] = compressDist(t[i], uOpt4.x, uOpt4.y, uOpt4.z);
    }
    if (tone != 0 && peakIn > peakOut) {
        if (tone == 1) {
            float w = peakIn / peakOut;
            for (int i = 0; i < 3; ++i) {
                if (t[i] > 0.0) {
                    float n = t[i] / peakOut;
                    t[i] = peakOut * n * (1.0 + n / (w * w)) / (1.0 + n);
                }
            }
        } else {
            float y = dot(uLumaOut, t);
            if (y > 1e-6) {
                float sh;
                if (tone == 3) {
                    float knee = 0.5 * peakOut;
                    sh = y <= knee ? y : knee + (peakOut - knee) * tanh((y - knee) / (peakOut - knee));
                } else {
                    float w = peakIn / peakOut;
                    float n = y / peakOut;
                    sh = peakOut * n * (1.0 + n / (w * w)) / (1.0 + n);
                }
                t *= sh / y;
            }
        }
    }
    if (gmap == 1) {
        float lo = min(t.r, min(t.g, t.b));
        if (lo < 0.0) {
            float y = max(dot(uLumaOut, t), 0.0);
            float s = y / (y - lo);
            t = vec3(y) + s * (t - vec3(y));
        }
    } else if (gmap == 2) {
        float ach = max(t.r, max(t.g, t.b));
        if (ach <= 0.0) {
            t = max(t, vec3(0.0));
        } else {
            for (int i = 0; i < 3; ++i) {
                float d = (ach - t[i]) / ach;
                t[i] = ach - compressDist(d, uOpt3.y, uOpt3.z, uOpt3.w) * ach;
            }
        }
    }
    if (uOpt2.w > 0.5 && outTf == 7) {
        float g = hlgGamma(peakOut * 100.0);
        float yd = max(dot(uLumaOut, t), 0.0);
        float ys = pow(yd / peakOut, 1.0 / g);
        t *= ys > 0.0 ? 1.0 / (peakOut * pow(ys, g - 1.0)) : 0.0;
    }
    t = uM3 * t;
    vec3 o = vec3(tfEncode(outTf, t.r), tfEncode(outTf, t.g), tfEncode(outTf, t.b));
    return mix(c, o, uOpt4.w);
}

void main()
{
    vec4 t = texture(uTex, vUv);
    if (t.a <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    vec3 o = apply(straight(t));
    fragColor = vec4(clamp(o, vec3(0.0), vec3(60000.0)) * t.a, t.a);
}
)";

class ColorManagementEffect final : public PassEffect
{
public:
    explicit ColorManagementEffect(const char* type)
        : m_type(QString::fromLatin1(type))
    { }

    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "color_management", kCmShader, why, true))
            return false;
        static const char* const names[] = {"uM1", "uM2", "uM3", "uFitTo", "uFitFrom",
            "uFitToInv", "uFitFromInv", "uOpt1", "uOpt2", "uOpt3", "uOpt4", "uLumaIn",
            "uLumaOut"};
        for (size_t i = 0; i < std::size(names); ++i)
            m_loc[i] = uniform(kit, names[i]);
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return fxmath::colorManagementIdentity(m_type, p, f);
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const cm::CstParams cp = fxmath::colorManagementParams(m_type, *in.params, in.localFrame);
        float fit[4][9];
        cm::fitMatrices(fit[0], fit[1], fit[2], fit[3]);
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniformMatrix3fv(m_loc[0], 1, GL_TRUE, cp.m);
        gl->glUniformMatrix3fv(m_loc[1], 1, GL_TRUE, cp.m2);
        gl->glUniformMatrix3fv(m_loc[2], 1, GL_TRUE, cp.m3);
        for (int i = 0; i < 4; ++i)
            gl->glUniformMatrix3fv(m_loc[3 + i], 1, GL_TRUE, fit[i]);
        gl->glUniform4f(m_loc[7], float(cp.inTf), float(cp.outTf), float(cp.tone), float(cp.gmap));
        gl->glUniform4f(m_loc[8], cp.peakIn, cp.peakOut, cp.fwdOotf ? 1.f : 0.f,
            cp.invOotf ? 1.f : 0.f);
        gl->glUniform4f(m_loc[9], float(cp.rrt), cp.gmThr, cp.gmLim, cp.gmPwr);
        gl->glUniform4f(m_loc[10], cp.hiThr, cp.hiLim, cp.hiPwr, cp.amount);
        gl->glUniform3fv(m_loc[11], 1, cp.lumaIn);
        gl->glUniform3fv(m_loc[12], 1, cp.lumaOut);
        draw(kit, *in.layer);
        return out;
    }

private:
    QString m_type;
    GLint m_loc[13] {};
};

template<const char* kId>
std::unique_ptr<GlEffect> makeCm()
{
    return std::make_unique<ColorManagementEffect>(kId);
}

const GlEffectFactory kTable[] = {
    {fxid::ColorSpaceTransform, &makeCm<fxid::ColorSpaceTransform>},
    {fxid::AcesTransform, &makeCm<fxid::AcesTransform>},
    {fxid::GamutMapping, &makeCm<fxid::GamutMapping>},
    {fxid::ChromaticAdaptation, &makeCm<fxid::ChromaticAdaptation>},
    {fxid::ColorCompressor, &makeCm<fxid::ColorCompressor>},
};

} // namespace

std::span<const GlEffectFactory> colorManagementGlEffects()
{
    return kTable;
}

} // namespace kk::render
