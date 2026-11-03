// SPDX-License-Identifier: GPL-3.0-or-later
// GlGrade — GPU evaluation of a prepared grade. Every GLSL function below has a C++ twin in
// core/GradeMath.cpp (primaries, curves, HSL, qualifier, windows) or render/GradeEval.cpp /
// render/CubeLut.cpp (node mixing, LUT sampling); test_graderender compares the two paths.
#include "render/GlGrade.h"

#include <QString>

#include <algorithm>
#include <string>

namespace kk::render {

namespace {

using namespace grademath;

/// Texture units of the corrector program.
constexpr int kUnitInput = 0;
constexpr int kUnitCurves = 1;
constexpr int kUnitLut3d = 2;
constexpr int kUnitLut1d = 3;
constexpr int kUnitPoints = 4;

const std::string kCorrectorSource = std::string(R"(
uniform sampler2D uInput;
uniform sampler2D uCurves;
uniform sampler3D uLut3d;
uniform sampler2D uLut1d;
uniform sampler2D uPoints;
uniform int uMode;        // 0 = graded picture, 1 = the key (alpha for a Layer mixer)
uniform int uCorrects;
uniform int uPrimaries;
uniform vec4 uLift, uGamma, uGain, uOffset;
uniform float uContrast, uPivot, uSaturation, uTemperature, uTint, uLumMix;
uniform mat3 uHue;
uniform int uCurveMask;
uniform int uLutMode;     // 0 none, 1 1D, 2 3D trilinear, 3 3D tetrahedral
uniform float uLutN;
uniform vec3 uLutMin, uLutMax;
uniform int uKeyed;
uniform float uMix;
uniform int uQual, uQualInvert, uQualUse;   // use: 1 hue, 2 sat, 4 lum
uniform vec3 uQualHue;    // centre, half width, softness (turns)
uniform vec3 uQualSat;    // low, high, softness
uniform vec3 uQualLum;
uniform int uWinCount;
uniform int uWinShape[8];
uniform int uWinInvert[8];
uniform float uWinOpacity[8];
uniform vec4 uWinGeom[8];     // centre x, y, half width, half height (pixels)
uniform vec4 uWinRotSoft[8];  // cos, sin, soft in, soft out
uniform ivec2 uWinPoints[8];  // first point, count (polygon)

const vec3 kLuma = vec3()")
    + std::to_string(kLumaR) + ", " + std::to_string(kLumaG) + ", " + std::to_string(kLumaB) + R"();
const int kCurveSize = )"
    + std::to_string(kCurveTableSize) + R"(;
const float kTempRange = )"
    + std::to_string(kTemperatureRange) + R"(;
const float kTintRange = )"
    + std::to_string(kTintRange) + R"(;
const float kGammaRange = )"
    + std::to_string(kGammaRange) + R"(;
const float kMinSoft = 1e-4;

float luma(vec3 c) { return dot(c, kLuma); }

float lgg(float x, float lift, float gamma, float gain)
{
    float v = max(gain * (x + lift * (1.0 - x)), 0.0);
    return pow(v, exp2(-kGammaRange * gamma));
}
vec3 lgg3(vec3 x, vec3 lift, vec3 gamma, vec3 gain)
{
    return vec3(lgg(x.r, lift.r, gamma.r, gain.r), lgg(x.g, lift.g, gamma.g, gain.g),
        lgg(x.b, lift.b, gamma.b, gain.b));
}

vec3 primaries(vec3 c)
{
    c += uOffset.rgb + uOffset.a;
    c.r *= 1.0 + kTempRange * uTemperature;
    c.b *= 1.0 - kTempRange * uTemperature;
    c.g *= 1.0 - kTintRange * uTint;
    c = lgg3(c, uLift.rgb, uGamma.rgb, uGain.rgb);
    float y = luma(c);
    float y2 = lgg(y, uLift.a, uGamma.a, uGain.a);
    vec3 rgbMaster = lgg3(c, vec3(uLift.a), vec3(uGamma.a), vec3(uGain.a));
    vec3 lumaMaster = c + (y2 - y);
    c = rgbMaster + (lumaMaster - rgbMaster) * uLumMix;
    c = (c - uPivot) * uContrast + uPivot;
    c = uHue * c;
    float y3 = luma(c);
    return y3 + (c - y3) * uSaturation;
}

