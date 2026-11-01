// SPDX-License-Identifier: GPL-3.0-or-later
// PLACEHOLDER OpenGL viewer surface.
#include "ui/viewer/GlViewerSurface.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPainter>

namespace cw {

GlViewerSurface::GlViewerSurface(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setObjectName(QStringLiteral("ViewerSurface"));
    setMinimumSize(160, 90);
}

void GlViewerSurface::setFrame(const QImage& frame)
{
    m_picture.setFrame(frame);
    update();
}

void GlViewerSurface::clear(const QString& text)
{
    m_picture.clear(text);
    update();
}

void GlViewerSurface::setAspect(QSize aspect)
{
    if (m_picture.setAspect(aspect))
        update();
}

QRect GlViewerSurface::pictureRect() const
{
    return m_picture.pictureRect(size());
}

void GlViewerSurface::initializeGL()
{
    // The real implementation creates its texture + shader program here.
}

void GlViewerSurface::paintGL()
{
    QOpenGLFunctions* gl = context()->functions();
    constexpr float kSurround = 0x0b / 255.0f;
    gl->glClearColor(kSurround, kSurround, kSurround, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    QPainter painter(this);
    m_picture.paint(painter, size(), false);
}

} // namespace cw
