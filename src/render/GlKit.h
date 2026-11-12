// SPDX-License-Identifier: GPL-3.0-or-later
// GlKit — the small OpenGL 3.3 core toolkit the compositor, effects and transitions share:
// shader programs, the unit quad, pooled render targets and common passes (blit, blur).
#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTransform>

#include <atomic>
#include <memory>
#include <vector>

namespace kk::render {

using GlFunctions = QOpenGLFunctions_3_3_Core;

/// The vertex shader every pass uses (GLSL 330). Inputs: a unit quad (0..1, (0,0) = top-left
/// corner). Uniforms `uPosMatrix` (quad → normalised device coordinates) and `uUvMatrix`
/// (quad → source texture coordinates). Outputs `vUv` and `vQuad` (position in the quad, for
/// crop, vignette and edge antialiasing).
///
/// Image orientation convention (used everywhere in the compositor): texture row 0 = TOP of
/// the picture, framebuffer row 0 = top, NDC y = −1 = top. Nothing is ever flipped, and
/// glReadPixels returns rows top-down exactly like QImage.
extern const char* const kVertexShader;

/// A compiled program with the two matrix uniforms of kVertexShader.
struct GlProgram
{
    GLuint id = 0;
    GLint posMatrix = -1;
    GLint uvMatrix = -1;

    /// Compiles kVertexShader + @p fragmentSource and links. @return false with the info log
    /// in @p log. @p name is used in messages.
    bool build(GlFunctions* gl, const char* name, const char* fragmentSource, QString* log);
    /// Uniform location (call once after build; cache the result).
    GLint uniform(GlFunctions* gl, const char* name) const;
    void destroy(GlFunctions* gl);
};

/// A colour-renderable texture + framebuffer object of a fixed size and format.
struct RenderTarget
{
    GLuint texture = 0;
    GLuint fbo = 0;
    QSize size;
    GLenum format = 0; ///< internal format (GL_RGBA16F intermediates, GL_RGBA8 output)
    int idleFrames = 0; ///< frames spent unused in the pool (for trimming)
};

/// GPU bytes of a target of @p size / @p format (RGBA16F = 8 per pixel, else 4).
[[nodiscard]] int64_t targetBytes(QSize size, GLenum format);

class TargetPool;

/// Move-only handle returning its RenderTarget to the pool on destruction.
class PooledTarget
{
public:
    PooledTarget() = default;
    PooledTarget(TargetPool* pool, RenderTarget* target)
        : m_pool(pool)
        , m_target(target)
    { }
    PooledTarget(PooledTarget&& other) noexcept { swap(other); }
    PooledTarget& operator=(PooledTarget&& other) noexcept
    {
        PooledTarget tmp(std::move(other));
        swap(tmp);
        return *this;
    }
    PooledTarget(const PooledTarget&) = delete;
    PooledTarget& operator=(const PooledTarget&) = delete;
    ~PooledTarget();

    [[nodiscard]] RenderTarget* get() const { return m_target; }
    RenderTarget* operator->() const { return m_target; }
    RenderTarget& operator*() const { return *m_target; }
    explicit operator bool() const { return m_target != nullptr; }

private:
    void swap(PooledTarget& o) noexcept
    {
        std::swap(m_pool, o.m_pool);
        std::swap(m_target, o.m_target);
    }
    TargetPool* m_pool = nullptr;
    RenderTarget* m_target = nullptr;
};

/// Pool of render targets keyed by (size, format). Steady-state frames allocate nothing:
/// the same layers/temporaries are handed out again every frame. Targets unused for
/// kTrimFrames frames are deleted by trim().
class TargetPool
{
public:
    explicit TargetPool(GlFunctions* gl)
        : m_gl(gl)
    { }
    ~TargetPool();

    /// A target of exactly @p size / @p format (contents undefined).
    [[nodiscard]] PooledTarget acquire(QSize size, GLenum format);
    /// Returns @p target to the free list (called by PooledTarget).
    void release(RenderTarget* target);
    /// Ages free targets and deletes those idle for too long. Call once per frame.
    void trim();
    /// Bytes held by all targets (in use and free).
    [[nodiscard]] int64_t bytes() const { return m_bytes; }
    /// Free targets are deleted, oldest-idle first, when a new allocation would take the total
    /// above this. Targets in use are never touched, so a frame always completes.
    void setByteLimit(int64_t bytes) { m_limit = bytes; }
    [[nodiscard]] int64_t byteLimit() const { return m_limit; }
    [[nodiscard]] size_t targetCount() const { return m_all.size(); }
    /// Deletes every free (idle) target; the ones in use stay (current context required).
    void trimAll();
    /// Deletes all GL objects (current context required).
    void destroyAll();
    /// Bytes held by the targets of all pools of the process (every Compositor has its own
    /// context and pool: the viewer, the Deliver preview, the render, ...).
    [[nodiscard]] static int64_t processBytes() { return s_processBytes.load(); }
    /// Free targets are also deleted when the pools of the process together would go above
    /// this.
    [[nodiscard]] static int64_t processByteLimit() { return s_processLimit.load(); }
    static void setProcessByteLimit(int64_t bytes) { s_processLimit = bytes; }

private:
    static constexpr int kTrimFrames = 120; ///< ~5 s of playback at 25 fps
    GlFunctions* m_gl;
    std::vector<std::unique_ptr<RenderTarget>> m_all;
    std::vector<RenderTarget*> m_free;
    int64_t m_bytes = 0;
    int64_t m_limit = 1024LL << 20;
    static inline std::atomic<int64_t> s_processBytes {0};
    static inline std::atomic<int64_t> s_processLimit {2048LL << 20};
    void deleteTarget(RenderTarget* t);
    void evictFor(int64_t incoming);
};

/// Shared GL helpers. Owned by the GL renderer; effects and transitions receive a reference.
/// All calls require the render thread's context to be current.
class GlKit
{
public:
    explicit GlKit(GlFunctions* gl);
    ~GlKit();

