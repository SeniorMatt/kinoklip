// SPDX-License-Identifier: GPL-3.0-or-later
// GlKit — shader building, unit quad, render-target pool, blit/fill/blur passes.
#include "render/GlKit.h"

#include "render/RenderLog.h"

#include <algorithm>
#include <cmath>

namespace kk::render {

const char* const kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
uniform mat3 uPosMatrix;
uniform mat3 uUvMatrix;
out vec2 vUv;
out vec2 vQuad;
void main()
{
    // A projective matrix (Stabilization in Perspective mode) leaves its third row in w, so the
    // picture is warped with perspective-correct texture coordinates; affine ones have w = 1.
    vec3 p = uPosMatrix * vec3(aPos, 1.0);
    gl_Position = vec4(p.xy, 0.0, p.z);
    vUv = (uUvMatrix * vec3(aPos, 1.0)).xy;
    vQuad = aPos;
}
)";

namespace {

/// Prepended to every fragment shader: version, the varyings and edgeCoverage().
///
/// edgeCoverage() antialiases the edges of transformed quads without MSAA: fwidth() gives the
/// quad-space size of one pixel, so `min(vQuad, 1 − vQuad) / fwidth` is the distance to the
/// nearest quad edge in pixels. Pixels whose centre is ≥ 0.5 px inside are fully covered,
/// which keeps axis-aligned full-frame quads exact (coverage 1 everywhere).
const char* const kFragmentHeader = R"(#version 330 core
in vec2 vUv;
in vec2 vQuad;
out vec4 fragColor;
float edgeCoverage()
{
    vec2 px = max(fwidth(vQuad), vec2(1e-6));
    vec2 d = min(vQuad, 1.0 - vQuad) / px;
    return clamp(min(d.x, d.y) + 0.5, 0.0, 1.0);
}
)";

const char* const kBlitFragment = R"(
uniform sampler2D uTex;
uniform float uOpacity;
uniform vec4 uCrop; // visible rect in quad coordinates: x0, y0, x1, y1
void main()
{
    vec2 inside = step(uCrop.xy, vQuad) * step(vQuad, uCrop.zw);
    fragColor = texture(uTex, vUv) * (uOpacity * inside.x * inside.y * edgeCoverage());
}
)";

const char* const kFillFragment = R"(
uniform vec4 uColor; // premultiplied
uniform vec4 uCrop;  // visible rect in quad coordinates: x0, y0, x1, y1
void main()
{
    vec2 inside = step(uCrop.xy, vQuad) * step(vQuad, uCrop.zw);
    fragColor = uColor * (inside.x * inside.y * edgeCoverage());
}
)";

// Separable Gaussian with "linear sampling": each tap i > 0 reads between two texels at
// uOffset[i] so the bilinear filter returns their weighted sum — half the texture fetches.
const char* const kBlurFragment = R"(
uniform sampler2D uTex;
uniform vec2 uStep;       // one texel along the blur direction, in uv units
uniform int uTaps;        // used entries of the arrays (centre + pairs)
uniform float uWeight[16];
uniform float uOffset[16];
void main()
{
    vec4 c = texture(uTex, vUv) * uWeight[0];
    for (int i = 1; i < uTaps; ++i) {
        vec2 o = uStep * uOffset[i];
        c += (texture(uTex, vUv + o) + texture(uTex, vUv - o)) * uWeight[i];
    }
    fragColor = c;
}
)";

// Separable min / max (erode / dilate) over 2·uTaps + 1 texels along uStep, per channel. The
// last tap pair counts with weight uFrac, so fractional radii change the result smoothly.
const char* const kMorphFragment = R"(
uniform sampler2D uTex;
uniform vec2 uStep;
uniform int uTaps;
uniform float uFrac;
uniform int uDilate;
void main()
{
    vec4 r = texture(uTex, vUv);
    for (int i = 1; i <= uTaps; ++i) {
        vec4 a = texture(uTex, vUv + uStep * float(i));
        vec4 b = texture(uTex, vUv - uStep * float(i));
        vec4 e = uDilate == 1 ? max(r, max(a, b)) : min(r, min(a, b));
        r = mix(r, e, i == uTaps ? uFrac : 1.0);
    }
    fragColor = r;
}
)";

constexpr GLfloat kQuad[] = {0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1.f, 1.f, 0.f, 1.f};