float curve(int index, float x)
{
    float pos = clamp(x, 0.0, 1.0) * float(kCurveSize - 1);
    int i = min(int(pos), kCurveSize - 2);
    float f = pos - float(i);
    float a = texelFetch(uCurves, ivec2(i, index), 0).r;
    float b = texelFetch(uCurves, ivec2(i + 1, index), 0).r;
    return a + (b - a) * f;
}
bool curveOn(int index) { return (uCurveMask & (1 << index)) != 0; }

vec3 rgbToHsl(vec3 c)
{
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float l = (mx + mn) * 0.5;
    float d = mx - mn;
    float h = 0.0;
    float s = 0.0;
    if (d > 1e-6) {
        s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
        if (mx == c.r)
            h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
        else if (mx == c.g)
            h = (c.b - c.r) / d + 2.0;
        else
            h = (c.r - c.g) / d + 4.0;
        h /= 6.0;
    }
    return vec3(h - floor(h), s, l);
}
float hueToChannel(float p, float q, float t)
{
    t -= floor(t);
    if (t < 1.0 / 6.0)
        return p + (q - p) * 6.0 * t;
    if (t < 0.5)
        return q;
    if (t < 2.0 / 3.0)
        return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}
vec3 hslToRgb(vec3 hsl)
{
    if (hsl.y <= 0.0)
        return vec3(hsl.z);
    float q = hsl.z < 0.5 ? hsl.z * (1.0 + hsl.y) : hsl.z + hsl.y - hsl.z * hsl.y;
    float p = 2.0 * hsl.z - q;
    return vec3(hueToChannel(p, q, hsl.x + 1.0 / 3.0), hueToChannel(p, q, hsl.x),
        hueToChannel(p, q, hsl.x - 1.0 / 3.0));
}

vec3 curves(vec3 c)
{
    for (int ch = 0; ch < 3; ++ch) {
        float v = clamp(c[ch], 0.0, 1.0);
        if (curveOn(0))
            v = curve(0, v);
        if (curveOn(ch + 1))
            v = curve(ch + 1, v);
        c[ch] = v;
    }
    // 4 hue-hue, 5 hue-sat, 6 hue-lum, 7 lum-sat, 8 sat-sat
    if ((uCurveMask & 0x1f0) == 0)
        return c;
    vec3 hsl = rgbToHsl(c);
    float h = hsl.x;
    float y = clamp(luma(c), 0.0, 1.0);
    float factor = 1.0;
    if (curveOn(5))
        factor *= curve(5, h);
    if (curveOn(7))
        factor *= curve(7, y);
    if (curveOn(8))
        factor *= curve(8, hsl.y);
    if (curveOn(4)) {
        float shifted = h + curve(4, h);
        hsl.x = shifted - floor(shifted);
    }
    hsl.y = clamp(hsl.y * factor, 0.0, 1.0);
    c = hslToRgb(hsl);
    if (curveOn(6)) {
        float y2 = luma(c);
        c += y2 * (curve(6, h) - 1.0);
    }
    return c;
}

