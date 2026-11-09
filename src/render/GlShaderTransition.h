// SPDX-License-Identifier: GPL-3.0-or-later
// GlShaderTransition — the class that runs a transition made of one fragment shader, and the
// small helpers the families use to read transition parameters and set uniforms.
#pragma once

#include "render/GlTransitions.h"

#include <QColor>

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

namespace kk::render {

/// A transition whose whole behaviour is one fragment shader with uniforms uA, uB, uProgress
/// plus extra uniforms set by `setExtra`.
class ShaderTransition : public GlTransition
{
public:
    using SetExtra
        = std::function<void(GlKit& kit, const GlProgram& program, const TransitionInput& in)>;

    ShaderTransition(std::string name, std::string fragment, SetExtra setExtra)
        : m_name(std::move(name))
        , m_fragment(std::move(fragment))
        , m_setExtra(std::move(setExtra))
    { }

    bool initialize(GlKit& kit, QString* why) override
    {
        if (!m_program.build(kit.gl(), m_name.c_str(), m_fragment.c_str(), why))
            return false;
        GlFunctions* gl = kit.gl();
        gl->glUseProgram(m_program.id);
        gl->glUniform1i(m_program.uniform(gl, "uA"), 0);
        gl->glUniform1i(m_program.uniform(gl, "uB"), 1);
        m_progress = m_program.uniform(gl, "uProgress");
        gl->glUseProgram(0);
        return true;
    }
    void destroy(GlKit& kit) override { m_program.destroy(kit.gl()); }
    void draw(GlKit& kit, const TransitionInput& in) override
    {
        GlFunctions* gl = kit.gl();
        gl->glUseProgram(m_program.id);
        kit.setFullscreen(m_program);
        gl->glUniform1f(m_progress, float(in.progress));
        if (m_setExtra)
            m_setExtra(kit, m_program, in);
        kit.bindTexture(0, in.a);
        kit.bindTexture(1, in.b);
        kit.drawQuad();
    }

private:
    std::string m_name;
    std::string m_fragment;
    SetExtra m_setExtra;
    GlProgram m_program;
    GLint m_progress = -1;
};

// ---- Reading parameters (a missing one reads as its default) ----

inline QString staticChoice(const ParamSet& p, const QString& key, const QString& def = {})
{
    const auto it = p.statics.constFind(key);
    return it == p.statics.constEnd() ? def : it->toString();
}

inline bool staticBool(const ParamSet& p, const QString& key, bool def)
{
    const auto it = p.statics.constFind(key);
    return it == p.statics.constEnd() ? def : it->toBool();
}

inline QColor staticColor(const ParamSet& p, const QString& key, const QColor& def)
{
    const auto it = p.statics.constFind(key);
    return it == p.statics.constEnd() ? def : it->value<QColor>();
}

inline double numberParam(const TransitionInput& in, const QString& key, double def)
{
    return in.params->value(key, in.localFrame, def);
}

// ---- Setting uniforms of the current program ----

inline void setFloat(GlKit& kit, const GlProgram& program, const char* name, double v)
{
    kit.gl()->glUniform1f(program.uniform(kit.gl(), name), float(v));
}
inline void setInt(GlKit& kit, const GlProgram& program, const char* name, int v)
{
    kit.gl()->glUniform1i(program.uniform(kit.gl(), name), v);
}
inline void setVec2(GlKit& kit, const GlProgram& program, const char* name, double x, double y)
{
    kit.gl()->glUniform2f(program.uniform(kit.gl(), name), float(x), float(y));
}
/// Premultiplied colour.
inline void setColor(GlKit& kit, const GlProgram& program, const char* name, const QColor& c)
{
    const float a = float(c.alphaF());
    kit.gl()->glUniform4f(program.uniform(kit.gl(), name), float(c.redF()) * a,
        float(c.greenF()) * a, float(c.blueF()) * a, a);
}

} // namespace kk::render
