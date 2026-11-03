// SPDX-License-Identifier: GPL-3.0-or-later
// GlWarpEffects — the second batch of Distort effects (Twirl, Bulge / Pinch, Kaleidoscope,
// Mirror Split, Corner Pin, Camera Shake) and Tile (Transform) on the GPU. Each is one pass
// remapping coordinates (Camera Shake with motion blur: kShakeSamples remaps averaged).
#include "core/VideoEffectKeys.h"
#include "render/GlEffectBase.h"

#include <QPolygonF>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

double val(const ParamSet& p, const QString& key, int64_t f, double fallback)
{
    return p.value(key, f, fallback);
}

constexpr double kDegToRad = std::numbers::pi / 180.0;
/// Sub-frame positions of the camera shake's motion blur.
constexpr int kShakeSamples = 8;

/// GLSL helpers: the content rectangle (uv) and mirrored / wrapped coordinates inside it.
const char* const kGlslWarp = R"(
uniform vec4 uContent; // content rectangle in uv: x0, y0, x1, y1
// Folds uv back into the content rectangle like a mirror (edges repeat mirrored).
vec2 mirrorInto(vec2 uv)
{
    vec2 size = uContent.zw - uContent.xy;
    vec2 t = (uv - uContent.xy) / size;
    t = 1.0 - abs(mod(t, 2.0) - 1.0);
    return uContent.xy + t * size;
}
// Wraps uv into the content rectangle (repeat).
vec2 wrapInto(vec2 uv)
{
    vec2 size = uContent.zw - uContent.xy;
    return uContent.xy + fract((uv - uContent.xy) / size) * size;
}
bool outside(vec2 uv)
{
    return any(lessThan(uv, uContent.xy)) || any(greaterThan(uv, uContent.zw));
}
)";

/// A warp pass: the shader body after kGlslWarp, the content uniform set automatically.
class WarpEffect : public PassEffect
{
protected:
    bool buildWarp(GlKit& kit, const char* name, const char* body, QString* why)
    {
        const std::string source = std::string(kGlslWarp) + body;
        if (!build(kit, name, source.c_str(), why))
            return false;
        m_content = uniform(kit, "uContent");
        m_size = uniform(kit, "uSize");
        return true;
    }
    /// begin() plus the content rectangle and layer size uniforms.
    PooledTarget beginWarp(GlKit& kit, const EffectInput& in, LayerGeometry* geometry)
    {
        *geometry = layerGeometry(in);
        PooledTarget out = begin(kit, *in.layer);
        kit.gl()->glUniform4f(m_content, geometry->x0, geometry->y0, geometry->x1, geometry->y1);
        kit.gl()->glUniform2f(m_size, geometry->width, geometry->height);
        return out;
    }

    GLint m_content = -1, m_size = -1;
};

// ---- twirl / bulge_pinch: radial remaps around a centre ----

class RadialWarp final : public WarpEffect
{
public:
    explicit RadialWarp(bool bulge)
        : m_bulge(bulge)
    { }

    bool initialize(GlKit& kit, QString* why) override
    {
        if (!buildWarp(kit, m_bulge ? "bulge_pinch" : "twirl", R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform vec2 uCentre;  // uv
uniform float uRadius; // px
uniform float uAmount; // twirl: radians at the centre; bulge: exponent (-0.7..0.7)
uniform int uBulge;
void main()
{
    vec2 p = (vUv - uCentre) * uSize;
    float r = length(p);
    vec2 uv = vUv;
    if (r < uRadius) {
        float t = r / uRadius;
        if (uBulge == 1) {
            // Sample nearer the centre (bulge, magnifies) or farther out (pinch); the
            // factor is 1 at the radius, so the edge of the area is seamless.
            uv = uCentre + p * pow(max(t, 1e-4), uAmount) / uSize;
        } else {
            float a = uAmount * (1.0 - t) * (1.0 - t);
            float c = cos(a), s = sin(a);
            uv = uCentre + vec2(c * p.x - s * p.y, s * p.x + c * p.y) / uSize;
        }
    }
    fragColor = outside(uv) ? vec4(0.0) : texture(uTex, uv);
}
)",
                why))
            return false;
        m_centre = uniform(kit, "uCentre");
        m_radius = uniform(kit, "uRadius");
        m_amount = uniform(kit, "uAmount");
        m_mode = uniform(kit, "uBulge");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return m_bulge ? val(p, k::Amount, f, 50) == 0.0 : val(p, k::Angle, f, 180) == 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        LayerGeometry g;
        PooledTarget out = beginWarp(kit, in, &g);
        float u = 0, v = 0;
        g.centre(in, val(p, k::CenterX, f, 0), val(p, k::CenterY, f, 0), &u, &v);
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(m_centre, u, v);
        gl->glUniform1f(m_radius, float(std::max(1.0, val(p, k::Radius, f, 300) * in.scale)));
        gl->glUniform1f(m_amount,
            m_bulge ? float(0.7 * std::clamp(val(p, k::Amount, f, 50) / 100.0, -1.0, 1.0))
                    : float(val(p, k::Angle, f, 180) * kDegToRad));
        gl->glUniform1i(m_mode, m_bulge ? 1 : 0);
        draw(kit, *in.layer);
        return out;
    }

