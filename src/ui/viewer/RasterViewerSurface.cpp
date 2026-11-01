// SPDX-License-Identifier: GPL-3.0-or-later
// Raster fallback viewer surface.
#include "ui/viewer/RasterViewerSurface.h"

#include <QPainter>

namespace cw {

RasterViewerSurface::RasterViewerSurface(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ViewerSurface"));
    setAttribute(Qt::WA_OpaquePaintEvent); // we paint every pixel; skip background erase
    setMinimumSize(160, 90);
}

void RasterViewerSurface::setFrame(const QImage& frame)
{
    m_picture.setFrame(frame);
    update();
}

void RasterViewerSurface::clear(const QString& text)
{
    m_picture.clear(text);
    update();
}

void RasterViewerSurface::setAspect(QSize aspect)
{
    if (m_picture.setAspect(aspect))
        update();
}

QRect RasterViewerSurface::pictureRect() const
{
    return m_picture.pictureRect(size());
}

void RasterViewerSurface::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    m_picture.paint(painter, size(), true);
}

} // namespace cw