GLuint compile(GlFunctions* gl, GLenum type, const char* const* sources, int count, QString* log)
{
    const GLuint shader = gl->glCreateShader(type);
    gl->glShaderSource(shader, count, sources, nullptr);
    gl->glCompileShader(shader);
    GLint ok = 0;
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buffer[2048] = {};
        gl->glGetShaderInfoLog(shader, sizeof(buffer), nullptr, buffer);
        *log = QString::fromUtf8(buffer);
        gl->glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

// ---- GlProgram ----

bool GlProgram::build(GlFunctions* gl, const char* name, const char* fragmentSource, QString* log)
{
    QString message;
    const char* vs[] = {kVertexShader};
    const char* fs[] = {kFragmentHeader, fragmentSource};
    const GLuint v = compile(gl, GL_VERTEX_SHADER, vs, 1, &message);
    const GLuint f = v ? compile(gl, GL_FRAGMENT_SHADER, fs, 2, &message) : 0;
    if (!v || !f) {
        if (v)
            gl->glDeleteShader(v);
        *log = QStringLiteral("%1: %2").arg(QLatin1String(name), message);
        return false;
    }
    id = gl->glCreateProgram();
    gl->glAttachShader(id, v);
    gl->glAttachShader(id, f);
    gl->glBindAttribLocation(id, 0, "aPos");
    gl->glLinkProgram(id);
    gl->glDeleteShader(v);
    gl->glDeleteShader(f);
    GLint linked = 0;
    gl->glGetProgramiv(id, GL_LINK_STATUS, &linked);
    if (!linked) {
        char buffer[2048] = {};
        gl->glGetProgramInfoLog(id, sizeof(buffer), nullptr, buffer);
        *log = QStringLiteral("%1: link failed: %2")
                   .arg(QLatin1String(name), QString::fromUtf8(buffer));
        gl->glDeleteProgram(id);
        id = 0;
        return false;
    }
    posMatrix = gl->glGetUniformLocation(id, "uPosMatrix");
    uvMatrix = gl->glGetUniformLocation(id, "uUvMatrix");
    return true;
}

GLint GlProgram::uniform(GlFunctions* gl, const char* name) const
{
    return gl->glGetUniformLocation(id, name);
}

void GlProgram::destroy(GlFunctions* gl)
{
    if (id)
        gl->glDeleteProgram(id);
    id = 0;
}

// ---- PooledTarget / TargetPool ----

PooledTarget::~PooledTarget()
{
    if (m_pool && m_target)
        m_pool->release(m_target);
}

TargetPool::~TargetPool() = default; // GL objects are deleted by destroyAll()

PooledTarget TargetPool::acquire(QSize size, GLenum format)
{
    for (size_t i = 0; i < m_free.size(); ++i) {
        RenderTarget* t = m_free[i];
        if (t->size == size && t->format == format) {
            m_free[i] = m_free.back();
            m_free.pop_back();
            t->idleFrames = 0;
            return PooledTarget(this, t);
        }
    }
    auto t = std::make_unique<RenderTarget>();
    t->size = size;
    t->format = format;
    m_gl->glGenTextures(1, &t->texture);
    m_gl->glBindTexture(GL_TEXTURE_2D, t->texture);
    const GLenum type = format == GL_RGBA16F ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
    m_gl->glTexImage2D(
        GL_TEXTURE_2D, 0, GLint(format), size.width(), size.height(), 0, GL_RGBA, type, nullptr);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_gl->glGenFramebuffers(1, &t->fbo);
    m_gl->glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    m_gl->glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->texture, 0);
    const GLenum status = m_gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        qCWarning(lcRender) << "incomplete framebuffer" << Qt::hex << status << size;
    RenderTarget* raw = t.get();
    m_all.push_back(std::move(t));
    return PooledTarget(this, raw);
}

void TargetPool::release(RenderTarget* target)
{
    target->idleFrames = 0;
    m_free.push_back(target);
}

void TargetPool::trim()
{
    for (size_t i = 0; i < m_free.size();) {
        RenderTarget* t = m_free[i];
        if (++t->idleFrames <= kTrimFrames) {
            ++i;
            continue;
        }
        m_gl->glDeleteFramebuffers(1, &t->fbo);
        m_gl->glDeleteTextures(1, &t->texture);
        m_free[i] = m_free.back();
        m_free.pop_back();
        const auto it
            = std::find_if(m_all.begin(), m_all.end(), [t](const auto& p) { return p.get() == t; });
        if (it != m_all.end())
            m_all.erase(it);
    }
}

void TargetPool::destroyAll()
{
    for (auto& t : m_all) {
        m_gl->glDeleteFramebuffers(1, &t->fbo);
        m_gl->glDeleteTextures(1, &t->texture);
    }
    m_all.clear();
    m_free.clear();
}

