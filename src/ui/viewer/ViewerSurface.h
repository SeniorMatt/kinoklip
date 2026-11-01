// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerSurface — the picture area of a viewer. OpenGL (GlViewerSurface) when available,
// raster fallback (RasterViewerSurface) otherwise. PLACEHOLDER drawing; keep the interface.
#pragma once

#include <QImage>
#include <QRect>
#include <QString>

class QPainter;
class QWidget;

namespace cw {

/// Interface of a viewer picture area. Implementations are QWidgets; use widget() to put one
/// in a layout.
///
/// GlViewerSurface (QOpenGLWidget) is the real thing: playback should eventually render the
/// GPU compositor's texture into it (contexts are shared, see main.cpp). RasterViewerSurface
/// exists for platforms without OpenGL 3.3 (QT_QPA_PLATFORM=offscreen screenshots, CI, VMs).
/// Thread-safety: GUI thread only.
class ViewerSurface
{
public:
    virtual ~ViewerSurface() = default;

    /// Creates the best available surface: GL if openGLAvailable(), raster otherwise.
    [[nodiscard]] static ViewerSurface* create(QWidget* parent);
    /// True if an OpenGL 3.3 core context can be created (cached after the first call).
    /// Setting the environment variable CW_NO_OPENGL=1 forces false.
    [[nodiscard]] static bool openGLAvailable();

    /// The QWidget implementing the surface.
    [[nodiscard]] virtual QWidget* widget() = 0;
    /// Shows @p frame scaled to fit (implicitly shared, no pixel copy).
    virtual void setFrame(const QImage& frame) = 0;
    /// Shows black with @p text centred in the picture area (empty state).
    virtual void clear(const QString& text = {}) = 0;
    /// Picture aspect used for the empty-state frame (timeline resolution).
    virtual void setAspect(QSize aspect) = 0;
    /// Rectangle (widget pixels) the picture occupies, letterboxed with a small margin.
    [[nodiscard]] virtual QRect pictureRect() const = 0;
};

/// State + drawing shared by both implementations (placeholder: QPainter-based).
class ViewerPicture
{
public:
    void setFrame(const QImage& frame);
    void clear(const QString& text);
    /// Returns true if the aspect changed.
    bool setAspect(QSize aspect);
    /// Letterboxed picture rectangle inside a widget of @p widgetSize.
    [[nodiscard]] QRect pictureRect(QSize widgetSize) const;
    /// Paints surround, picture (or empty-state text) for a widget of @p widgetSize.
    void paint(QPainter& painter, QSize widgetSize, bool fillSurround) const;

private:
    QImage m_frame;
    QString m_emptyText;
    QSize m_aspect {16, 9};
};

} // namespace cw