    /// Builds the common programs and the quad. @return false with a reason on failure.
    bool initialize(QString* why);
    /// Deletes all GL objects.
    void destroy();

    [[nodiscard]] GlFunctions* gl() const { return m_gl; }
    [[nodiscard]] TargetPool& targets() { return m_targets; }

    /// Pooled RGBA16F target (the intermediate format) or @p format.
    [[nodiscard]] PooledTarget acquire(QSize size, GLenum format = GL_RGBA16F)
    {
        return m_targets.acquire(size, format);
    }

    /// Binds @p target as the draw framebuffer and sets the viewport to its size.
    void bindTarget(const RenderTarget& target);
    /// Clears the bound target to transparent black.
    void clearTransparent();
    /// Enables premultiplied "over" blending (src + dst·(1 − src.a)), or disables blending.
    void setBlendOver(bool enabled);
    /// Draws the unit quad (two triangles) with the current program.
    void drawQuad();

    /// Sets uPosMatrix/uUvMatrix of @p program (current) for a full-target pass.
    void setFullscreen(const GlProgram& program);
    /// Uploads a QTransform as a GLSL mat3 to @p location of the current program.
    void setMatrix(GLint location, const QTransform& m);
    /// Unit quad → pixel rectangle @p rect (y down) of a target of @p targetSize, as NDC.
    [[nodiscard]] static QTransform quadToNdc(const QRectF& rect, QSize targetSize);
    /// Pixel coordinates (y down) of a target of @p size → normalised device coordinates.
    [[nodiscard]] static QTransform pixelToNdc(QSize size);

    /// Binds @p texture to texture unit @p unit.
    void bindTexture(int unit, GLuint texture);

    /// Draws @p texture (premultiplied RGBA) through @p quadToNdcMatrix with @p opacity into
    /// the bound target, uv = identity over the texture; only the part of the quad inside
    /// @p crop (quad coordinates) is drawn. Blending as currently set.
    void blit(GLuint texture, const QTransform& quadToNdcMatrix, float opacity = 1.0f,
        const QRectF& crop = QRectF(0, 0, 1, 1));
    /// Copies @p src to a new target of @p size (bilinear resample), no blending.
    [[nodiscard]] PooledTarget resample(const RenderTarget& src, QSize size);
    /// Fills the bound target area covered by @p quadToNdcMatrix with premultiplied @p rgba.
    void fill(const QTransform& quadToNdcMatrix, const float rgba[4],
        const QRectF& crop = QRectF(0, 0, 1, 1));
    /// Sets a vec4 crop uniform (x0, y0, x1, y1) from @p crop.
    void setCrop(GLint location, const QRectF& crop);

    /// Separable Gaussian blur of @p src with standard deviation @p sigmaPx (target pixels).
    /// Large sigmas are blurred on a 2^k downscaled copy (sigma ≤ kMaxDirectSigma there) and
    /// scaled back, so the cost is bounded (~13 bilinear taps per pass) for any radius.
    /// Edges clamp. @return a new target of src's size; empty handle when sigma is negligible.
    [[nodiscard]] PooledTarget gaussianBlur(const RenderTarget& src, double sigmaPx);

    /// Separable box blur of @p src: every texel within @p radiusPx (target pixels; the
    /// fraction weights the outermost texel) counts equally. Large radii run on a downscaled
    /// copy like gaussianBlur(). @return a new target; empty handle when the radius is
    /// negligible.
    [[nodiscard]] PooledTarget boxBlur(const RenderTarget& src, double radiusPx);
    /// Erodes (per-channel minimum, @p dilate false) or dilates (maximum) @p src over a
    /// square of half-width @p radiusPx target pixels (fractional radii blend the outermost
    /// texels). Used for matte shrink / grow and strokes. Edges clamp. @return a new target;
    /// empty handle when the radius is negligible.
    [[nodiscard]] PooledTarget morphology(const RenderTarget& src, double radiusPx, bool dilate);

    /// 1×1 transparent texture (stand-in for a missing transition side).
    [[nodiscard]] GLuint transparentTexture() const { return m_transparent; }

private:
    /// Above this sigma the blur runs on a downscaled copy.
    static constexpr double kMaxDirectSigma = 6.0;
    /// Uniform array size of the blur shader (centre tap + pairs of linear-filtered taps).
    static constexpr int kMaxBlurTaps = 16;

    /// Taps of one morphology pass (more radius = more passes).
    static constexpr int kMaxMorphTaps = 16;

    void blurPass(const RenderTarget& src, const RenderTarget& dst, double sigma, bool horizontal);
    /// One separable pass of the symmetric kernel raw[0..radius] (any non-negative weights;
    /// normalised here), using linear-sampled tap pairs.
    void kernelPass(const RenderTarget& src, const RenderTarget& dst, const double* raw, int radius,
        bool horizontal);

    GlFunctions* m_gl;
    TargetPool m_targets;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLuint m_transparent = 0;

    GlProgram m_blit;
    GLint m_blitOpacity = -1;
    GLint m_blitCrop = -1;
    GlProgram m_fill;
    GLint m_fillColor = -1;
    GLint m_fillCrop = -1;
    GlProgram m_blur;
    GLint m_blurStep = -1;
    GLint m_blurTaps = -1;
    GLint m_blurWeights = -1;
    GLint m_blurOffsets = -1;
    GlProgram m_morph;
    GLint m_morphStep = -1;
    GLint m_morphTaps = -1;
    GLint m_morphFrac = -1;
    GLint m_morphDilate = -1;
};

} // namespace kk::render