// ---- GlKit ----

GlKit::GlKit(GlFunctions* gl)
    : m_gl(gl)
    , m_targets(gl)
{ }

GlKit::~GlKit() = default;

bool GlKit::initialize(QString* why)
{
    if (!m_blit.build(m_gl, "blit", kBlitFragment, why)
        || !m_fill.build(m_gl, "fill", kFillFragment, why)
        || !m_blur.build(m_gl, "blur", kBlurFragment, why)
        || !m_morph.build(m_gl, "morphology", kMorphFragment, why))
        return false;
    m_blitOpacity = m_blit.uniform(m_gl, "uOpacity");
    m_blitCrop = m_blit.uniform(m_gl, "uCrop");
    m_fillColor = m_fill.uniform(m_gl, "uColor");
    m_fillCrop = m_fill.uniform(m_gl, "uCrop");
    m_blurStep = m_blur.uniform(m_gl, "uStep");
    m_blurTaps = m_blur.uniform(m_gl, "uTaps");
    m_blurWeights = m_blur.uniform(m_gl, "uWeight");
    m_blurOffsets = m_blur.uniform(m_gl, "uOffset");
    m_morphStep = m_morph.uniform(m_gl, "uStep");
    m_morphTaps = m_morph.uniform(m_gl, "uTaps");
    m_morphFrac = m_morph.uniform(m_gl, "uFrac");
    m_morphDilate = m_morph.uniform(m_gl, "uDilate");
    m_gl->glUseProgram(m_blit.id);
    m_gl->glUniform1i(m_blit.uniform(m_gl, "uTex"), 0);
    m_gl->glUseProgram(m_blur.id);
    m_gl->glUniform1i(m_blur.uniform(m_gl, "uTex"), 0);
    m_gl->glUseProgram(m_morph.id);
    m_gl->glUniform1i(m_morph.uniform(m_gl, "uTex"), 0);
    m_gl->glUseProgram(0);

    m_gl->glGenVertexArrays(1, &m_vao);
    m_gl->glBindVertexArray(m_vao);
    m_gl->glGenBuffers(1, &m_vbo);
    m_gl->glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    m_gl->glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    m_gl->glEnableVertexAttribArray(0);
    m_gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    // The VAO stays bound for the context's lifetime: every pass draws this quad.

    const GLubyte zero[4] = {0, 0, 0, 0};
    m_gl->glGenTextures(1, &m_transparent);
    m_gl->glBindTexture(GL_TEXTURE_2D, m_transparent);
    m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    m_gl->glDisable(GL_DEPTH_TEST);
    m_gl->glDisable(GL_CULL_FACE);
    m_gl->glDisable(GL_SCISSOR_TEST);
    m_gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return true;
}

void GlKit::destroy()
{
    m_targets.destroyAll();
    m_blit.destroy(m_gl);
    m_fill.destroy(m_gl);
    m_blur.destroy(m_gl);
    m_morph.destroy(m_gl);
    if (m_vbo)
        m_gl->glDeleteBuffers(1, &m_vbo);
    if (m_vao)
        m_gl->glDeleteVertexArrays(1, &m_vao);
    if (m_transparent)
        m_gl->glDeleteTextures(1, &m_transparent);
    m_vbo = m_vao = m_transparent = 0;
}

void GlKit::bindTarget(const RenderTarget& target)
{
    m_gl->glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    m_gl->glViewport(0, 0, target.size.width(), target.size.height());
}

void GlKit::clearTransparent()
{
    m_gl->glClearColor(0.f, 0.f, 0.f, 0.f);
    m_gl->glClear(GL_COLOR_BUFFER_BIT);
}

void GlKit::setBlendOver(bool enabled)
{
    if (enabled)
        m_gl->glEnable(GL_BLEND);
    else
        m_gl->glDisable(GL_BLEND);
}

void GlKit::drawQuad()
{
    m_gl->glDrawArrays(GL_TRIANGLES, 0, 6);
}

void GlKit::setMatrix(GLint location, const QTransform& m)
{
    // QTransform maps (x, y) → (m11·x + m21·y + dx, m12·x + m22·y + dy); as a column-major
    // GLSL mat3 the columns are (m11, m12, m13), (m21, m22, m23), (dx, dy, m33).
    const GLfloat v[9] = {float(m.m11()), float(m.m12()), float(m.m13()), float(m.m21()),
        float(m.m22()), float(m.m23()), float(m.dx()), float(m.dy()), float(m.m33())};
    m_gl->glUniformMatrix3fv(location, 1, GL_FALSE, v);
}

