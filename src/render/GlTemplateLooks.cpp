// SPDX-License-Identifier: GPL-3.0-or-later
// GlTemplateLooks — the template looks on the GPU (docs/behavior-spec.md section 65): CCTV,
// Digital Glitch, Drone Overlay, DSLR, DVE, Video Call and Video Camera.
//
// Digital Glitch and DVE are one shader over the layer. The overlay looks (CCTV, Drone Overlay,
// DSLR, Video Call, Video Camera) draw their text and marks with LookHud (QPainter) into a
// picture the size of the layer, keep it as a texture until something that shows in it changes,
// and lay it over the picture in their shader (which also does the picture part of the look:
// the CCTV washing, the Video Call tile). Parameter pixels are timeline pixels, multiplied by
// EffectInput::scale; random numbers depend on the pixel, the seed and the clip-local frame
// only, so exports repeat exactly.
#include "core/VideoEffectKeys.h"
#include "render/GlEffectBase.h"
#include "render/GlShaderEffect.h"
#include "render/LookHud.h"

#include <QImage>

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

double pct(const ParamSet& p, const QString& key, int64_t f, double fallback)
{
    return std::clamp(val(p, key, f, fallback), 0.0, 100.0) / 100.0;
}

/// The picture shader of an overlay look ends with this: the overlay (unit 1, premultiplied)
/// goes over the processed picture @c pic (premultiplied).
constexpr char kOverlayTail[] = R"(
uniform sampler2D uSecond;
vec4 withOverlay(vec4 pic)
{
    vec4 ov = texture(uSecond, vUv);
    return vec4(pic.rgb * (1.0 - ov.a) + ov.rgb, ov.a + pic.a * (1.0 - ov.a));
}
)";

/// An effect whose last step is an overlay painted by LookHud.
class OverlayLook final : public PassEffect
{
public:
    using Setter = void (*)(GlKit& kit, const GLint* loc, const EffectInput& in);
    using Identity = bool (*)(const ParamSet& p, int64_t frame);

    struct Spec
    {
        const char* id;
        const char* fragment;
        std::vector<const char*> uniforms;
        Setter setter;
        Identity identity;
    };

    explicit OverlayLook(Spec spec)
        : m_spec(std::move(spec))
    { }

    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = std::string(kGlslHash) + kOverlayTail + m_spec.fragment;
        if (!build(kit, m_spec.id, source.c_str(), why, true))
            return false;
        for (const char* name : m_spec.uniforms)
            m_loc.push_back(uniform(kit, name));
        return true;
    }
    void destroy(GlKit& kit) override
    {
        PassEffect::destroy(kit);
        if (m_texture)
            kit.gl()->glDeleteTextures(1, &m_texture);
        m_texture = 0;
        m_key.clear();
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return m_spec.identity && m_spec.identity(p, f);
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        upload(kit, in);
        PooledTarget out = begin(kit, *in.layer);
        m_spec.setter(kit, m_loc.data(), in);
        draw(kit, *in.layer, m_texture);
        return out;
    }

private:
    /// Paints and uploads the overlay unless the texture already shows this very picture.
    void upload(GlKit& kit, const EffectInput& in)
    {
        const QString id = QLatin1String(m_spec.id);
        lookhud::Frame frame;
        frame.size = in.layer->size;
        frame.content = QRectF(in.paddingPx, in.paddingPx,
            in.layer->size.width() - 2 * in.paddingPx, in.layer->size.height() - 2 * in.paddingPx);
        frame.scale = in.scale;
        frame.localFrame = in.localFrame;
        frame.fps = in.fps;
        const QString key = lookhud::cacheKey(id, *in.params, frame);
        if (m_texture && key == m_key)
            return;
        const QImage image = lookhud::paint(id, *in.params, frame);
        GlFunctions* gl = kit.gl();
        if (!m_texture)
            gl->glGenTextures(1, &m_texture);
        kit.bindTexture(1, m_texture);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(image.bytesPerLine() / 4));
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width(), image.height(), 0, GL_BGRA,
            GL_UNSIGNED_BYTE, image.constBits());
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        m_key = key;
    }

    Spec m_spec;
    std::vector<GLint> m_loc;
    GLuint m_texture = 0;
    QString m_key;
};

