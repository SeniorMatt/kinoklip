// SPDX-License-Identifier: GPL-3.0-or-later
// GlGenerators — one GLSL pass that draws the per-pixel part of every built-in generator
// (gradients, colour bars, grey scale, noise, countdown dial) into the bound target.
#pragma once

#include "render/Generators.h"
#include "render/GlKit.h"

namespace kk::render {

/// GPU implementation of the generator patterns (render thread only, like GlKit).
class GlGenerators
{
public:
    /// Compiles the program. False with the log in @p why.
    bool initialize(GlKit& kit, QString* why);
    void destroy(GlKit& kit);
    /// Fills the bound target (no blending) with the pattern of @p g for a frame of
    /// @p frameSize timeline pixels covering the whole target. Kinds without a pattern clear it.
    void drawPattern(GlKit& kit, const GeneratorFrame& g, QSizeF frameSize);

private:
    GlProgram m_program;
    GLint m_mode = -1;
    GLint m_frame = -1;
    GLint m_colors = -1;
    GLint m_center = -1;
    GLint m_dir = -1;
    GLint m_length = -1;
    GLint m_radius = -1;
    GLint m_full = -1;
    GLint m_steps = -1;
    GLint m_vertical = -1;
    GLint m_mono = -1;
    GLint m_noise = -1;
    GLint m_seed = -1;
    GLint m_sweep = -1;
    GLint m_dial = -1;
};

} // namespace kk::render
