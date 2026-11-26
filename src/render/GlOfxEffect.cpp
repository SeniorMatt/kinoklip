// SPDX-License-Identifier: GPL-3.0-or-later
// GlOfxEffect — see GlOfxEffect.h.
#include "render/GlOfxEffect.h"

#include "plugins/OfxRegistry.h"

#include <QLoggingCategory>

#include <algorithm>
#include <cmath>
#include <vector>

Q_LOGGING_CATEGORY(lcOfxRender, "kk.ofx.render")

namespace kk::render {

namespace {

class OfxGlEffect final : public GlEffect
{
public:
    explicit OfxGlEffect(std::shared_ptr<const plugins::OfxPluginEntry> entry)
        : m_entry(std::move(entry))
    { }

    bool initialize(GlKit&, QString* why) override
    {
        plugins::OfxRegistry& registry = plugins::OfxRegistry::instance();
        // A crash while the plugin loads or renders its first frame puts its bundle on the
        // bad-plugin list at the next start.
        registry.markLoading(m_entry->bundlePath);
        QString error;
        m_instance = ofx::Instance::create(m_entry->bundleInfo(), m_entry->info.index, &error);
        if (!m_instance) {
            registry.markLoaded(m_entry->bundlePath);
            *why = error;
            registry.reportRenderError(m_entry->fullId, error);
            return false;
        }
        return true;
    }

    void destroy(GlKit&) override { m_instance.reset(); }

    PooledTarget apply(GlKit& kit, const EffectInput& in) override
    {
        if (!m_instance || m_failedFrames > kGiveUpAfter)
            return {};
        GlFunctions* gl = kit.gl();
        const RenderTarget& layer = *in.layer;
        const int w = layer.size.width(), h = layer.size.height();
        const size_t floats = size_t(w) * size_t(h) * 4;
        m_src.resize(floats);
        m_dst.resize(floats);
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, layer.fbo);
        gl->glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, m_src.data());
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

        const double scale = in.scale > 0 ? in.scale : 1.0;
        ofx::RenderArgs args;
        args.time = double(in.localFrame);
        args.fps = in.fps;
        args.width = w;
        args.height = h;
        args.scale = scale;
        args.originX = -in.paddingPx / scale;
        args.originY = -in.paddingPx / scale;
        args.projectWidth = (w - 2 * in.paddingPx) / scale;
        args.projectHeight = (h - 2 * in.paddingPx) / scale;
        args.interactive = !in.waitForAi;
        const ParamSet& set = *in.params;
        const double pw = args.projectWidth, ph = args.projectHeight;
        bind(set, args.time, pw, ph);
        QString error;
        const ofx::RenderResult result = m_instance->render(args, m_src.data(), m_dst.data(), &error);
        m_instance->setEvaluator({});
        plugins::OfxRegistry& registry = plugins::OfxRegistry::instance();
        if (result == ofx::RenderResult::Failed) {
            ++m_failedFrames;
            qCWarning(lcOfxRender).noquote() << m_entry->fullId << error;
            registry.reportRenderError(m_entry->fullId, error);
            return {};
        }
        if (!m_loadedMarked) {
            m_loadedMarked = true;
            registry.markLoaded(m_entry->bundlePath);
        }
        m_failedFrames = 0;
        if (result == ofx::RenderResult::Identity)
            return {};
        PooledTarget out = kit.acquire(layer.size, layer.format);
        kit.bindTexture(0, out->texture);
        gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_FLOAT, m_dst.data());
        kit.bindTexture(0, 0);
        return out;
    }

    /// Effects that grow the picture (blurs, glows) are not cut at the clip's edge: the plugin's
    /// region of definition for a source of the clip's size says how far it reaches beyond it,
    /// and the renderer gives the layer a transparent margin of that size.
    double contentPaddingPx(const ParamSet& set, int64_t localFrame, QSizeF content) const override
    {
        if (!m_instance || content.width() < 1 || content.height() < 1)
            return 0.0;
        const double time = double(localFrame);
        bind(set, time, content.width(), content.height());
        double rod[4] = {};
        const bool known
            = m_instance->regionOfDefinition(time, 25.0, content.width(), content.height(), rod);
        m_instance->setEvaluator({}); // it points at @p set
        if (!known)
            return 0.0;
        const double pad = std::max({0.0, -rod[0], -rod[1], rod[2] - content.width(),
            rod[3] - content.height()});
        return std::min(std::ceil(pad), kMaxPaddingPx);
    }
    double paddingPx(const ParamSet&, int64_t) const override { return 0.0; }

private:
    /// Gives the instance the parameter values at @p time and the way to read them at other times.
    void bind(const ParamSet& set, double time, double pw, double ph) const
    {
        const auto& maps = m_entry->params;
        for (const plugins::OfxParamMap& map : maps)
            m_instance->setValue(map.info.name, plugins::OfxRegistry::valueFor(map, set, time, pw, ph));
        m_instance->setEvaluator([&set, &maps, pw, ph](const QString& name, double t, ofx::ParamValue* out) {
            for (const plugins::OfxParamMap& map : maps) {
                if (map.info.name == name) {
                    *out = plugins::OfxRegistry::valueFor(map, set, t, pw, ph);
                    return true;
                }
            }
            return false;
        });
    }

    static constexpr double kMaxPaddingPx = 1000.0;
    static constexpr int kGiveUpAfter = 30; ///< consecutive failed frames before the effect is bypassed
    std::shared_ptr<const plugins::OfxPluginEntry> m_entry;
    std::unique_ptr<ofx::Instance> m_instance;
    std::vector<float> m_src, m_dst; ///< kept between frames
    bool m_loadedMarked = false;
    int m_failedFrames = 0;
};

