// SPDX-License-Identifier: GPL-3.0-or-later
// GlGrade — the GPU pass of the Color page grade: evaluates a PreparedGrade (render/GradeEval.h)
// node by node over a premultiplied RGBA16F picture (a clip's layer or the timeline
// composite). The GLSL mirrors core/GradeMath.cpp and render/GradeEval.cpp exactly.
#pragma once

#include "render/GlKit.h"
#include "render/GradeEval.h"

#include <memory>
#include <vector>

namespace kk::render {

/// GPU grade pass. Cost per frame: one fullscreen pass per correcting corrector (plus one
/// key pass for keyed nodes under a Layer mixer), one per mixer input; curve tables, LUTs
/// and polygon points go through small textures (uploaded only when they change). Render
/// thread only (like GlKit).
class GlGrade
{
public:
    /// Compiles the programs and creates the table textures. @return false with a reason.
    bool initialize(GlKit& kit, QString* why);
    void destroy(GlKit& kit);

    /// Applies @p grade to @p input (premultiplied RGBA16F; pixel (x, y) of the target is the
    /// pixel grid the grade was prepared for). Returns a new pooled target of the same size,
    /// or an empty handle when nothing changes.
    [[nodiscard]] PooledTarget apply(
        GlKit& kit, const RenderTarget& input, const PreparedGrade& grade);

private:
    /// Uniform locations of the corrector program.
    struct CorrectorUniforms
    {
        GLint mode = -1, corrects = -1, primaries = -1;
        GLint lift = -1, gamma = -1, gain = -1, offset = -1;
        GLint contrast = -1, pivot = -1, saturation = -1, temperature = -1, tint = -1;
        GLint lumMix = -1, hue = -1, curveMask = -1;
        GLint lutMode = -1, lutN = -1, lutMin = -1, lutMax = -1;
        GLint keyed = -1, mix = -1;
        GLint qual = -1, qualInvert = -1, qualUse = -1, qualHue = -1, qualSat = -1, qualLum = -1;
        GLint winCount = -1, winShape = -1, winInvert = -1, winOpacity = -1, winGeom = -1;
        GLint winRotSoft = -1, winPoints = -1;
    };

    void drawCorrector(GlKit& kit, const PreparedGradeNode& node, GLuint input, int mode);
    void uploadCurves(GlFunctions* gl, const grademath::CurveTables& tables);
    void uploadLut(GlFunctions* gl, const std::shared_ptr<const CubeLut>& lut);
    /// Uploads the polygon points of @p node's windows; fills @p ranges (start, count) per window.
    void uploadPoints(GlFunctions* gl, const PreparedGradeNode& node, std::vector<int>& ranges);

    GlProgram m_corrector;
    CorrectorUniforms m_u;
    GlProgram m_parallel;
    GLint m_parallelCount = -1;
    GlProgram m_layer;
    GLint m_layerConstAlpha = -1, m_layerUseKey = -1, m_layerOpaque = -1;

    GLuint m_curveTex = 0; ///< kCurveTableSize x kGradeCurveCount, R32F
    GLuint m_lut3d = 0;
    GLuint m_lut1d = 0;
    std::shared_ptr<const CubeLut> m_uploadedLut; ///< the table in the textures (kept alive)
    GLuint m_pointsTex = 0; ///< polygon points, RG32F, width = point count
    int m_pointsCapacity = 0;
    std::vector<float> m_points; ///< scratch
};

} // namespace kk::render