vec3 lutAt(ivec3 i) { return texelFetch(uLut3d, i, 0).rgb; }
vec3 lut(vec3 c)
{
    int n = int(uLutN);
    vec3 t = clamp((c - uLutMin) / (uLutMax - uLutMin), 0.0, 1.0) * float(n - 1);
    if (uLutMode == 1) {
        vec3 r;
        for (int ch = 0; ch < 3; ++ch) {
            int i0 = min(int(t[ch]), n - 2);
            float f = t[ch] - float(i0);
            r[ch] = texelFetch(uLut1d, ivec2(i0, 0), 0)[ch] * (1.0 - f)
                + texelFetch(uLut1d, ivec2(i0 + 1, 0), 0)[ch] * f;
        }
        return r;
    }
    ivec3 i0 = min(ivec3(t), ivec3(n - 2));
    vec3 f = t - vec3(i0);
    if (uLutMode == 2) {
        vec3 acc = vec3(0.0);
        for (int corner = 0; corner < 8; ++corner) {
            ivec3 d = ivec3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
            vec3 w3 = mix(1.0 - f, f, vec3(d));
            acc += w3.x * w3.y * w3.z * lutAt(i0 + d);
        }
        return acc;
    }
    // Tetrahedral (CubeLut::applyTetrahedral()).
    float fr = f.r, fg = f.g, fb = f.b;
    ivec3 a, b;
    vec4 w;
    if (fr >= fg) {
        if (fg >= fb) { a = ivec3(1, 0, 0); b = ivec3(1, 1, 0); w = vec4(1.0 - fr, fr - fg, fg - fb, fb); }
        else if (fr >= fb) { a = ivec3(1, 0, 0); b = ivec3(1, 0, 1); w = vec4(1.0 - fr, fr - fb, fb - fg, fg); }
        else { a = ivec3(0, 0, 1); b = ivec3(1, 0, 1); w = vec4(1.0 - fb, fb - fr, fr - fg, fg); }
    } else {
        if (fb > fg) { a = ivec3(0, 0, 1); b = ivec3(0, 1, 1); w = vec4(1.0 - fb, fb - fg, fg - fr, fr); }
        else if (fb > fr) { a = ivec3(0, 1, 0); b = ivec3(0, 1, 1); w = vec4(1.0 - fg, fg - fb, fb - fr, fr); }
        else { a = ivec3(0, 1, 0); b = ivec3(1, 1, 0); w = vec4(1.0 - fg, fg - fr, fr - fb, fb); }
    }
    return w.x * lutAt(i0) + w.y * lutAt(i0 + a) + w.z * lutAt(i0 + b) + w.w * lutAt(i0 + ivec3(1));
}

float rangeKey(float v, vec3 r)
{
    float soft = max(r.z, kMinSoft);
    float k = 1.0;
    if (r.x > 0.0)
        k *= smoothstep(r.x - soft, r.x, v);
    if (r.y < 1.0)
        k *= 1.0 - smoothstep(r.y, r.y + soft, v);
    return k;
}
float qualifierKey(vec3 c)
{
    if (uQual == 0)
        return 1.0;
    vec3 hsl = rgbToHsl(clamp(c, 0.0, 1.0));
    float key = 1.0;
    if ((uQualUse & 1) != 0) {
        float rel = hsl.x - uQualHue.x + 0.5;
        float dist = abs(rel - floor(rel) - 0.5);
        float soft = max(uQualHue.z, kMinSoft);
        key *= 1.0 - smoothstep(uQualHue.y, uQualHue.y + soft, dist);
    }
    if ((uQualUse & 2) != 0)
        key *= rangeKey(hsl.y, uQualSat);
    if ((uQualUse & 4) != 0)
        key *= rangeKey(hsl.z, uQualLum);
    return uQualInvert != 0 ? 1.0 - key : key;
}

float windowCoverage(int i, vec2 p)
{
    vec2 d = p - uWinGeom[i].xy;
    float cs = uWinRotSoft[i].x, sn = uWinRotSoft[i].y;
    float lx = cs * d.x + sn * d.y;
    float ly = -sn * d.x + cs * d.y;
    float dist = 0.0;
    int shape = uWinShape[i];
    if (shape == 0) {
        float rx = max(uWinGeom[i].z, 1e-3);
        float ry = max(uWinGeom[i].w, 1e-3);
        float k = sqrt((lx / rx) * (lx / rx) + (ly / ry) * (ly / ry));
        dist = (k - 1.0) * min(rx, ry);
    } else if (shape == 1) {
        float qx = abs(lx) - uWinGeom[i].z;
        float qy = abs(ly) - uWinGeom[i].w;
        float ox = max(qx, 0.0), oy = max(qy, 0.0);
        dist = sqrt(ox * ox + oy * oy) + min(max(qx, qy), 0.0);
    } else if (shape == 3) {
        dist = ly;
    } else {
        int first = uWinPoints[i].x;
        int n = uWinPoints[i].y;
        float best = 1e30;
        bool inside = false;
        for (int k = 0, j = n - 1; k < n; j = k++) {
            vec2 A = texelFetch(uPoints, ivec2(first + j, 0), 0).xy;
            vec2 B = texelFetch(uPoints, ivec2(first + k, 0), 0).xy;
            float ex = B.x - A.x, ey = B.y - A.y;
            float wx = lx - A.x, wy = ly - A.y;
            float len2 = ex * ex + ey * ey;
            float t = len2 > 0.0 ? clamp((wx * ex + wy * ey) / len2, 0.0, 1.0) : 0.0;
            float qx = wx - ex * t, qy = wy - ey * t;
            best = min(best, qx * qx + qy * qy);
            if ((A.y > ly) != (B.y > ly) && lx < A.x + (ly - A.y) * ex / ey)
                inside = !inside;
        }
        dist = sqrt(best) * (inside ? -1.0 : 1.0);
    }
    return 1.0 - smoothstep(-uWinRotSoft[i].z, uWinRotSoft[i].w, dist);
}
float windowsMatte(vec2 p)
{
    if (uWinCount == 0)
        return 1.0;
    float m = 0.0;
    for (int i = 0; i < uWinCount; ++i) {
        float c = windowCoverage(i, p);
        if (uWinInvert[i] != 0)
            c = 1.0 - c;
        c *= uWinOpacity[i];
        m = m + c - m * c;
    }
    return m;
}