/// A transition of the OpenFX Transition context: SourceFrom and SourceTo are the two pictures
/// read back from the GPU, the plugin's "Transition" parameter is the progress.
class OfxGlTransition final : public GlTransition
{
public:
    explicit OfxGlTransition(std::shared_ptr<const plugins::OfxPluginEntry> entry)
        : m_entry(std::move(entry))
    { }

    bool initialize(GlKit&, QString* why) override
    {
        plugins::OfxRegistry& registry = plugins::OfxRegistry::instance();
        registry.markLoading(m_entry->bundlePath);
        QString error;
        m_instance = ofx::Instance::create(m_entry->bundleInfo(), m_entry->info.index, &error);
        if (!m_instance) {
            registry.markLoaded(m_entry->bundlePath);
            *why = error;
            registry.reportRenderError(m_entry->fullId, error);
            return false;
        }
        return true;
    }

    void destroy(GlKit&) override { m_instance.reset(); }

    void draw(GlKit& kit, const TransitionInput& in) override
    {
        GlFunctions* gl = kit.gl();
        const int w = in.size.width(), h = in.size.height();
        const size_t floats = size_t(w) * size_t(h) * 4;
        const QTransform whole = GlKit::quadToNdc(QRectF(QPointF(0, 0), QSizeF(in.size)), in.size);
        const auto bail = [&](GLuint fallback) {
            kit.bindTarget(*in.target);
            kit.blit(fallback, whole);
        };
        if (!m_instance || m_failedFrames > kGiveUpAfter) {
            bail(in.progress < 0.5 ? in.a : in.b); // a cut, like a missing plugin
            return;
        }
        m_from.assign(floats, 0.0f);
        m_to.assign(floats, 0.0f);
        m_dst.resize(floats);
        GLuint fbo = 0;
        gl->glGenFramebuffers(1, &fbo);
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        const auto read = [&](GLuint texture, std::vector<float>& into) {
            if (texture == kit.transparentTexture())
                return; // a missing side stays transparent
            gl->glFramebufferTexture2D(
                GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            gl->glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, into.data());
        };
        read(in.a, m_from);
        read(in.b, m_to);
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        gl->glDeleteFramebuffers(1, &fbo);

        ofx::RenderArgs args;
        args.time = double(in.localFrame);
        args.fps = 25.0;
        args.width = w;
        args.height = h;
        args.projectWidth = w;
        args.projectHeight = h;
        const ParamSet& set = *in.params;
        const auto& maps = m_entry->params;
        for (const plugins::OfxParamMap& map : maps)
            m_instance->setValue(map.info.name, plugins::OfxRegistry::valueFor(map, set, args.time, w, h));
        ofx::ParamValue progress;
        progress.v = {std::clamp(in.progress, 0.0, 1.0)};
        m_instance->setValue(QStringLiteral("Transition"), progress);
        m_instance->setEvaluator([&](const QString& name, double time, ofx::ParamValue* out) {
            if (name == QLatin1String("Transition")) {
                *out = progress;
                return true;
            }
            for (const plugins::OfxParamMap& map : maps) {
                if (map.info.name == name) {
                    *out = plugins::OfxRegistry::valueFor(map, set, time, w, h);
                    return true;
                }
            }
            return false;
        });
        QString error;
        const ofx::RenderResult result
            = m_instance->render(args, m_from.data(), m_dst.data(), &error, m_to.data());
        m_instance->setEvaluator({});
        plugins::OfxRegistry& registry = plugins::OfxRegistry::instance();
        if (result != ofx::RenderResult::Rendered) {
            ++m_failedFrames;
            if (result == ofx::RenderResult::Failed) {
                qCWarning(lcOfxRender).noquote() << m_entry->fullId << error;
                registry.reportRenderError(m_entry->fullId, error);
            }
            bail(in.progress < 0.5 ? in.a : in.b);
            return;
        }
        if (!m_loadedMarked) {
            m_loadedMarked = true;
            registry.markLoaded(m_entry->bundlePath);
        }
        m_failedFrames = 0;
        PooledTarget out = kit.acquire(in.size, GL_RGBA16F);
        kit.bindTexture(0, out->texture);
        gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_FLOAT, m_dst.data());
        kit.bindTexture(0, 0);
        kit.bindTarget(*in.target);
        kit.blit(out->texture, whole);
    }

private:
    static constexpr int kGiveUpAfter = 30;
    std::shared_ptr<const plugins::OfxPluginEntry> m_entry;
    std::unique_ptr<ofx::Instance> m_instance;
    std::vector<float> m_from, m_to, m_dst;
    bool m_loadedMarked = false;
    int m_failedFrames = 0;
};

} // namespace

std::unique_ptr<GlTransition> makeOfxTransition(const QString& fullId)
{
    const auto entry = plugins::OfxRegistry::instance().plugin(fullId);
    if (!entry || !entry->info.isTransition())
        return nullptr;
    return std::make_unique<OfxGlTransition>(entry);
}

bool isOfxId(const QString& id)
{
    return id.startsWith(QLatin1String(plugins::kOfxIdPrefix));
}

std::unique_ptr<GlEffect> makeOfxEffect(const QString& fullId)
{
    const auto entry = plugins::OfxRegistry::instance().plugin(fullId);
    if (!entry)
        return nullptr;
    return std::make_unique<OfxGlEffect>(entry);
}

} // namespace kk::render
