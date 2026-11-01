// SPDX-License-Identifier: GPL-3.0-or-later
// RasterViewerSurface — QPainter fallback viewer picture area for platforms without OpenGL.
#pragma once

#include "ui/viewer/ViewerSurface.h"

#include <QWidget>

namespace cw {

/// Plain QWidget implementation of ViewerSurface (offscreen screenshots, CI, no-GPU VMs).
class RasterViewerSurface : public QWidget, public ViewerSurface
{
    Q_OBJECT
public:
    /// Creates an empty (black) surface.
    explicit RasterViewerSurface(QWidget* parent = nullptr);

    QWidget* widget() override { return this; }
    void setFrame(const QImage& frame) override;
    void clear(const QString& text = {}) override;
    void setAspect(QSize aspect) override;
    [[nodiscard]] QRect pictureRect() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    ViewerPicture m_picture;
};

} // namespace cw