void GlKit::setFullscreen(const GlProgram& program)
{
    setMatrix(program.posMatrix, QTransform(2, 0, 0, 2, -1, -1));
    setMatrix(program.uvMatrix, QTransform());
}

QTransform GlKit::pixelToNdc(QSize size)
{
    return QTransform(2.0 / size.width(), 0, 0, 2.0 / size.height(), -1, -1);
}

QTransform GlKit::quadToNdc(const QRectF& rect, QSize targetSize)
{
    return QTransform(rect.width(), 0, 0, rect.height(), rect.x(), rect.y())
        * pixelToNdc(targetSize);
}

void GlKit::setCrop(GLint location, const QRectF& crop)
{
    m_gl->glUniform4f(
        location, float(crop.left()), float(crop.top()), float(crop.right()), float(crop.bottom()));
}

void GlKit::bindTexture(int unit, GLuint texture)
{
    m_gl->glActiveTexture(GLenum(GL_TEXTURE0 + unit));
    m_gl->glBindTexture(GL_TEXTURE_2D, texture);
}

void GlKit::blit(
    GLuint texture, const QTransform& quadToNdcMatrix, float opacity, const QRectF& crop)
{
    m_gl->glUseProgram(m_blit.id);
    setMatrix(m_blit.posMatrix, quadToNdcMatrix);
    setMatrix(m_blit.uvMatrix, QTransform());
    m_gl->glUniform1f(m_blitOpacity, opacity);
    setCrop(m_blitCrop, crop);
    bindTexture(0, texture);
    drawQuad();
}

PooledTarget GlKit::resample(const RenderTarget& src, QSize size)
{
    PooledTarget dst = acquire(size, src.format);
    bindTarget(*dst);
    setBlendOver(false);
    blit(src.texture, QTransform(2, 0, 0, 2, -1, -1));
    return dst;
}

void GlKit::fill(const QTransform& quadToNdcMatrix, const float rgba[4], const QRectF& crop)
{
    m_gl->glUseProgram(m_fill.id);
    setMatrix(m_fill.posMatrix, quadToNdcMatrix);
    setMatrix(m_fill.uvMatrix, QTransform());
    m_gl->glUniform4fv(m_fillColor, 1, rgba);
    setCrop(m_fillCrop, crop);
    drawQuad();
}

void GlKit::blurPass(
    const RenderTarget& src, const RenderTarget& dst, double sigma, bool horizontal)
{
    // Discrete Gaussian over [-r, r], r = ceil(3σ).
    double raw[2 * kMaxBlurTaps + 2] = {};
    const int radius = std::min(int(std::ceil(3.0 * sigma)), 2 * (kMaxBlurTaps - 1));
    for (int i = 0; i <= radius; ++i)
        raw[i] = std::exp(-0.5 * i * i / (sigma * sigma));
    kernelPass(src, dst, raw, radius, horizontal);
}

void GlKit::kernelPass(const RenderTarget& src, const RenderTarget& dst, const double* raw,
    int radius, bool horizontal)
{
    // Normalise the symmetric kernel raw[0..radius], then merge neighbour pairs (i, i+1) into
    // one bilinear tap at their weighted centre.
    float weights[kMaxBlurTaps] = {};
    float offsets[kMaxBlurTaps] = {};
    radius = std::min(radius, 2 * (kMaxBlurTaps - 1));
    double sum = 0;
    for (int i = 0; i <= radius; ++i)
        sum += i == 0 ? raw[i] : 2.0 * raw[i];
    weights[0] = float(raw[0] / sum);
    int taps = 1;
    for (int i = 1; i <= radius; i += 2) {
        const double w1 = raw[i];
        const double w2 = i + 1 <= radius ? raw[i + 1] : 0.0;
        const double w = w1 + w2;
        if (w <= 0.0)
            continue;
        weights[taps] = float(w / sum);
        offsets[taps] = float((i * w1 + (i + 1) * w2) / w);
        ++taps;
    }

    bindTarget(dst);
    setBlendOver(false);
    m_gl->glUseProgram(m_blur.id);
    setFullscreen(m_blur);
    const float stepX = horizontal ? 1.0f / float(src.size.width()) : 0.0f;
    const float stepY = horizontal ? 0.0f : 1.0f / float(src.size.height());
    m_gl->glUniform2f(m_blurStep, stepX, stepY);
    m_gl->glUniform1i(m_blurTaps, taps);
    m_gl->glUniform1fv(m_blurWeights, kMaxBlurTaps, weights);
    m_gl->glUniform1fv(m_blurOffsets, kMaxBlurTaps, offsets);
    bindTexture(0, src.texture);
    drawQuad();
}

