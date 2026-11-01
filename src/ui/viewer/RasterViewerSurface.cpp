// SPDX-License-Identifier: GPL-3.0-or-later
// Raster fallback viewer surface.
#include "ui/viewer/RasterViewerSurface.h"

#include <QPainter>

namespace kk {

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

void RasterViewerSurface::setRenderedFrame(const RenderedFramePtr& frame)
{
    // The image shares its pixels with the RenderedFrame (implicit sharing): no copy.
    if (frame && !frame->image.isNull()) {
        setFrame(frame->image);
    }
}

void RasterViewerSurface::clear(const QString& text)
{
    m_picture.clear(text);
    update();
}

void RasterViewerSurface::setAspect(QSize aspect)
{
    if (m_picture.setAspect(aspect)) {
        update();
    }
}

void RasterViewerSurface::setZoom(double zoom)
{
    if (m_picture.setZoom(zoom)) {
        update();
    }
}

void RasterViewerSurface::setOverlayText(const QString& text)
{
    if (text != m_picture.overlayText()) {
        m_picture.setOverlayText(text);
        update();
    }
}

QRect RasterViewerSurface::pictureRect() const
{
    return m_picture.pictureRect(size(), devicePixelRatioF());
}

void RasterViewerSurface::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    m_picture.paint(painter, size(), true, devicePixelRatioF());
}

} // namespace kk