/// Content rectangle in layer pixels as (x0, y0, x1, y1).
void setRect(GlFunctions* gl, GLint loc, const LayerGeometry& g)
{
    gl->glUniform4f(loc, g.x0 * g.width, g.y0 * g.height, g.x1 * g.width, g.y1 * g.height);
}

// ---- CCTV ----

constexpr char kCctvShader[] = R"(
uniform sampler2D uTex;
uniform vec4 uRect;     // content rectangle, layer px
uniform vec2 uSizePx;
uniform vec4 uAmounts;  // desaturation, noise, scanlines, vignette (0..1)
uniform uint uSlot;     // clip-local frame (changes the noise)
void main()
{
    vec4 t = texture(uTex, vUv);
    vec3 c = straight(t);
    vec2 px = vUv * uSizePx;
    float unit = (uRect.w - uRect.y) / 1080.0;
    // Washed, slightly green-grey colours with a little extra contrast, like a cheap sensor.
    float l = luma(c);
    c = mix(c, vec3(l) * vec3(0.95, 1.0, 0.96), uAmounts.x);
    c = (c - 0.5) * (1.0 + 0.2 * uAmounts.x) + 0.5;
    // Sensor noise, new every frame.
    float n = hash2(ivec2(px), uSlot * 2654435761u) - 0.5;
    c += n * 0.4 * uAmounts.y;
    // Scanlines: every second line of about 2 design pixels is darker.
    float line = floor((px.y - uRect.y) / max(2.0 * unit, 1.0));
    c *= 1.0 - uAmounts.z * 0.4 * mod(line, 2.0);
    // Corner shading.
    vec2 q = (px - 0.5 * (uRect.xy + uRect.zw)) / (0.5 * (uRect.zw - uRect.xy));
    c *= 1.0 - uAmounts.w * 0.75 * smoothstep(0.35, 1.5, dot(q, q));
    vec4 pic = vec4(clamp(c, 0.0, 1.0) * t.a, t.a);
    fragColor = withOverlay(pic);
}
)";

void cctvSetter(GlKit& kit, const GLint* loc, const EffectInput& in)
{
    const ParamSet& p = *in.params;
    const LayerGeometry g = layerGeometry(in);
    GlFunctions* gl = kit.gl();
    setRect(gl, loc[0], g);
    gl->glUniform2f(loc[1], g.width, g.height);
    gl->glUniform4f(loc[2], float(pct(p, k::Desaturate, in.localFrame, 85)),
        float(pct(p, k::Noise, in.localFrame, 25)),
        float(pct(p, k::ScanlineAmount, in.localFrame, 30)),
        float(pct(p, k::VignetteAmount, in.localFrame, 40)));
    gl->glUniform1ui(loc[3], GLuint(std::max<int64_t>(0, in.localFrame)));
}

bool cctvIdentity(const ParamSet& p, int64_t f)
{
    return pct(p, k::OverlayOpacity, f, 100) <= 0.0 && pct(p, k::Desaturate, f, 85) <= 0.0
        && pct(p, k::Noise, f, 25) <= 0.0 && pct(p, k::ScanlineAmount, f, 30) <= 0.0
        && pct(p, k::VignetteAmount, f, 40) <= 0.0;
}

// ---- Drone Overlay, DSLR, Video Camera: only the overlay ----

constexpr char kOverlayOnlyShader[] = R"(
uniform sampler2D uTex;
void main()
{
    fragColor = withOverlay(texture(uTex, vUv));
}
)";

void noUniforms(GlKit&, const GLint*, const EffectInput&) { }

bool overlayIdentity(const ParamSet& p, int64_t f)
{
    return pct(p, k::OverlayOpacity, f, 100) <= 0.0;
}

// ---- Video Call ----

