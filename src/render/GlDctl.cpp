// SPDX-License-Identifier: GPL-3.0-or-later
// GlDctl — the DCTL effect and the DCTL transition (spec 205): the .dctl file is translated to
// GLSL by core/DctlTranslator.cpp, compiled when the file (or its modification time) changes,
// and the parameters of the effect feed the DEFINE_UI_PARAMS of the file. A file that cannot
// be translated or compiled leaves the picture as it is and writes the reason to the log once.
#include "core/DctlTranslator.h"
#include "core/VideoEffectKeys.h"
#include "render/GlEffectBase.h"
#include "render/GlTransitions.h"

#include <QDateTime>
#include <QFileInfo>

#include <cmath>

namespace kk::render {

namespace {

using namespace fx;
namespace k = fxkeys;

/// What the file's DEFINE_UI_PARAMS ask for, from the effect's parameter slots: every slot is the
/// change from the default set in the file.
double uiValue(const dctl::UiParam& u, const ParamSet& p, int64_t frame)
{
    const double slot = p.value(k::dctlParam(u.slot + 1), frame, 0.0);
    switch (u.kind) {
    case dctl::UiKind::CheckBox:
        return (slot >= 0.5) != (u.def >= 0.5) ? 1.0 : 0.0;
    case dctl::UiKind::ComboBox: {
        const int n = std::max<int>(1, int(u.options.size()));
        const int idx = (int(std::lround(u.def)) + int(std::lround(slot))) % n;
        return idx < 0 ? idx + n : idx;
    }
    case dctl::UiKind::SliderInt:
        return std::clamp(std::round(u.def + slot), u.min, u.max);
    case dctl::UiKind::ValueBox:
        return u.def + slot;
    case dctl::UiKind::ColorPicker:
        return 0.0;
    case dctl::UiKind::SliderFloat:
        break;
    }
    return std::clamp(u.def + slot, u.min, u.max);
}

void setUiUniforms(GlFunctions* gl, const dctl::Result& r, const QVector<GLint>& loc,
    const ParamSet& p, int64_t frame)
{
    for (int i = 0; i < r.params.size(); ++i) {
        const dctl::UiParam& u = r.params[i];
        if (loc[i] < 0)
            continue;
        switch (u.kind) {
        case dctl::UiKind::ColorPicker: {
            const fxmath::Rgb c = fxmath::colorParam(p, k::dctlColor(u.slot + 1), {1.f, 1.f, 1.f});
            gl->glUniform3f(loc[i], c.r, c.g, c.b);
            break;
        }
        case dctl::UiKind::SliderInt:
        case dctl::UiKind::ComboBox:
        case dctl::UiKind::CheckBox:
            gl->glUniform1i(loc[i], int(std::lround(uiValue(u, p, frame))));
            break;
        default:
            gl->glUniform1f(loc[i], float(uiValue(u, p, frame)));
        }
    }
}

/// The translated file of one effect instance: re-read when its path or time changes.
struct Loaded
{
    QString path;
    qint64 stamp = -1;
    dctl::Result result;
    bool compiled = false;
    bool reported = false;

    bool stale(const QString& now) const
    {
        const QFileInfo fi(now);
        return now != path || fi.lastModified().toMSecsSinceEpoch() != stamp;
    }
    void reload(const QString& now, dctl::Entry entry)
    {
        path = now;
        stamp = QFileInfo(now).lastModified().toMSecsSinceEpoch();
        result = dctl::translateFile(now, entry);
        compiled = false;
        reported = false;
    }
    void report(const QString& what)
    {
        if (reported)
            return;
        reported = true;
        qWarning("DCTL %s: %s", qPrintable(path), qPrintable(what));
    }
};

class DctlEffect final : public PassEffect
{
public:
    bool initialize(GlKit&, QString*) override { return true; } // built when a file is chosen

