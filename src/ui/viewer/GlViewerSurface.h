// SPDX-License-Identifier: GPL-3.0-or-later
// GlViewerSurface — OpenGL (QOpenGLWidget) viewer picture area. PLACEHOLDER drawing.
#pragma once

#include "ui/viewer/ViewerSurface.h"

#include <QOpenGLWidget>

namespace cw {

/// QOpenGLWidget implementation of ViewerSurface.
///
/// Placeholder: clears with GL and draws the CPU frame through Qt's GL paint engine.
/// TODO(playback): upload frames as textures with a shader blit, or show the GPU compositor's
/// output texture directly (shared contexts make that copy-free).
class GlViewerSurface : public QOpenGLWidget, public ViewerSurface
{
    Q_OBJECT
public:
    /// Creates an empty (black) surface.
    explicit GlViewerSurface(QWidget* parent = nullptr);

    QWidget* widget() override { return this; }
    void setFrame(const QImage& frame) override;
    void clear(const QString& text = {}) override;
    void setAspect(QSize aspect) override;
    [[nodiscard]] QRect pictureRect() const override;

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    ViewerPicture m_picture;
};

} // namespace cw