void main()
{
    vec4 src = texelFetch(uInput, ivec2(gl_FragCoord.xy), 0);
    vec3 c = src.a > 0.0 ? src.rgb / src.a : vec3(0.0);
    float key = uMix;
    if (uKeyed != 0)
        key *= qualifierKey(c) * windowsMatte(gl_FragCoord.xy);
    if (uMode == 1) {
        fragColor = vec4(key, key, key, 1.0);
        return;
    }
    vec3 g = c;
    if (uCorrects != 0 && key > 0.0) {
        if (uPrimaries != 0)
            g = primaries(g);
        if (uCurveMask != 0)
            g = curves(g);
        if (uLutMode != 0)
            g = lut(g);
    }
    vec3 o = c + (g - c) * key;
    fragColor = vec4(o * src.a, src.a);
}
)";

// Sampler arrays need constant indices in GLSL 3.30: eight named inputs.
const char* const kParallelSource = R"(
uniform sampler2D uIn0, uIn1, uIn2, uIn3, uIn4, uIn5, uIn6, uIn7;
uniform int uCount;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 first = texelFetch(uIn0, p, 0);
    vec3 sum = first.rgb;
    if (uCount > 1) sum += texelFetch(uIn1, p, 0).rgb;
    if (uCount > 2) sum += texelFetch(uIn2, p, 0).rgb;
    if (uCount > 3) sum += texelFetch(uIn3, p, 0).rgb;
    if (uCount > 4) sum += texelFetch(uIn4, p, 0).rgb;
    if (uCount > 5) sum += texelFetch(uIn5, p, 0).rgb;
    if (uCount > 6) sum += texelFetch(uIn6, p, 0).rgb;
    if (uCount > 7) sum += texelFetch(uIn7, p, 0).rgb;
    fragColor = vec4(sum / float(uCount), first.a);
}
)";

// One Layer mixer step: result = (1 - a) result + a in + (o - in) (grademath::layerOver()),
// in premultiplied colour (every node keeps the input's alpha, so the step is the same).
const char* const kLayerSource = R"(
uniform sampler2D uResult;
uniform sampler2D uIn;
uniform sampler2D uOut;
uniform sampler2D uKey;
uniform int uUseKey;
uniform int uOpaque;      // the top input is a mixer or the grade's input: it replaces
uniform float uConstAlpha;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 r = texelFetch(uResult, p, 0);
    vec4 o = texelFetch(uOut, p, 0);
    if (uOpaque != 0) {
        fragColor = vec4(o.rgb, r.a);
        return;
    }
    vec4 i = texelFetch(uIn, p, 0);
    float a = uUseKey != 0 ? texelFetch(uKey, p, 0).r : uConstAlpha;
    fragColor = vec4((1.0 - a) * r.rgb + a * i.rgb + (o.rgb - i.rgb), r.a);
}
)";

GLuint makeTexture(GlFunctions* gl, GLenum target)
{
    GLuint tex = 0;
    gl->glGenTextures(1, &tex);
    gl->glBindTexture(target, tex);
    gl->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (target == GL_TEXTURE_3D)
        gl->glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    gl->glBindTexture(target, 0);
    return tex;
}

} // namespace