    bool isIdentity(const ParamSet& p, int64_t) const override
    {
        return p.staticValue(k::DctlFile).toString().isEmpty();
    }

    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        const QString path = in.params->staticValue(k::DctlFile).toString();
        if (path.isEmpty())
            return {};
        if (m_file.stale(path)) {
            m_program.destroy(kit.gl());
            m_file.reload(path, dctl::Entry::Transform);
        }
        if (!m_file.result.ok) {
            m_file.report(m_file.result.error);
            return {};
        }
        if (!m_file.compiled) {
            QString why;
            m_file.compiled = true;
            m_ready = build(kit, "dctl", m_file.result.glsl.toUtf8().constData(), &why, true);
            if (!m_ready) {
                m_file.report(QStringLiteral("the translated file does not compile: ") + why);
            } else {
                m_loc.clear();
                for (const dctl::UiParam& u : m_file.result.params)
                    m_loc.push_back(uniform(kit, u.uniform.toUtf8().constData()));
            }
        }
        if (!m_ready)
            return {};
        PooledTarget out = begin(kit, *in.layer);
        setUiUniforms(kit.gl(), m_file.result, m_loc, *in.params, in.localFrame);
        draw(kit, *in.layer);
        return out;
    }

private:
    Loaded m_file;
    bool m_ready = false;
    QVector<GLint> m_loc;
};

class DctlTransition final : public GlTransition
{
public:
    bool initialize(GlKit&, QString*) override { return true; }
    void destroy(GlKit& kit) override { m_program.destroy(kit.gl()); }
    void draw(GlKit& kit, const TransitionInput& in) override
    {
        GlFunctions* gl = kit.gl();
        const QString path = in.params->staticValue(k::DctlFile).toString();
        if (m_file.stale(path)) {
            m_program.destroy(gl);
            m_file.reload(path, dctl::Entry::Transition);
        }
        if (!m_file.result.ok) {
            m_file.report(m_file.result.error);
            drawPlain(kit, in);
            return;
        }
        if (!m_file.compiled) {
            QString why;
            m_file.compiled = true;
            m_ready = m_program.build(gl, "dctl_transition",
                m_file.result.glsl.toUtf8().constData(), &why);
            if (!m_ready) {
                m_file.report(QStringLiteral("the translated file does not compile: ") + why);
            } else {
                gl->glUseProgram(m_program.id);
                gl->glUniform1i(m_program.uniform(gl, "uA"), 0);
                gl->glUniform1i(m_program.uniform(gl, "uB"), 1);
                gl->glUseProgram(0);
                m_loc.clear();
                for (const dctl::UiParam& u : m_file.result.params)
                    m_loc.push_back(m_program.uniform(gl, u.uniform.toUtf8().constData()));
            }
        }
        if (!m_ready) {
            drawPlain(kit, in);
            return;
        }
        gl->glUseProgram(m_program.id);
        kit.setFullscreen(m_program);
        gl->glUniform1f(m_program.uniform(gl, "uProgress"), float(in.progress));
        setUiUniforms(gl, m_file.result, m_loc, *in.params, in.localFrame);
        kit.bindTexture(0, in.a);
        kit.bindTexture(1, in.b);
        kit.drawQuad();
    }

private:
    /// Without a usable file: a cross dissolve, so the cut still works.
    void drawPlain(GlKit& kit, const TransitionInput& in)
    {
        GlFunctions* gl = kit.gl();
        if (!m_plain.id) {
            QString why;
            if (!m_plain.build(gl, "dctl_fallback", R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform float uProgress;
void main() { fragColor = mix(texture(uA, vUv), texture(uB, vUv), uProgress); }
)",
                    &why))
                return;
            gl->glUseProgram(m_plain.id);
            gl->glUniform1i(m_plain.uniform(gl, "uA"), 0);
            gl->glUniform1i(m_plain.uniform(gl, "uB"), 1);
            gl->glUseProgram(0);
        }
        gl->glUseProgram(m_plain.id);
        kit.setFullscreen(m_plain);
        gl->glUniform1f(m_plain.uniform(gl, "uProgress"), float(in.progress));
        kit.bindTexture(0, in.a);
        kit.bindTexture(1, in.b);
        kit.drawQuad();
    }

    Loaded m_file;
    GlProgram m_program, m_plain;
    bool m_ready = false;
    QVector<GLint> m_loc;
};

const GlEffectFactory kTable[] = {
    {fxid::Dctl, &make<DctlEffect>},
};

} // namespace

std::span<const GlEffectFactory> dctlGlEffects()
{
    return kTable;
}

void appendDctlTransition(std::vector<GlTransitionFactory>& out)
{
    out.push_back({QString::fromLatin1(fxid::DctlTransition),
        [] { return std::unique_ptr<GlTransition>(std::make_unique<DctlTransition>()); }});
}

} // namespace kk::render