constexpr char kVideoCallShader[] = R"(
uniform sampler2D uTex;
uniform vec4 uRect;     // content rectangle, layer px
uniform vec2 uSizePx;
uniform vec4 uTile;     // tile rectangle, layer px
uniform float uRadius;  // tile corner radius, layer px
uniform vec4 uBackground; // straight colour, opacity
void main()
{
    vec2 px = vUv * uSizePx;
    vec2 centre = 0.5 * (uTile.xy + uTile.zw), half_ = 0.5 * (uTile.zw - uTile.xy);
    float r = clamp(uRadius, 0.0, max(min(half_.x, half_.y), 0.0));
    vec2 d = abs(px - centre) - half_ + r;
    float sd = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
    float cover = clamp(0.5 - sd, 0.0, 1.0);
    // The tile shows the whole picture scaled to cover the tile (a little is cropped).
    vec2 content = uRect.zw - uRect.xy;
    float k = max((uTile.z - uTile.x) / content.x, (uTile.w - uTile.y) / content.y);
    vec2 sp = 0.5 * (uRect.xy + uRect.zw) + (px - centre) / max(k, 1e-4);
    vec4 pic = texture(uTex, clamp(sp, vec2(0.5), uSizePx - 0.5) / uSizePx) * cover;
    vec4 bg = vec4(uBackground.rgb * uBackground.a, uBackground.a);
    fragColor = withOverlay(pic + bg * (1.0 - pic.a));
}
)";

void videoCallSetter(GlKit& kit, const GLint* loc, const EffectInput& in)
{
    const ParamSet& p = *in.params;
    const LayerGeometry g = layerGeometry(in);
    GlFunctions* gl = kit.gl();
    setRect(gl, loc[0], g);
    gl->glUniform2f(loc[1], g.width, g.height);
    const double m = val(p, k::TileMargin, in.localFrame, 48) * in.scale;
    gl->glUniform4f(loc[2], float(g.x0 * g.width + m), float(g.y0 * g.height + m),
        float(g.x1 * g.width - m), float(g.y1 * g.height - m));
    gl->glUniform1f(loc[3], float(val(p, k::CornerRadius, in.localFrame, 28) * in.scale));
    const fxmath::Rgb c = colorParam(p, k::BackgroundColor, {0.125f, 0.133f, 0.157f});
    gl->glUniform4f(loc[4], c.r, c.g, c.b, float(pct(p, k::BackgroundOpacity, in.localFrame, 100)));
}

// ---- Digital Glitch ----