bool GlGrade::initialize(GlKit& kit, QString* why)
{
    GlFunctions* gl = kit.gl();
    QString log;
    if (!m_corrector.build(gl, "grade_corrector", kCorrectorSource.c_str(), &log)
        || !m_parallel.build(gl, "grade_parallel", kParallelSource, &log)
        || !m_layer.build(gl, "grade_layer", kLayerSource, &log)) {
        *why = log;
        return false;
    }
    auto u = [&](const char* name) { return m_corrector.uniform(gl, name); };
    m_u.mode = u("uMode");
    m_u.corrects = u("uCorrects");
    m_u.primaries = u("uPrimaries");
    m_u.lift = u("uLift");
    m_u.gamma = u("uGamma");
    m_u.gain = u("uGain");
    m_u.offset = u("uOffset");
    m_u.contrast = u("uContrast");
    m_u.pivot = u("uPivot");
    m_u.saturation = u("uSaturation");
    m_u.temperature = u("uTemperature");
    m_u.tint = u("uTint");
    m_u.lumMix = u("uLumMix");
    m_u.hue = u("uHue");
    m_u.curveMask = u("uCurveMask");
    m_u.lutMode = u("uLutMode");
    m_u.lutN = u("uLutN");
    m_u.lutMin = u("uLutMin");
    m_u.lutMax = u("uLutMax");
    m_u.keyed = u("uKeyed");
    m_u.mix = u("uMix");
    m_u.qual = u("uQual");
    m_u.qualInvert = u("uQualInvert");
    m_u.qualUse = u("uQualUse");
    m_u.qualHue = u("uQualHue");
    m_u.qualSat = u("uQualSat");
    m_u.qualLum = u("uQualLum");
    m_u.winCount = u("uWinCount");
    m_u.winShape = u("uWinShape");
    m_u.winInvert = u("uWinInvert");
    m_u.winOpacity = u("uWinOpacity");
    m_u.winGeom = u("uWinGeom");
    m_u.winRotSoft = u("uWinRotSoft");
    m_u.winPoints = u("uWinPoints");
    gl->glUseProgram(m_corrector.id);
    gl->glUniform1i(u("uInput"), kUnitInput);
    gl->glUniform1i(u("uCurves"), kUnitCurves);
    gl->glUniform1i(u("uLut3d"), kUnitLut3d);
    gl->glUniform1i(u("uLut1d"), kUnitLut1d);
    gl->glUniform1i(u("uPoints"), kUnitPoints);

    m_parallelCount = m_parallel.uniform(gl, "uCount");
    gl->glUseProgram(m_parallel.id);
    for (int i = 0; i < kMaxMixerInputs; ++i) {
        const QByteArray name = "uIn" + QByteArray::number(i);
        gl->glUniform1i(m_parallel.uniform(gl, name.constData()), i);
    }
    gl->glUseProgram(m_layer.id);
    gl->glUniform1i(m_layer.uniform(gl, "uResult"), 0);
    gl->glUniform1i(m_layer.uniform(gl, "uIn"), 1);
    gl->glUniform1i(m_layer.uniform(gl, "uOut"), 2);
    gl->glUniform1i(m_layer.uniform(gl, "uKey"), 3);
    m_layerConstAlpha = m_layer.uniform(gl, "uConstAlpha");
    m_layerUseKey = m_layer.uniform(gl, "uUseKey");
    m_layerOpaque = m_layer.uniform(gl, "uOpaque");
    gl->glUseProgram(0);

    m_curveTex = makeTexture(gl, GL_TEXTURE_2D);
    gl->glBindTexture(GL_TEXTURE_2D, m_curveTex);
    gl->glTexImage2D(
        GL_TEXTURE_2D, 0, GL_R32F, kCurveTableSize, kGradeCurveCount, 0, GL_RED, GL_FLOAT, nullptr);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    m_lut3d = makeTexture(gl, GL_TEXTURE_3D);
    m_lut1d = makeTexture(gl, GL_TEXTURE_2D);
    m_pointsTex = makeTexture(gl, GL_TEXTURE_2D);
    return true;
}