private:
    bool m_bulge;
    GLint m_centre = -1, m_radius = -1, m_amount = -1, m_mode = -1;
};

std::unique_ptr<GlEffect> makeTwirl()
{
    return std::make_unique<RadialWarp>(false);
}

std::unique_ptr<GlEffect> makeBulge()
{
    return std::make_unique<RadialWarp>(true);
}

// ---- kaleidoscope ----

class Kaleidoscope final : public WarpEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!buildWarp(kit, "kaleidoscope", R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform vec2 uCentre;   // uv
uniform float uSegment; // radians per segment
uniform float uAngle;   // radians
void main()
{
    vec2 p = (vUv - uCentre) * uSize;
    float r = length(p);
    float a = atan(p.y, p.x) - uAngle;
    a = mod(a, uSegment);
    if (a > 0.5 * uSegment)
        a = uSegment - a; // every other copy mirrored: seamless edges
    a += uAngle;
    vec2 uv = uCentre + r * vec2(cos(a), sin(a)) / uSize;
    fragColor = texture(uTex, mirrorInto(uv));
}
)",
                why))
            return false;
        m_centre = uniform(kit, "uCentre");
        m_segment = uniform(kit, "uSegment");
        m_angle = uniform(kit, "uAngle");
        return true;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        LayerGeometry g;
        PooledTarget out = beginWarp(kit, in, &g);
        float u = 0, v = 0;
        g.centre(in, val(p, k::CenterX, f, 0), val(p, k::CenterY, f, 0), &u, &v);
        const double segments = std::max(2.0, std::round(val(p, k::Segments, f, 6)));
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(m_centre, u, v);
        gl->glUniform1f(m_segment, float(2.0 * std::numbers::pi / segments));
        gl->glUniform1f(m_angle, float(-val(p, k::Angle, f, 0) * kDegToRad));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_centre = -1, m_segment = -1, m_angle = -1;
};

// ---- mirror_split ----

class MirrorSplit final : public WarpEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!buildWarp(kit, "mirror_split", R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform vec2 uLine;  // mirror lines (uv): x, y
uniform ivec2 uKeep; // x: 0 none, 1 keep left, 2 keep right; y: 0 none, 1 keep top, 2 bottom
void main()
{
    vec2 uv = vUv;
    if ((uKeep.x == 1 && uv.x > uLine.x) || (uKeep.x == 2 && uv.x < uLine.x))
        uv.x = 2.0 * uLine.x - uv.x;
    if ((uKeep.y == 1 && uv.y > uLine.y) || (uKeep.y == 2 && uv.y < uLine.y))
        uv.y = 2.0 * uLine.y - uv.y;
    fragColor = outside(uv) ? vec4(0.0) : texture(uTex, uv);
}
)",
                why))
            return false;
        m_line = uniform(kit, "uLine");
        m_keep = uniform(kit, "uKeep");
        return true;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        const QString mode = choiceParam(p, k::Mode, fxchoice::KeepLeft);
        int kx = 0, ky = 0;
        if (mode == fxchoice::KeepLeft || mode == fxchoice::Quad)
            kx = 1;
        else if (mode == fxchoice::KeepRight)
            kx = 2;
        if (mode == fxchoice::KeepTop || mode == fxchoice::Quad)
            ky = 1;
        else if (mode == fxchoice::KeepBottom)
            ky = 2;
        LayerGeometry g;
        PooledTarget out = beginWarp(kit, in, &g);
        float u = 0, v = 0;
        g.centre(in, val(p, k::CenterX, f, 0), val(p, k::CenterY, f, 0), &u, &v);
        kit.gl()->glUniform2f(m_line, u, v);
        kit.gl()->glUniform2i(m_keep, kx, ky);
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_line = -1, m_keep = -1;
};

