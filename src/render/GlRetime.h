// SPDX-License-Identifier: GPL-3.0-or-later
// GlRetime — in-between frames for retimed clips on the GPU: frame blend and motion-compensated
// interpolation (coarse-to-fine symmetric block matching, GLSL 3.3).
#pragma once

#include "render/GlKit.h"

#include <QString>

namespace kk::render {

/// Builds the picture "between" two consecutive source frames A and B at weight @p w
/// (0 = A, 1 = B). Both inputs are premultiplied RGBA targets of the same size (the decoded
/// frames converted to RGB at display orientation).
///
/// * blend(): (1 - w) A + w B, one pass (Frame Blend).
/// * interpolate() (Optical Flow): estimates the motion v of every pixel of the in-between
///   frame directly at time w ("symmetric" matching: A(p - w v) ~ B(p + (1 - w) v), so there
///   are no holes to fill as with forward warping), then samples both frames along it.
///   1. Luma pyramid of A and B: working level 0 at most kFlowMaxWidth px wide (box
///      downsampling), then halved down to ~kFlowMinEdge px (at most kMaxLevels levels).
///   2. Coarsest level first: per pixel, test the prediction (twice the coarser level's motion)
///      and the (2 kSearchRadius + 1)^2 integer offsets around it with a 3x3 sum of absolute
///      luma differences plus a small penalty on the change (prefers smooth fields); then a
///      cost-weighted 3x3 smoothing pass. Each level refines the one before (motion range
///      ~ kSearchRadius * 2^levels px of level 0).
///   3. Level 0: sub-pixel refinement (parabola through the costs at +-1 px).
///   4. Full resolution: A and B sampled along the (bilinearly upsampled) motion and mixed by
///      w; where the two samples disagree (occlusions, failed matches) the result fades to
///      the plain blend, which ghosts instead of tearing.
///   Cost at 1080p on a GTX 1650 class GPU: ~2-4 ms per in-between frame (level 0 = 640 x 360,
///   5 levels); on llvmpipe (software GL) ~0.3-0.6 s, fine for export, slow for playback
///   (the viewer then drops frames; Nearest / Frame Blend preview faster).
///
/// Thread-safety: render thread only (GL context current).
class GlRetime
{
public:
    /// Builds the shader programs. @return false with a reason on failure.
    bool initialize(GlKit& kit, QString* why);
    void destroy(GlKit& kit);

    /// Frame blend of @p a and @p b at weight @p w. New target of a's size.
    [[nodiscard]] PooledTarget blend(
        GlKit& kit, const RenderTarget& a, const RenderTarget& b, float w);
    /// Motion-compensated in-between frame (see the class comment). New target of a's size.
    [[nodiscard]] PooledTarget interpolate(
        GlKit& kit, const RenderTarget& a, const RenderTarget& b, float w);

    static constexpr int kFlowMaxWidth = 640; ///< level 0 width limit (px)
    static constexpr int kFlowMinEdge = 24; ///< stop the pyramid below this edge (px)
    static constexpr int kMaxLevels = 5;
    static constexpr int kSearchRadius = 2; ///< integer offsets tested per level: (2r + 1)^2

private:
    /// Luma of @p src into a new target of @p size (box filtered by the bilinear tap).
    PooledTarget luma(GlKit& kit, const RenderTarget& src, QSize size);

    GlProgram m_blend;
    GLint m_blendW = -1;
    GlProgram m_luma;
    GlProgram m_search;
    GLint m_searchW = -1;
    GLint m_searchTexel = -1;
    GLint m_searchHasPrev = -1;
    GLint m_searchPrevScale = -1;
    GLint m_searchSubpixel = -1;
    GlProgram m_smooth;
    GLint m_smoothTexel = -1;
    GlProgram m_warp;
    GLint m_warpW = -1;
    GLint m_warpScale = -1;
};

} // namespace kk::render