class DigitalGlitch final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = std::string(kGlslHash) + R"(
uniform sampler2D uTex;
uniform vec2 uSizePx;
uniform vec4 uParams;   // amount (0..1), band shift px, block px, colour split px
uniform vec2 uNoise;    // digital noise (0..1), unused
uniform uint uSlot;     // which glitch pattern is showing
uniform uint uSeed;
vec4 at(vec2 p) { return texture(uTex, fract(p / uSizePx)); } // wraps round, like a bad scan
void main()
{
    float amount = uParams.x, shift = uParams.y, block = uParams.z, split = uParams.w;
    vec2 px = vUv * uSizePx;
    uint s = uSeed * 7919u + uSlot * 104729u;
    // Pattern-wide flicker: some patterns are calmer than others.
    float calm = hash01(s + 11u);
    float level = amount * mix(0.4, 1.0, calm);
    // Horizontal bands of random height slide sideways.
    float bandH = max(2.0, block * mix(0.15, 0.6, hash01(s + 3u)));
    float band = floor(px.y / bandH);
    float bandOn = hash01(uint(band) * 1315423911u + s) < level * 0.5 ? 1.0 : 0.0;
    float bandShift = (hash01(uint(band) * 2246822519u + s + 5u) - 0.5) * 2.0 * shift * level;
    // Square blocks jump to another place.
    ivec2 cell = ivec2(floor(px / block));
    float blockOn = hash2(cell, s + 17u) < level * 0.16 ? 1.0 : 0.0;
    vec2 blockMove = (vec2(hash2(cell, s + 19u), hash2(cell, s + 23u)) - 0.5) * block * 3.0 * level;
    vec2 p = px + vec2(bandShift * bandOn, 0.0) + blockMove * blockOn;
    // Colour channels split apart where something moved, and a little everywhere in rough patterns.
    float active_ = max(bandOn, blockOn);
    float rough = hash01(s + 29u) < level * 0.35 ? 0.35 : 0.0;
    float sp = split * level * max(active_, rough);
    vec4 sr = at(p + vec2(sp, 0.0)), sg = at(p), sb = at(p - vec2(sp, 0.0));
    vec3 c = vec3(sr.r, sg.g, sb.b);
    float a = (sr.a + sg.a + sb.a) / 3.0;
    // Digital noise: coloured speckles inside the moved blocks and fine flicker on moved bands.
    float cellNoise = hash2(ivec2(floor(px / max(block * 0.125, 2.0))), s + 31u);
    if (blockOn > 0.5 && cellNoise < uNoise.x * 0.6)
        c = mix(c, vec3(hash2(ivec2(px) / 3, s + 37u), hash2(ivec2(px) / 3, s + 41u), hash2(ivec2(px) / 3, s + 43u)) * a, 0.85);
    c += (hash2(ivec2(px), s + 47u) - 0.5) * 0.25 * uNoise.x * level * active_ * a;
    fragColor = vec4(clamp(c, 0.0, 1.0), a);
}
)";
        if (!build(kit, "look_digital_glitch", source.c_str(), why, true))
            return false;
        m_size = uniform(kit, "uSizePx");
        m_params = uniform(kit, "uParams");
        m_noise = uniform(kit, "uNoise");
        m_slot = uniform(kit, "uSlot");
        m_seed = uniform(kit, "uSeed");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return val(p, k::Amount, f, 50) <= 0.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const LayerGeometry g = layerGeometry(in);
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        gl->glUniform2f(m_size, g.width, g.height);
        gl->glUniform4f(m_params, float(pct(p, k::Amount, in.localFrame, 50)),
            float(val(p, k::ShiftAmount, in.localFrame, 120) * in.scale),
            float(std::max(2.0, val(p, k::BlockSize, in.localFrame, 64) * in.scale)),
            float(val(p, k::ColorSplit, in.localFrame, 8) * in.scale));
        gl->glUniform2f(m_noise, float(pct(p, k::Noise, in.localFrame, 20)), 0.f);
        // A new pattern every 1 / rate seconds.
        const double seconds = in.fps > 0 ? double(in.localFrame) / in.fps : 0.0;
        const double rate = std::clamp(val(p, k::GlitchRate, in.localFrame, 8), 0.5, 50.0);
        gl->glUniform1ui(m_slot, GLuint(std::floor(seconds * rate)));
        gl->glUniform1ui(m_seed, GLuint(std::max(0.0, val(p, k::Seed, in.localFrame, 1))));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_size = -1, m_params = -1, m_noise = -1, m_slot = -1, m_seed = -1;
};

// ---- DVE ----