// ---- corner_pin ----

/// The corner offsets of the corner pin, in the order top left, top right, bottom right,
/// bottom left (x, y each).
const QString* const kCornerKeys[8] = {&k::TopLeftX, &k::TopLeftY, &k::TopRightX, &k::TopRightY,
    &k::BottomRightX, &k::BottomRightY, &k::BottomLeftX, &k::BottomLeftY};

class CornerPin final : public WarpEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!buildWarp(kit, "corner_pin", R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform mat3 uInverse; // layer px -> unit square of the content (projective)
void main()
{
    vec3 s = uInverse * vec3(vUv * uSize, 1.0);
    if (s.z <= 0.0) { // behind the "camera" of a folded quad
        fragColor = vec4(0.0);
        return;
    }
    vec2 t = s.xy / s.z;
    // Antialiased edges: coverage from the distance to the unit square in pixels.
    vec2 fw = max(fwidth(t), vec2(1e-6));
    vec2 inside = clamp(min(t, 1.0 - t) / fw + 0.5, 0.0, 1.0);
    vec2 uv = uContent.xy + clamp(t, 0.0, 1.0) * (uContent.zw - uContent.xy);
    fragColor = texture(uTex, uv) * (inside.x * inside.y);
}
)",
                why))
            return false;
        m_inverse = uniform(kit, "uInverse");
        return true;
    }
    double paddingPx(const ParamSet& p, int64_t f) const override
    {
        double most = 0.0;
        for (const QString* key : kCornerKeys)
            most = std::max(most, std::abs(val(p, *key, f, 0)));
        return most + 1.0;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        for (const QString* key : kCornerKeys) {
            if (val(p, *key, f, 0) != 0.0)
                return false;
        }
        return true;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        LayerGeometry g;
        PooledTarget out = beginWarp(kit, in, &g);
        // Target corners in layer px: the content corners moved by the offsets.
        const double x0 = g.x0 * g.width, y0 = g.y0 * g.height;
        const double x1 = g.x1 * g.width, y1 = g.y1 * g.height;
        const double base[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
        QPolygonF quad;
        for (int i = 0; i < 4; ++i) {
            quad << QPointF(base[2 * i] + val(p, *kCornerKeys[2 * i], f, 0) * in.scale,
                base[2 * i + 1] + val(p, *kCornerKeys[2 * i + 1], f, 0) * in.scale);
        }
        // The projective map unit square -> quad, inverted: output pixel -> content position.
        QTransform toQuad;
        QTransform inverse;
        if (QTransform::squareToQuad(quad, toQuad))
            inverse = toQuad.inverted();
        // QTransform maps row vectors, (x, y, 1) · M. The column-vector GLSL matrix is M
        // transposed, which is exactly M's values m11, m12, m13, m21, ... read column-major.
        const float m[9] = {float(inverse.m11()), float(inverse.m12()), float(inverse.m13()),
            float(inverse.m21()), float(inverse.m22()), float(inverse.m23()), float(inverse.m31()),
            float(inverse.m32()), float(inverse.m33())};
        kit.gl()->glUniformMatrix3fv(m_inverse, 1, GL_FALSE, m);
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_inverse = -1;
};

// ---- camera_shake ----

class CameraShake final : public WarpEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string body = std::string(R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform vec2 uCentre; // content centre, px
const int kSamples = )")
            + std::to_string(kShakeSamples) + R"(;
uniform int uSamples;
uniform vec3 uShake[kSamples]; // dx, dy (px), rotation (radians) per sub-frame
uniform float uInvZoom;
void main()
{
    vec2 q = vUv * uSize - uCentre;
    vec4 sum = vec4(0.0);
    for (int i = 0; i < uSamples; ++i) {
        vec3 s = uShake[i];
        vec2 d = q - s.xy;
        float c = cos(-s.z), n = sin(-s.z);
        vec2 src = vec2(c * d.x - n * d.y, n * d.x + c * d.y) * uInvZoom + uCentre;
        // Uncovered edges repeat the picture mirrored (Zoom hides them).
        sum += texture(uTex, mirrorInto(src / uSize));
    }
    fragColor = sum / float(uSamples);
}
)";
        if (!buildWarp(kit, "camera_shake", body.c_str(), why))
            return false;
        m_centre = uniform(kit, "uCentre");
        m_samples = uniform(kit, "uSamples");
        m_shake = uniform(kit, "uShake");
        m_invZoom = uniform(kit, "uInvZoom");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amplitude, f, 12) <= 0.0 && val(p, k::Rotation, f, 1) <= 0.0
            && val(p, k::Zoom, f, 105) == 100.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        const bool blur = boolParam(p, k::MotionBlur, true);
        const int samples = blur ? kShakeSamples : 1;
        float shake[kShakeSamples * 3];
        const double fps = in.fps > 0 ? in.fps : 25.0;
        for (int i = 0; i < samples; ++i) {
            // Sub-frames over half a frame (a 180 degree shutter), centred on the frame.
            const double sub = samples > 1 ? (double(i) / (samples - 1) - 0.5) * 0.5 : 0.0;
            double dx = 0, dy = 0, rot = 0;
            fxmath::cameraShake(p, f, (double(f) + sub) / fps, &dx, &dy, &rot);
            shake[i * 3] = float(dx * in.scale);
            shake[i * 3 + 1] = float(dy * in.scale);
            shake[i * 3 + 2] = float(rot);
        }
        LayerGeometry g;
        PooledTarget out = beginWarp(kit, in, &g);
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(m_centre, 0.5f * g.width, 0.5f * g.height);
        gl->glUniform1i(m_samples, samples);
        gl->glUniform3fv(m_shake, samples, shake);
        gl->glUniform1f(m_invZoom, float(100.0 / std::max(1.0, val(p, k::Zoom, f, 105))));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_centre = -1, m_samples = -1, m_shake = -1, m_invZoom = -1;
};