PooledTarget GlKit::gaussianBlur(const RenderTarget& src, double sigmaPx)
{
    constexpr double kNegligibleSigma = 0.25;
    if (sigmaPx < kNegligibleSigma)
        return {};

    // Halve the resolution until sigma fits the direct kernel. Each halving samples at the
    // corner shared by 4 source texels, so the bilinear filter is an exact 2×2 box average.
    PooledTarget scaled;
    const RenderTarget* level = &src;
    double sigma = sigmaPx;
    while (sigma > kMaxDirectSigma && level->size.width() > 8 && level->size.height() > 8) {
        const QSize half((level->size.width() + 1) / 2, (level->size.height() + 1) / 2);
        scaled = resample(*level, half);
        level = scaled.get();
        sigma *= 0.5;
    }
    PooledTarget horizontal = acquire(level->size, src.format);
    blurPass(*level, *horizontal, sigma, true);
    PooledTarget vertical = acquire(level->size, src.format);
    blurPass(*horizontal, *vertical, sigma, false);
    if (level == &src)
        return vertical;
    return resample(*vertical, src.size); // blurred, so bilinear upsampling is invisible
}

PooledTarget GlKit::boxBlur(const RenderTarget& src, double radiusPx)
{
    constexpr double kNegligibleRadius = 0.25;
    if (radiusPx < kNegligibleRadius)
        return {};
    // Same downscaling as gaussianBlur(): a box of radius r on a half-size copy is a box of
    // radius 2r (plus the exact 2x2 prefilter), so the cost stays at most 16 taps per pass.
    constexpr double kMaxDirectRadius = 2.0 * (kMaxBlurTaps - 1);
    PooledTarget scaled;
    const RenderTarget* level = &src;
    double radius = radiusPx;
    while (radius > kMaxDirectRadius && level->size.width() > 8 && level->size.height() > 8) {
        const QSize half((level->size.width() + 1) / 2, (level->size.height() + 1) / 2);
        scaled = resample(*level, half);
        level = scaled.get();
        radius *= 0.5;
    }
    // Weight 1 inside the radius, the fraction for the last texel: radius 2.5 = 2 full taps
    // either side plus two half taps.
    double raw[2 * kMaxBlurTaps + 2] = {};
    const int whole = std::min(int(std::floor(radius)), int(kMaxDirectRadius));
    for (int i = 0; i <= whole; ++i)
        raw[i] = 1.0;
    int extent = whole;
    if (radius - whole > 1e-3 && whole + 1 <= int(kMaxDirectRadius)) {
        raw[whole + 1] = radius - whole;
        extent = whole + 1;
    }
    PooledTarget horizontal = acquire(level->size, src.format);
    kernelPass(*level, *horizontal, raw, extent, true);
    PooledTarget vertical = acquire(level->size, src.format);
    kernelPass(*horizontal, *vertical, raw, extent, false);
    if (level == &src)
        return vertical;
    return resample(*vertical, src.size);
}

PooledTarget GlKit::morphology(const RenderTarget& src, double radiusPx, bool dilate)
{
    if (radiusPx < 0.01)
        return {};
    // A square structuring element is separable (rows, then columns), and dilating by a then
    // by b equals dilating by a + b, so large radii run as several passes of at most
    // kMaxMorphTaps taps each. Cost: radius x 2 fetches per pixel per axis.
    const RenderTarget* current = &src;
    PooledTarget result;
    for (int axis = 0; axis < 2; ++axis) {
        double left = radiusPx;
        while (left > 0.01) {
            const double chunk = std::min(left, double(kMaxMorphTaps));
            left -= chunk;
            const int taps = int(std::ceil(chunk - 1e-6));
            const double frac = chunk - (taps - 1);
            PooledTarget next = acquire(src.size, src.format);
            bindTarget(*next);
            setBlendOver(false);
            m_gl->glUseProgram(m_morph.id);
            setFullscreen(m_morph);
            m_gl->glUniform2f(m_morphStep, axis == 0 ? 1.0f / float(src.size.width()) : 0.0f,
                axis == 0 ? 0.0f : 1.0f / float(src.size.height()));
            m_gl->glUniform1i(m_morphTaps, taps);
            m_gl->glUniform1f(m_morphFrac, float(frac));
            m_gl->glUniform1i(m_morphDilate, dilate ? 1 : 0);
            bindTexture(0, current->texture);
            drawQuad();
            result = std::move(next);
            current = result.get();
        }
    }
    return result;
}

} // namespace kk::render
