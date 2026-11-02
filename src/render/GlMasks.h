// SPDX-License-Identifier: GPL-3.0-or-later
// GlMasks — the GPU mask pass: turns a clip's masks (core/Mask.h) into an alpha matte and
// multiplies the clip's layer by it, before the layer is transformed and composited.
#pragma once

#include "core/TimelineData.h"
#include "render/GlKit.h"

#include <QPointF>
#include <QSizeF>
#include <QVector>

#include <cstdint>
#include <vector>

namespace kk::render {

/// Where the clip content lies inside the layer target the mask pass works on.
/// Layer pixel = padPx + (content + contentSize / 2) · scale (content origin at its centre).
struct MaskLayerGeometry
{
    QSizeF contentSize; ///< timeline pixels
    double scale = 1.0; ///< layer pixels per timeline pixel
    double padPx = 0.0; ///< transparent margin around the content, layer pixels
};

/// Mask pass of the GPU compositor (see src/render/readme.md, "Masks").
///
/// Approach:
/// * Rectangles and ellipses are analytic: the combine shader evaluates a signed distance
///   (exact for the rotated rectangle, a first-order ellipse estimate) per pixel, shifts it by
///   the expansion and maps it through a smoothstep from −featherIn to +featherOut (at least
///   one pixel wide, which is the antialiasing).
/// * Point paths are flattened on the CPU (adaptive bezier subdivision, 0.25 layer px), offset
///   by expand + (featherOut − featherIn) / 2 (miter joins) and filled into a coverage target
///   with one triangle fan whose blending toggles the destination (even-odd rule, exact 0/1,
///   no stencil buffer). A Gaussian blur (σ = (in + out) / 3.76, which gives the smoothstep's
///   slope at the edge; at least 0.5 px for antialiasing; downscaled for large σ) feathers it.
/// * One fullscreen combine pass evaluates up to kChunk masks (at most kMaxPathsPerChunk
///   coverage textures) in list order — invert, opacity, mode (MaskMode table in core/Mask.h)
///   — and writes layer × matte. More masks chain through intermediate matte targets.
///
/// Cost: analytic masks add one pass over the layer for up to 8 masks; each feathered path
/// mask adds its fan + a separable blur. Thread-safety: render thread only (like GlKit).
class GlMasks
{
public:
    /// Compiles the programs and creates the fan vertex buffer. @return false with a reason.
    bool initialize(GlKit& kit, QString* why);
    void destroy(GlKit& kit);

    /// True if @p clip has at least one mask that renders (isRenderable()). Cheap (no GL).
    [[nodiscard]] static bool hasActiveMasks(const ClipData& clip);

    /// Applies the masks of @p clip at clip-local frame @p localFrame to @p layer (premultiplied
    /// RGBA16F, see MaskLayerGeometry). Returns a new pooled target of the same size, or an
    /// empty handle when the clip has no active mask.
    [[nodiscard]] PooledTarget apply(GlKit& kit, const RenderTarget& layer,
        const MaskLayerGeometry& geometry, const ClipData& clip, int64_t localFrame);

private:
    static constexpr int kChunk = 8; ///< masks per combine pass (uniform array size)
    static constexpr int kMaxPathsPerChunk = 4; ///< coverage samplers per combine pass

    /// Uniform values of one mask in a combine pass.
    struct Item
    {
        int type = 0; ///< 0 rectangle, 1 ellipse, 2 coverage texture
        int mode = 0;
        int invert = 0;
        float opacity = 1.0f;
        float geom[4] = {}; ///< analytic: centre x, y, half width, half height (layer px)
        float rot[2] = {1.0f, 0.0f}; ///< cos, sin of the rotation
        float soft[3] = {}; ///< analytic: feather in, feather out, expand (layer px)
        int coverage = -1; ///< type 2: index into m_coverage
    };

    /// Renders the coverage of path mask @p mask into a new target (feathered).
    PooledTarget pathCoverage(GlKit& kit, QSize size, const MaskLayerGeometry& g,
        const MaskData& mask, int64_t localFrame);

    GlProgram m_combine;
    GLint m_uCount = -1, m_uType = -1, m_uMode = -1, m_uInvert = -1, m_uOpacity = -1;
    GLint m_uGeom = -1, m_uRot = -1, m_uSoft = -1, m_uHasPrev = -1, m_uStart = -1;
    GLint m_uApply = -1, m_uCovIndex = -1;
    GlProgram m_fan;
    GLuint m_fanVao = 0;
    GLuint m_fanVbo = 0;
    GLint m_quadVao = 0; ///< GlKit's VAO, rebound after drawing the fan

    // Per-frame scratch (capacity reused: no allocation in steady state).
    std::vector<const MaskData*> m_masks;
    std::vector<Item> m_items;
    std::vector<PooledTarget> m_coverage;
    MaskPath m_path;
    QVector<QPointF> m_polygon;
    std::vector<float> m_vertices;
};

} // namespace kk::render