void GlGrade::destroy(GlKit& kit)
{
    GlFunctions* gl = kit.gl();
    m_corrector.destroy(gl);
    m_parallel.destroy(gl);
    m_layer.destroy(gl);
    for (GLuint* tex : {&m_curveTex, &m_lut3d, &m_lut1d, &m_pointsTex}) {
        if (*tex)
            gl->glDeleteTextures(1, tex);
        *tex = 0;
    }
    m_uploadedLut.reset();
    m_pointsCapacity = 0;
}

void GlGrade::uploadCurves(GlFunctions* gl, const CurveTables& tables)
{
    gl->glBindTexture(GL_TEXTURE_2D, m_curveTex);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kCurveTableSize, kGradeCurveCount, GL_RED, GL_FLOAT,
        tables.table.data());
    gl->glBindTexture(GL_TEXTURE_2D, 0);
}

void GlGrade::uploadLut(GlFunctions* gl, const std::shared_ptr<const CubeLut>& lut)
{
    if (lut == m_uploadedLut)
        return;
    const int n = lut->size();
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (lut->is3d()) {
        gl->glBindTexture(GL_TEXTURE_3D, m_lut3d);
        gl->glTexImage3D(
            GL_TEXTURE_3D, 0, GL_RGB32F, n, n, n, 0, GL_RGB, GL_FLOAT, lut->data().constData());
        gl->glBindTexture(GL_TEXTURE_3D, 0);
    } else {
        gl->glBindTexture(GL_TEXTURE_2D, m_lut1d);
        gl->glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGB32F, n, 1, 0, GL_RGB, GL_FLOAT, lut->data().constData());
        gl->glBindTexture(GL_TEXTURE_2D, 0);
    }
    m_uploadedLut = lut;
}

void GlGrade::uploadPoints(GlFunctions* gl, const PreparedGradeNode& node, std::vector<int>& ranges)
{
    ranges.assign(node.windows.size() * 2, 0);
    m_points.clear();
    for (size_t i = 0; i < node.windows.size(); ++i) {
        const WindowValues& w = node.windows[i];
        if (GradeWindowShape(w.shape) != GradeWindowShape::Polygon)
            continue;
        ranges[i * 2] = int(m_points.size() / 2);
        ranges[i * 2 + 1] = int(w.points.size() / 2);
        m_points.insert(m_points.end(), w.points.begin(), w.points.end());
    }
    if (m_points.empty())
        return;
    const int count = int(m_points.size() / 2);
    gl->glBindTexture(GL_TEXTURE_2D, m_pointsTex);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (count > m_pointsCapacity) {
        m_pointsCapacity = std::max(count, 64);
        gl->glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RG32F, m_pointsCapacity, 1, 0, GL_RG, GL_FLOAT, nullptr);
    }
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, count, 1, GL_RG, GL_FLOAT, m_points.data());
    gl->glBindTexture(GL_TEXTURE_2D, 0);
}