class Dve final : public PassEffect
{
public:
    bool initialize(GlKit& kit, QString* why) override
    {
        if (!build(kit, "look_dve", R"(
uniform sampler2D uTex;
uniform vec2 uSizePx;
uniform vec4 uBox;      // content centre (px) and half size (px)
uniform vec2 uMove;     // box centre offset, px
uniform float uScale;   // box size, 1 = the whole picture
uniform mat3 uRot;      // box rotation (columns), applied to the plane of the picture
uniform float uDist;    // camera distance, px
uniform vec4 uBorder;   // width px, radius px, shadow blur px, shadow opacity
uniform vec3 uBorderColor;
uniform vec2 uShadowOffset;
uniform float uOpacity;
// Position inside the unscaled picture plane (px from the centre) of the screen point s, with the
// signed distance to the box edge in screen px (negative inside).
vec3 plane(vec2 s)
{
    vec3 n = uRot[2];
    float den = n.z * uDist - n.x * s.x - n.y * s.y;
    float t = (abs(den) < 1e-3) ? 1e6 : n.z * uDist / den;
    vec3 P = vec3(t * s.x, t * s.y, uDist - t * uDist);
    vec3 q = transpose(uRot) * P;
    float k = max(uScale, 1e-4);
    vec2 u = q.xy / k;
    float r = clamp(uBorder.y / k, 0.0, min(uBox.z, uBox.w));
    vec2 d = abs(u) - uBox.zw + r;
    float sd = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
    return vec3(u, sd * k * (t > 0.0 ? 1.0 : 1e3));
}
void main()
{
    vec2 px = vUv * uSizePx;
    vec3 b = plane(px - uBox.xy - uMove);
    float cover = clamp(0.5 - b.z, 0.0, 1.0);
    vec4 pic = texture(uTex, clamp(uBox.xy + b.xy, vec2(0.5), uSizePx - 0.5) / uSizePx);
    // Border: the outermost uBorder.x px inside the box edge.
    float edge = clamp(0.5 + b.z + uBorder.x, 0.0, 1.0);
    vec4 box = vec4(mix(pic.rgb, uBorderColor, uBorder.x > 0.0 ? edge : 0.0),
                    mix(pic.a, 1.0, uBorder.x > 0.0 ? edge : 0.0)) * cover;
    // Shadow: the same box shifted, softened over the blur width.
    vec3 sb = plane(px - uBox.xy - uMove - uShadowOffset);
    float soft = max(uBorder.z, 1.0);
    float sh = uBorder.w * (1.0 - smoothstep(-0.5 * soft, 0.5 * soft, sb.z));
    vec4 shadow = vec4(0.0, 0.0, 0.0, sh);
    fragColor = (box + shadow * (1.0 - box.a)) * uOpacity;
}
)",
                why, true))
            return false;
        m_size = uniform(kit, "uSizePx");
        m_box = uniform(kit, "uBox");
        m_move = uniform(kit, "uMove");
        m_scale = uniform(kit, "uScale");
        m_rot = uniform(kit, "uRot");
        m_dist = uniform(kit, "uDist");
        m_border = uniform(kit, "uBorder");
        m_borderColor = uniform(kit, "uBorderColor");
        m_shadowOffset = uniform(kit, "uShadowOffset");
        m_opacity = uniform(kit, "uOpacity");
        return true;
    }
    bool isIdentity(const ParamSet& p, int64_t f) const override
    {
        return std::abs(val(p, k::Scale, f, 60) - 100.0) < 1e-6 && val(p, k::PosX, f, 0) == 0.0
            && val(p, k::PosY, f, 0) == 0.0 && val(p, k::Rotation, f, 0) == 0.0
            && val(p, k::TiltX, f, 0) == 0.0 && val(p, k::TiltY, f, 0) == 0.0
            && val(p, k::BorderWidth, f, 8) <= 0.0 && val(p, k::CornerRadius, f, 12) <= 0.0
            && val(p, k::Opacity, f, 100) >= 100.0;
    }
    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const ParamSet& p = *in.params;
        const int64_t f = in.localFrame;
        const LayerGeometry g = layerGeometry(in);
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        const float halfW = 0.5f * (g.x1 - g.x0) * g.width, halfH = 0.5f * (g.y1 - g.y0) * g.height;
        gl->glUniform2f(m_size, g.width, g.height);
        gl->glUniform4f(
            m_box, 0.5f * (g.x0 + g.x1) * g.width, 0.5f * (g.y0 + g.y1) * g.height, halfW, halfH);
        gl->glUniform2f(m_move, float(val(p, k::PosX, f, 0) * in.scale),
            float(val(p, k::PosY, f, 0) * in.scale));
        gl->glUniform1f(m_scale, float(std::max(0.0, val(p, k::Scale, f, 60)) / 100.0));
        // R = Ry(tilt Y) * Rx(tilt X) * Rz(rotation), column-major.
        const double rad = std::numbers::pi / 180.0;
        const double a = val(p, k::Rotation, f, 0) * rad, bx = val(p, k::TiltX, f, 0) * rad,
                     by = val(p, k::TiltY, f, 0) * rad;
        const double ca = std::cos(a), sa = std::sin(a), cx = std::cos(bx), sx = std::sin(bx),
                     cy = std::cos(by), sy = std::sin(by);
        const double rz[3][3] = {{ca, -sa, 0}, {sa, ca, 0}, {0, 0, 1}};
        const double rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
        const double ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
        double t1[3][3] = {}, m[3][3] = {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int l = 0; l < 3; ++l)
                    t1[i][j] += rx[i][l] * rz[l][j];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int l = 0; l < 3; ++l)
                    m[i][j] += ry[i][l] * t1[l][j];
        float columns[9];
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row)
                columns[col * 3 + row] = float(m[row][col]);
        gl->glUniformMatrix3fv(m_rot, 1, GL_FALSE, columns);
        gl->glUniform1f(m_dist, float(2.0 * std::max(g.width, g.height)));
        gl->glUniform4f(m_border, float(val(p, k::BorderWidth, f, 8) * in.scale),
            float(val(p, k::CornerRadius, f, 12) * in.scale),
            float(val(p, k::ShadowBlur, f, 24) * in.scale), float(pct(p, k::ShadowOpacity, f, 55)));
        const fxmath::Rgb c = colorParam(p, k::BorderColor, {0.96f, 0.96f, 0.96f});
        gl->glUniform3f(m_borderColor, c.r, c.g, c.b);
        gl->glUniform2f(m_shadowOffset, float(val(p, k::ShadowOffsetX, f, 0) * in.scale),
            float(val(p, k::ShadowOffsetY, f, 12) * in.scale));
        gl->glUniform1f(m_opacity, float(pct(p, k::Opacity, f, 100)));
        draw(kit, *in.layer);
        return out;
    }

