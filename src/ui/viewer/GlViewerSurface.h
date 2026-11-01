// SPDX-License-Identifier: GPL-3.0-or-later
// GlViewerSurface — OpenGL 3.3 core (QOpenGLWidget) viewer picture area.
#pragma once

#include "ui/viewer/ViewerSurface.h"

#include <QOpenGLExtraFunctions>
#include <QOpenGLWidget>

class QLabel;

namespace kk {

/// QOpenGLWidget implementation of ViewerSurface.
///
/// Two ways to get pixels on screen, both one textured quad (GLSL 330 core):
/// * **Zero copy** (setRenderedFrame() with a texture): the GPU compositor's output texture is
///   sampled directly — the widget's context shares objects with the render thread's
///   (Qt::AA_ShareOpenGLContexts). Before sampling, `glWaitSync(fence)` makes *this* context's
///   command stream wait on the GPU for the render to finish (no CPU stall). Filtering comes
///   from a sampler object, so the shared texture's own state is never modified from here.
///   The frame (and so its pooled texture) is kept alive until the paint that draws the *next*
///   frame has been submitted.
/// * **Upload** (setFrame(), or a RenderedFrame with only an image): a persistent texture
///   updated with glTexSubImage2D straight from the QImage memory (ARGB32 = BGRA bytes on
///   little-endian: no conversion, no per-frame allocation at a constant size).
///
/// The letterbox is a scissored clear. Text (empty state, overlay) is a transparent QLabel on
/// top, because GL draws no text.
class GlViewerSurface : public QOpenGLWidget, public ViewerSurface, protected QOpenGLExtraFunctions
{
    Q_OBJECT
public:
    /// Creates an empty (black) surface.
    explicit GlViewerSurface(QWidget* parent = nullptr);
    ~GlViewerSurface() override;

    QWidget* widget() override { return this; }
    void setFrame(const QImage& frame) override;
    void setRenderedFrame(const RenderedFramePtr& frame) override;
    /// True when contexts are shared application-wide (Qt::AA_ShareOpenGLContexts, set in
    /// main()); otherwise the compositor's textures are not visible here and images are used.
    [[nodiscard]] bool canDrawTextures() const override;
    void clear(const QString& text = {}) override;
    void setAspect(QSize aspect) override;
    void setZoom(double zoom) override;
    [[nodiscard]] QRect pictureRect() const override;
    void setOverlayText(const QString& text) override;

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void uploadFrame();
    void releaseGl();
    void updateText();

    ViewerPicture m_picture;
    QImage m_frame; ///< image waiting for / already in m_texture
    bool m_frameDirty = false;
    RenderedFramePtr m_pending; ///< compositor frame to draw (texture path)
    RenderedFramePtr m_drawn; ///< frame drawn by the previous paint (kept alive, see class doc)
    QLabel* m_text = nullptr; ///< overlay for empty state / overlay text

    bool m_glReady = false;
    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLuint m_texture = 0;
    GLuint m_sampler = 0;
    QSize m_textureSize;
    GLint m_rectUniform = -1;
};

} // namespace kk