void GlGrade::drawCorrector(GlKit& kit, const PreparedGradeNode& n, GLuint input, int mode)
{
    GlFunctions* gl = kit.gl();
    const PrimaryValues& p = n.primaries;
    std::vector<int> ranges;
    uploadPoints(gl, n, ranges);
    if (n.curves && mode == 0)
        uploadCurves(gl, *n.curves);
    if (n.lut && mode == 0)
        uploadLut(gl, n.lut);
    gl->glUseProgram(m_corrector.id);
    kit.setFullscreen(m_corrector);
    gl->glUniform1i(m_u.mode, mode);
    gl->glUniform1i(m_u.corrects, n.corrects ? 1 : 0);
    gl->glUniform1i(m_u.primaries, n.primariesIdentity ? 0 : 1);
    gl->glUniform4f(m_u.lift, p.lift[0], p.lift[1], p.lift[2], p.lift[3]);
    gl->glUniform4f(m_u.gamma, p.gamma[0], p.gamma[1], p.gamma[2], p.gamma[3]);
    gl->glUniform4f(m_u.gain, p.gain[0], p.gain[1], p.gain[2], p.gain[3]);
    gl->glUniform4f(m_u.offset, p.offset[0], p.offset[1], p.offset[2], p.offset[3]);
    gl->glUniform1f(m_u.contrast, p.contrast);
    gl->glUniform1f(m_u.pivot, p.pivot);
    gl->glUniform1f(m_u.saturation, p.saturation);
    gl->glUniform1f(m_u.temperature, p.temperature);
    gl->glUniform1f(m_u.tint, p.tint);
    gl->glUniform1f(m_u.lumMix, p.lumMix);
    gl->glUniformMatrix3fv(m_u.hue, 1, GL_TRUE, p.hueMatrix); // row-major on the CPU
    gl->glUniform1i(m_u.curveMask, n.curves ? n.curves->mask() : 0);
    int lutMode = 0;
    if (n.lut)
        lutMode = !n.lut->is3d() ? 1 : (n.tetrahedral ? 3 : 2);
    gl->glUniform1i(m_u.lutMode, lutMode);
    if (n.lut) {
        gl->glUniform1f(m_u.lutN, float(n.lut->size()));
        const float* mn = n.lut->domainMin();
        const float* mx = n.lut->domainMax();
        gl->glUniform3f(m_u.lutMin, mn[0], mn[1], mn[2]);
        gl->glUniform3f(m_u.lutMax, mx[0], mx[1], mx[2]);
    }
    gl->glUniform1i(m_u.keyed, n.keyed ? 1 : 0);
    gl->glUniform1f(m_u.mix, n.mix);
    const GradeQualifier& q = n.qualifier;
    gl->glUniform1i(m_u.qual, q.enabled ? 1 : 0);
    gl->glUniform1i(m_u.qualInvert, q.invert ? 1 : 0);
    gl->glUniform1i(m_u.qualUse, (q.useHue ? 1 : 0) | (q.useSat ? 2 : 0) | (q.useLum ? 4 : 0));
    gl->glUniform3f(m_u.qualHue, float(q.hueCenter), float(q.hueWidth) * 0.5f, float(q.hueSoft));
    gl->glUniform3f(m_u.qualSat, float(q.satLow), float(q.satHigh), float(q.satSoft));
    gl->glUniform3f(m_u.qualLum, float(q.lumLow), float(q.lumHigh), float(q.lumSoft));
    const int count = std::min(int(n.windows.size()), kMaxGradeWindows);
    int shape[kMaxGradeWindows] = {}, invert[kMaxGradeWindows] = {};
    float opacity[kMaxGradeWindows] = {};
    float geom[kMaxGradeWindows * 4] = {}, rotSoft[kMaxGradeWindows * 4] = {};
    int points[kMaxGradeWindows * 2] = {};
    for (int i = 0; i < count; ++i) {
        const WindowValues& w = n.windows[size_t(i)];
        shape[i] = w.shape;
        invert[i] = w.invert ? 1 : 0;
        opacity[i] = w.opacity;
        const float g[4] = {w.cx, w.cy, w.halfW, w.halfH};
        const float r[4] = {w.cosR, w.sinR, w.softIn, w.softOut};
        std::copy(g, g + 4, geom + i * 4);
        std::copy(r, r + 4, rotSoft + i * 4);
        points[i * 2] = ranges[size_t(i) * 2];
        points[i * 2 + 1] = ranges[size_t(i) * 2 + 1];
    }
    gl->glUniform1i(m_u.winCount, count);
    if (count > 0) {
        gl->glUniform1iv(m_u.winShape, count, shape);
        gl->glUniform1iv(m_u.winInvert, count, invert);
        gl->glUniform1fv(m_u.winOpacity, count, opacity);
        gl->glUniform4fv(m_u.winGeom, count, geom);
        gl->glUniform4fv(m_u.winRotSoft, count, rotSoft);
        gl->glUniform2iv(m_u.winPoints, count, points);
    }
    kit.bindTexture(kUnitInput, input);
    kit.bindTexture(kUnitCurves, m_curveTex);
    gl->glActiveTexture(GL_TEXTURE0 + kUnitLut3d);
    gl->glBindTexture(GL_TEXTURE_3D, m_lut3d);
    kit.bindTexture(kUnitLut1d, m_lut1d);
    kit.bindTexture(kUnitPoints, m_pointsTex);
    kit.setBlendOver(false);
    kit.drawQuad();
    gl->glActiveTexture(GL_TEXTURE0 + kUnitLut3d);
    gl->glBindTexture(GL_TEXTURE_3D, 0);
    gl->glActiveTexture(GL_TEXTURE0);
}