private:
    GLint m_size = -1, m_box = -1, m_move = -1, m_scale = -1, m_rot = -1, m_dist = -1,
          m_border = -1, m_borderColor = -1, m_shadowOffset = -1, m_opacity = -1;
};

std::unique_ptr<GlEffect> makeOverlay(const OverlayLook::Spec& spec)
{
    return std::make_unique<OverlayLook>(spec);
}

std::unique_ptr<GlEffect> makeCctv()
{
    return makeOverlay({fxid::LookCctv, kCctvShader, {"uRect", "uSizePx", "uAmounts", "uSlot"},
        &cctvSetter, &cctvIdentity});
}
std::unique_ptr<GlEffect> makeDrone()
{
    return makeOverlay(
        {fxid::LookDroneOverlay, kOverlayOnlyShader, {}, &noUniforms, &overlayIdentity});
}
std::unique_ptr<GlEffect> makeDslr()
{
    return makeOverlay({fxid::LookDslr, kOverlayOnlyShader, {}, &noUniforms, &overlayIdentity});
}
std::unique_ptr<GlEffect> makeVideoCamera()
{
    return makeOverlay(
        {fxid::LookVideoCamera, kOverlayOnlyShader, {}, &noUniforms, &overlayIdentity});
}
std::unique_ptr<GlEffect> makeVideoCall()
{
    return makeOverlay({fxid::LookVideoCall, kVideoCallShader,
        {"uRect", "uSizePx", "uTile", "uRadius", "uBackground"}, &videoCallSetter, nullptr});
}

const GlEffectFactory kTable[] = {
    {fxid::LookCctv, &makeCctv},
    {fxid::LookDigitalGlitch, &make<DigitalGlitch>},
    {fxid::LookDroneOverlay, &makeDrone},
    {fxid::LookDslr, &makeDslr},
    {fxid::LookDve, &make<Dve>},
    {fxid::LookVideoCall, &makeVideoCall},
    {fxid::LookVideoCamera, &makeVideoCamera},
};

} // namespace

std::span<const GlEffectFactory> templateLookGlEffects()
{
    return kTable;
}

} // namespace kk::render
