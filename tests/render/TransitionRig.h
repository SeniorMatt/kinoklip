// SPDX-License-Identifier: GPL-3.0-or-later
// TransitionRig — draws one GPU transition directly (no timeline, no compositor): two pictures go
// in as textures, one progress value, the result comes out as a QImage. Used by the endpoint test
// (test_transitions.cpp) and by the contact sheet tool (tests/tools/kk_transitionsheets.cpp).
// Needs a QGuiApplication on a platform with OpenGL 3.3 (xcb under xvfb), like the GL tests.
#pragma once

#include "core/EffectRegistry.h"
#include "render/GlKit.h"
#include "render/GlTransitions.h"

#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>

#include <map>
#include <memory>

namespace kk::test {

class TransitionRig
{
public:
    TransitionRig() = default;
    TransitionRig(const TransitionRig&) = delete;
    TransitionRig& operator=(const TransitionRig&) = delete;
    ~TransitionRig() { destroy(); }

    /// Creates the context and the toolkit. @return false (with @p why) without OpenGL 3.3.
    bool initialize(QString* why)
    {
        m_surface.create();
        m_context = std::make_unique<QOpenGLContext>();
        if (!m_context->create() || !m_context->makeCurrent(&m_surface)) {
            *why = QStringLiteral("no OpenGL context");
            return false;
        }
        if (m_context->isOpenGLES() || m_context->format().version() < qMakePair(3, 3)) {
            *why = QStringLiteral("OpenGL 3.3 core is not available");
            return false;
        }
        m_gl = QOpenGLVersionFunctionsFactory::get<render::GlFunctions>(m_context.get());
        if (!m_gl || !m_gl->initializeOpenGLFunctions()) {
            *why = QStringLiteral("OpenGL 3.3 functions are not available");
            return false;
        }
        m_kit = std::make_unique<render::GlKit>(m_gl);
        return m_kit->initialize(why);
    }

    /// Draws transition @p id with @p params at @p progress (0 = all @p a, 1 = all @p b) at the
    /// size of @p a; @p localFrame is the frame inside the transition (animated parameters).
    /// A null image when the transition has no GPU implementation or its program failed.
    QImage draw(const QString& id, const ParamSet& params, double progress, const QImage& a,
        const QImage& b, int64_t localFrame = 0, QString* why = nullptr)
    {
        render::GlTransition* transition = transitionFor(id, why);
        if (!transition)
            return {};
        const QSize size = a.size();
        const GLuint ta = upload(a);
        const GLuint tb = upload(b);
        render::PooledTarget target = m_kit->acquire(size, GL_RGBA8);
        m_kit->bindTarget(*target);
        m_kit->clearTransparent();
        m_kit->setBlendOver(true);
        render::TransitionInput in;
        in.a = ta;
        in.b = tb;
        in.progress = progress;
        in.params = &params;
        in.localFrame = localFrame;
        in.size = size;
        in.target = target.get();
        transition->draw(*m_kit, in);
        m_gl->glUseProgram(0);

        QImage out(size, QImage::Format_RGBA8888_Premultiplied);
        m_gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, target->fbo);
        m_gl->glReadPixels(
            0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_BYTE, out.bits());
        m_gl->glDeleteTextures(1, &ta);
        m_gl->glDeleteTextures(1, &tb);
        return out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    void destroy()
    {
        if (!m_context)
            return;
        if (m_context->makeCurrent(&m_surface)) {
            for (auto& [id, transition] : m_built)
                transition->destroy(*m_kit);
            m_built.clear();
            m_kit->destroy();
        }
        m_kit.reset();
        m_context.reset();
    }

private:
    render::GlTransition* transitionFor(const QString& id, QString* why)
    {
        if (const auto it = m_built.find(id); it != m_built.end())
            return it->second.get();
        const render::GlTransitionFactory* factory = render::findGlTransitionFactory(id);
        if (!factory)
            return nullptr;
        std::unique_ptr<render::GlTransition> transition = factory->create();
        QString log;
        if (!transition->initialize(*m_kit, &log)) {
            if (why)
                *why = log;
            return nullptr;
        }
        return m_built.emplace(id, std::move(transition)).first->second.get();
    }

    GLuint upload(const QImage& image)
    {
        const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        GLuint texture = 0;
        m_gl->glGenTextures(1, &texture);
        m_gl->glBindTexture(GL_TEXTURE_2D, texture);
        m_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rgba.width(), rgba.height(), 0, GL_RGBA,
            GL_UNSIGNED_BYTE, rgba.constBits());
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        m_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return texture;
    }

    QOffscreenSurface m_surface;
    std::unique_ptr<QOpenGLContext> m_context;
    render::GlFunctions* m_gl = nullptr;
    std::unique_ptr<render::GlKit> m_kit;
    std::map<QString, std::unique_ptr<render::GlTransition>> m_built;
};

/// Two distinct opaque test pictures of @p size: A has colour bars over a ramp and a ring; B has a
/// diagonal gradient, a checker board and a bright disc. Each has a letter-free layout so that
/// upside-down or mirrored output shows (A: bright at the top left, B: bright at the bottom
/// right).
inline QImage testPicture(bool second, QSize size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const double u = double(x) / size.width();
            const double v = double(y) / size.height();
            int r, g, b;
            if (!second) {
                static const int bars[7][3] = {{235, 235, 235}, {235, 235, 30}, {30, 235, 235},
                    {30, 235, 30}, {235, 30, 235}, {235, 30, 30}, {30, 30, 235}};
                const int i = std::min(6, int(u * 7));
                const double ramp = 1.0 - 0.6 * (u * 0.5 + v * 0.5); // brighter top left
                r = int(bars[i][0] * ramp);
                g = int(bars[i][1] * ramp);
                b = int(bars[i][2] * ramp);
                const double d = std::hypot(u - 0.5, (v - 0.5) * size.height() / size.width());
                if (std::abs(d - 0.2) < 0.012)
                    r = g = b = 250;
            } else {
                const double gx = 0.15 + 0.7 * (u * 0.5 + v * 0.5); // brighter bottom right
                const bool checker = ((x / 12) + (y / 12)) % 2 == 0;
                r = int(255 * gx * (checker ? 1.0 : 0.55));
                g = int(255 * (1.0 - gx) * 0.8 * (checker ? 1.0 : 0.55));
                b = int(255 * (0.3 + 0.4 * u));
                if (std::hypot(u - 0.7, (v - 0.35) * size.height() / size.width()) < 0.12)
                    r = g = b = 255;
            }
            row[x] = qPremultiply(qRgba(r, g, b, 255));
        }
    }
    return img;
}

} // namespace kk::test