PooledTarget GlGrade::apply(GlKit& kit, const RenderTarget& input, const PreparedGrade& grade)
{
    if (grade.identity || grade.output < 0)
        return {};
    GlFunctions* gl = kit.gl();
    const QSize size = input.size;
    const size_t count = grade.nodes.size();
    // Per node: its output texture (owned target, or a texture of another node / the input)
    // and, for nodes a Layer mixer reads, the key texture or a constant alpha.
    std::vector<PooledTarget> owned(count);
    std::vector<GLuint> texture(count, 0);
    std::vector<PooledTarget> keyTargets(count);
    std::vector<float> constAlpha(count, 1.0f);
    auto textureOf = [&](int index) { return index < 0 ? input.texture : texture[size_t(index)]; };

    for (size_t i = 0; i < count; ++i) {
        const PreparedGradeNode& n = grade.nodes[i];
        switch (n.type) {
        case GradeNodeType::Corrector: {
            const GLuint in = textureOf(n.inputs[0]);
            texture[i] = in;
            if (n.bypassed) {
                constAlpha[i] = 0.0f;
                break;
            }
            constAlpha[i] = n.mix;
            if (n.keyed && n.keyNeeded) {
                keyTargets[i] = kit.acquire(size);
                kit.bindTarget(*keyTargets[i]);
                drawCorrector(kit, n, in, 1);
            }
            if (!n.corrects)
                break;
            owned[i] = kit.acquire(size);
            kit.bindTarget(*owned[i]);
            drawCorrector(kit, n, in, 0);
            texture[i] = owned[i]->texture;
            break;
        }
        case GradeNodeType::Parallel: {
            owned[i] = kit.acquire(size);
            kit.bindTarget(*owned[i]);
            gl->glUseProgram(m_parallel.id);
            kit.setFullscreen(m_parallel);
            const int inputs = std::min(int(n.inputs.size()), kMaxMixerInputs);
            gl->glUniform1i(m_parallelCount, inputs);
            for (int k = 0; k < kMaxMixerInputs; ++k)
                kit.bindTexture(k, textureOf(n.inputs[size_t(std::min(k, inputs - 1))]));
            kit.setBlendOver(false);
            kit.drawQuad();
            gl->glActiveTexture(GL_TEXTURE0);
            texture[i] = owned[i]->texture;
            break;
        }
        case GradeNodeType::Layer: {
            GLuint result = textureOf(n.inputs[0]);
            PooledTarget current;
            for (size_t k = 1; k < n.inputs.size(); ++k) {
                const int in = n.inputs[k];
                const PreparedGradeNode* top = in >= 0 ? &grade.nodes[size_t(in)] : nullptr;
                const bool corrector = top && top->type == GradeNodeType::Corrector;
                PooledTarget next = kit.acquire(size);
                kit.bindTarget(*next);
                gl->glUseProgram(m_layer.id);
                kit.setFullscreen(m_layer);
                gl->glUniform1i(m_layerOpaque, corrector ? 0 : 1);
                const bool useKey = corrector && keyTargets[size_t(in)];
                gl->glUniform1i(m_layerUseKey, useKey ? 1 : 0);
                gl->glUniform1f(m_layerConstAlpha, corrector ? constAlpha[size_t(in)] : 1.0f);
                kit.bindTexture(0, result);
                kit.bindTexture(1, corrector ? textureOf(top->inputs[0]) : textureOf(in));
                kit.bindTexture(2, textureOf(in));
                kit.bindTexture(3, useKey ? keyTargets[size_t(in)]->texture : result);
                kit.setBlendOver(false);
                kit.drawQuad();
                gl->glActiveTexture(GL_TEXTURE0);
                current = std::move(next);
                result = current->texture;
            }
            if (current) {
                owned[i] = std::move(current);
                texture[i] = owned[i]->texture;
            } else {
                texture[i] = result;
            }
            break;
        }
        }
    }
    // The output's picture lives in the target of the last node that changed it.
    const GLuint result = texture[size_t(grade.output)];
    for (PooledTarget& t : owned) {
        if (t && t->texture == result)
            return std::move(t);
    }
    return {}; // the output passes the unchanged input through
}

} // namespace kk::render
