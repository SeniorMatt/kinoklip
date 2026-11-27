// SPDX-License-Identifier: GPL-3.0-or-later
// GlPluginEffect — see GlPluginEffect.h.
#include "render/GlPluginEffect.h"

#include "plugins/PluginRegistry.h"
#include "render/GlOfxEffect.h"
#include "render/GlEffectBase.h"
#include "render/GlShaderTransition.h"
#include "render/PluginShader.h"

#include <QColor>

#include <algorithm>
#include <cmath>

namespace kk::render {

namespace {

using plugins::ParamKind;
using plugins::PluginParam;
using plugins::PluginShaderSpec;

QColor colorOf(const ParamSet& set, const PluginParam& p)
{
    const auto it = set.statics.constFind(p.key);
    if (it == set.statics.constEnd())
        return p.colorDefault;
    QColor c = it->value<QColor>();
    if (!c.isValid())
        c = QColor::fromString(it->toString());
    return c.isValid() ? c : p.colorDefault;
}

int choiceIndex(const ParamSet& set, const PluginParam& p)
{
    const QString value = set.statics.value(p.key, p.stringDefault).toString();
    int index = int(p.choices.indexOf(value));
    if (index < 0)
        index = int(std::max<qsizetype>(0, p.choices.indexOf(p.stringDefault)));
    return index;
}

/// Writes parameter @p p of @p set at @p frame into uniform @p loc of the current program.
/// Numbers are clamped to the manifest's min..max: a keyframe curve may overshoot, a shader
/// should never see a value its author did not plan for.
void setParam(GlFunctions* gl, GLint loc, const PluginParam& p, const ParamSet& set, int64_t frame)
{
    if (loc < 0)
        return;
    switch (p.type) {
    case ParamKind::Number: {
        const double v = std::clamp(set.value(p.key, frame, p.def), p.min, p.max);
        gl->glUniform1f(loc, float(v));
        break;
    }
    case ParamKind::Integer: {
        const double v = std::clamp(set.value(p.key, frame, p.def), p.min, p.max);
        gl->glUniform1i(loc, int(std::lround(v)));
        break;
    }
    case ParamKind::Boolean:
        gl->glUniform1i(loc, set.statics.value(p.key, p.boolDefault).toBool() ? 1 : 0);
        break;
    case ParamKind::Choice:
        gl->glUniform1i(loc, choiceIndex(set, p));
        break;
    case ParamKind::Color: {
        const QColor c = colorOf(set, p);
        gl->glUniform4f(
            loc, float(c.redF()), float(c.greenF()), float(c.blueF()), float(c.alphaF()));
        break;
    }
    case ParamKind::String:
        break;
    }
}

/// A video effect made of the plugin's fragment shader.
class PluginShaderEffect final : public fx::PassEffect
{
public:
    explicit PluginShaderEffect(std::shared_ptr<const PluginShaderSpec> spec)
        : m_spec(std::move(spec))
    { }

    bool initialize(GlKit& kit, QString* why) override
    {
        const std::string source = pluginshader::composeEffect(*m_spec);
        const std::string name = m_spec->fullId.toStdString();
        // The prelude already carries the common helpers, so build() adds none.
        if (!build(kit, name.c_str(), source.c_str(), why, false)) {
            plugins::PluginRegistry::instance().reportShaderError(m_spec->fullId, *why);
            return false;
        }
        plugins::PluginRegistry::instance().reportShaderError(m_spec->fullId, {});
        m_resolution = uniform(kit, "uResolution");
        m_scale = uniform(kit, "uScale");
        m_time = uniform(kit, "uTime");
        m_frame = uniform(kit, "uFrame");
        m_fps = uniform(kit, "uFps");
        m_content = uniform(kit, "uContent");
        for (const PluginParam& p : m_spec->params)
            m_loc.push_back(uniform(kit, pluginshader::uniformName(p.key).toUtf8().constData()));
        return true;
    }

    double paddingPx(const ParamSet&, int64_t) const override { return m_spec->paddingPx; }

    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        PooledTarget out = begin(kit, *in.layer);
        GlFunctions* gl = kit.gl();
        const fx::LayerGeometry g = fx::layerGeometry(in);
        gl->glUniform2f(m_resolution, g.width, g.height);
        gl->glUniform1f(m_scale, float(in.scale));
        gl->glUniform1f(m_time, float(double(in.localFrame) / in.fps));
        gl->glUniform1f(m_frame, float(in.localFrame));
        gl->glUniform1f(m_fps, float(in.fps));
        gl->glUniform4f(m_content, g.x0, g.y0, g.x1, g.y1);
        for (size_t i = 0; i < m_loc.size(); ++i)
            setParam(gl, m_loc[i], m_spec->params[qsizetype(i)], *in.params, in.localFrame);
        draw(kit, *in.layer);
        return out;
    }

private:
    std::shared_ptr<const PluginShaderSpec> m_spec;
    std::vector<GLint> m_loc;
    GLint m_resolution = -1, m_scale = -1, m_time = -1, m_frame = -1, m_fps = -1, m_content = -1;
};

/// A transition made of the plugin's fragment shader; the plain ShaderTransition runs it.
class PluginShaderTransition final : public GlTransition
{
public:
    explicit PluginShaderTransition(std::shared_ptr<const PluginShaderSpec> spec)
        : m_spec(std::move(spec))
    {
        m_inner = std::make_unique<ShaderTransition>(m_spec->fullId.toStdString(),
            pluginshader::composeTransition(*m_spec),
            [spec = m_spec](GlKit& kit, const GlProgram& program, const TransitionInput& in) {
                GlFunctions* gl = kit.gl();
                gl->glUniform2f(program.uniform(gl, "uResolution"), float(in.size.width()),
                    float(in.size.height()));
                for (const PluginParam& p : spec->params)
                    setParam(gl,
                        program.uniform(gl, pluginshader::uniformName(p.key).toUtf8().constData()),
                        p, *in.params, in.localFrame);
            });
    }
    bool initialize(GlKit& kit, QString* why) override
    {
        const bool ok = m_inner->initialize(kit, why);
        plugins::PluginRegistry::instance().reportShaderError(
            m_spec->fullId, ok ? QString() : *why);
        return ok;
    }
    void destroy(GlKit& kit) override { m_inner->destroy(kit); }
    void draw(GlKit& kit, const TransitionInput& in) override { m_inner->draw(kit, in); }

private:
    std::shared_ptr<const PluginShaderSpec> m_spec;
    std::unique_ptr<ShaderTransition> m_inner;
};

} // namespace

bool isPluginId(const QString& id)
{
    return id.startsWith(QLatin1String(plugins::kIdPrefix)) || isOfxId(id);
}

std::unique_ptr<GlEffect> makePluginEffect(const QString& fullId)
{
    if (isOfxId(fullId))
        return makeOfxEffect(fullId);
    const auto spec = plugins::PluginRegistry::instance().shaderSpec(fullId);
    if (!spec || spec->transition)
        return nullptr;
    return std::make_unique<PluginShaderEffect>(spec);
}

std::unique_ptr<GlTransition> makePluginTransition(const QString& fullId)
{
    if (isOfxId(fullId))
        return makeOfxTransition(fullId);
    const auto spec = plugins::PluginRegistry::instance().shaderSpec(fullId);
    if (!spec || !spec->transition)
        return nullptr;
    return std::make_unique<PluginShaderTransition>(spec);
}

} // namespace kk::render
