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
/// Draws the current frame as one textured quad (GLSL 330 core). Frames are uploaded into a
/// single persistent texture with glTexSubImage2D directly from the QImage memory
/// (ARGB32/ARGB32_Premultiplied = BGRA bytes on little-endian, so no CPU conversion and no
/// per-frame allocation when the size stays the same). The letterbox is a scissored clear.
///
/// TODO(playback): accept a GL texture id from the GPU compositor (shared contexts, see main.cpp)
/// to skip the upload entirely.
class GlViewerSurface : public QOpenGLWidget, public ViewerSurface, protected QOpenGLExtraFunctions
{
    Q_OBJECT
public:
    /// Creates an empty (black) surface.
    explicit GlViewerSurface(QWidget* parent = nullptr);
    ~GlViewerSurface() override;

    QWidget* widget() override { return this; }
    void setFrame(const QImage& frame) override;
    void clear(const QString& text = {}) override;
    void setAspect(QSize aspect) override;
    [[nodiscard]] QRect pictureRect() const override;

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void uploadFrame();
    void releaseGl();
    void updateEmptyText();

    ViewerPicture m_picture;
    QImage m_frame; ///< frame waiting for / already in the texture
    bool m_frameDirty = false;
    QLabel* m_emptyText = nullptr; ///< overlay for the empty state (GL draws no text)

    bool m_glReady = false;
    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLuint m_texture = 0;
    QSize m_textureSize;
    GLint m_rectUniform = -1;
};

} // namespace kk