// ---- tile ----

class Tile final : public WarpEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!buildWarp(kit, "tile", R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform vec2 uCentre;  // uv of the content centre + offset
uniform float uInvTile;
uniform int uMirror;
void main()
{
    vec2 c = 0.5 * (uContent.xy + uContent.zw);
    vec2 uv = c + (vUv - uCentre) * uInvTile;
    fragColor = texture(uTex, uMirror == 1 ? mirrorInto(uv) : wrapInto(uv));
}
)",
                why))
            return false;
        m_centre = uniform(kit, "uCentre");
        m_invTile = uniform(kit, "uInvTile");
        m_mirror = uniform(kit, "uMirror");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Zoom, f, 50) >= 100.0 && val(p, k::OffsetX, f, 0) == 0.0
            && val(p, k::OffsetY, f, 0) == 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        LayerGeometry g;
        PooledTarget out = beginWarp(kit, in, &g);
        float u = 0, v = 0;
        g.centre(in, val(p, k::OffsetX, f, 0), val(p, k::OffsetY, f, 0), &u, &v);
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(m_centre, u, v);
        gl->glUniform1f(m_invTile, float(100.0 / std::clamp(val(p, k::Zoom, f, 50), 1.0, 100.0)));
        gl->glUniform1i(m_mirror, boolParam(p, k::Mirror, false) ? 1 : 0);
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_centre = -1, m_invTile = -1, m_mirror = -1;
};

const GlEffectFactory kWarpTable[] = {
    {fxid::Twirl, &makeTwirl},
    {fxid::BulgePinch, &makeBulge},
    {fxid::Kaleidoscope, &make<Kaleidoscope>},
    {fxid::MirrorSplit, &make<MirrorSplit>},
    {fxid::CornerPin, &make<CornerPin>},
    {fxid::CameraShake, &make<CameraShake>},
    {fxid::Tile, &make<Tile>},
};

} // namespace

std::span<const GlEffectFactory> warpGlEffects()
{
    return kWarpTable;
}

} // namespace kk::render
