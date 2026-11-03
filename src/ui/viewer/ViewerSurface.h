// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerSurface — the picture area of a viewer. OpenGL (GlViewerSurface) when available,
// raster fallback (RasterViewerSurface) otherwise. Keep the interface when extending.
#pragma once

#include "render/RenderTypes.h"

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>

class QPainter;
class QWidget;

namespace kk {

class ViewerPicture;

/// Interface of a viewer picture area. Implementations are QWidgets; use widget() to put one
/// in a layout.
///
/// GlViewerSurface (QOpenGLWidget) draws the GPU compositor's output texture directly (zero
/// copy: contexts share objects, see main.cpp) or uploads images; RasterViewerSurface exists
/// for platforms without OpenGL 3.3 (QT_QPA_PLATFORM=offscreen screenshots, CI, VMs) and draws
/// images with QPainter.
///
/// Geometry: the picture is the aspect set with setAspect() (the timeline resolution) — not
/// the size of the frame, which may be a reduced-quality render — fitted into the widget with
/// a small margin (zoom 0) or scaled to zoom x timeline pixels per device pixel, centred and
/// moved by the pan offset (widget pixels; only while zoomed).
///
/// Thread-safety: GUI thread only.
class ViewerSurface
{
public:
    virtual ~ViewerSurface() = default;

    /// Creates the best available surface: GL if openGLAvailable(), raster otherwise.
    [[nodiscard]] static ViewerSurface* create(QWidget* parent);
    /// True if an OpenGL 3.3 core context can be created (cached after the first call).
    /// Setting the environment variable KK_NO_OPENGL=1 forces false.
    [[nodiscard]] static bool openGLAvailable();

    /// The QWidget implementing the surface.
    [[nodiscard]] virtual QWidget* widget() = 0;
    /// Shows @p frame scaled to fit (implicitly shared, no pixel copy).
    virtual void setFrame(const QImage& frame) = 0;
    /// Shows a compositor frame: its texture where the surface can draw textures (zero copy),
    /// otherwise its image. The frame is kept alive until a later frame has been drawn.
    virtual void setRenderedFrame(const RenderedFramePtr& frame) = 0;
    /// True if setRenderedFrame() can draw RenderedFrame::texture (GL surfaces).
    [[nodiscard]] virtual bool canDrawTextures() const = 0;
    /// Shows black with @p text centred in the picture area (empty state).
    virtual void clear(const QString& text = {}) = 0;
    /// Picture aspect / native size (timeline resolution).
    virtual void setAspect(QSize aspect) = 0;
    /// 0 = fit to the widget; otherwise device pixels per timeline pixel (0.5, 1, 2). Keeps
    /// the pan offset.
    virtual void setZoom(double zoom) = 0;
    /// Sets zoom (as setZoom()) and pan offset (widget px of the picture centre from the
    /// widget centre; ignored at Fit) together, with one repaint.
    virtual void setView(double zoom, QPointF pan) = 0;
    /// The zoom / pan state and its maths.
    [[nodiscard]] virtual const ViewerPicture& picture() const = 0;
    /// Rectangle (widget pixels) the picture occupies (may exceed the widget when zoomed).
    [[nodiscard]] virtual QRect pictureRect() const = 0;
    /// Text drawn over the picture's centre (e.g. "Audio only"); empty = none.
    virtual void setOverlayText(const QString& text) = 0;
};

/// State and letterbox maths shared by both implementations; paint() is the raster drawing.
class ViewerPicture
{
public:
    void setFrame(const QImage& frame);
    void clear(const QString& text);
    /// Returns true if the aspect changed.
    bool setAspect(QSize aspect);
    /// Smallest and largest zoom (device px per timeline px) a viewer offers.
    static constexpr double kMinZoom = 0.1;
    static constexpr double kMaxZoom = 16.0;

    /// Returns true if the zoom changed.
    bool setZoom(double zoom);
    [[nodiscard]] double zoom() const { return m_zoom; }
    /// Pan offset in widget px (picture centre minus widget centre); used only while zoomed.
    /// Returns true if it changed.
    bool setPan(QPointF pan);
    [[nodiscard]] QPointF pan() const { return m_pan; }
    /// The zoom shown now: zoom(), or at Fit the device px per timeline px of the fitted
    /// picture.
    [[nodiscard]] double effectiveZoom(QSize widgetSize, qreal dpr = 1.0) const;
    /// The pan that keeps the picture point under @p anchor (widget px) in place when the
    /// zoom changes from the current state to @p zoom (> 0), clamped by clampPan().
    [[nodiscard]] QPointF panForZoomAbout(
        double zoom, QPointF anchor, QSize widgetSize, qreal dpr = 1.0) const;
    /// @p pan limited so that at least kMinVisible px (or the whole picture, when smaller) of
    /// the picture at @p zoom stays inside the widget on each axis.
    [[nodiscard]] QPointF clampPan(
        QPointF pan, double zoom, QSize widgetSize, qreal dpr = 1.0) const;
    /// Exact (unrounded) picture rectangle for @p zoom and @p pan; pictureRect() rounds it.
    [[nodiscard]] QRectF pictureRectF(QSize widgetSize, qreal dpr, double zoom, QPointF pan) const;
    void setOverlayText(const QString& text) { m_overlay = text; }
    [[nodiscard]] const QString& overlayText() const { return m_overlay; }
    [[nodiscard]] const QString& emptyText() const { return m_emptyText; }
    [[nodiscard]] bool hasFrame() const { return !m_frame.isNull() || m_hasExternalFrame; }
    /// Marks that a frame without a QImage (a GL texture) is shown.
    void setExternalFrame(bool on)
    {
        m_hasExternalFrame = on;
        if (on) {
            m_emptyText.clear();
        }
    }
    /// Letterboxed picture rectangle inside a widget of @p widgetSize with @p dpr device
    /// pixels per widget pixel.
    [[nodiscard]] QRect pictureRect(QSize widgetSize, qreal dpr = 1.0) const;
    /// Paints surround, picture (or empty-state text) for a widget of @p widgetSize.
    void paint(QPainter& painter, QSize widgetSize, bool fillSurround, qreal dpr = 1.0) const;
    /// Paints only the overlay / empty text (used on top of GL).
    void paintTexts(QPainter& painter, const QRect& picture) const;

private:
    QImage m_frame;
    QString m_emptyText;
    QString m_overlay;
    QSize m_aspect {16, 9};
    bool m_aspectSet = false;
    bool m_hasExternalFrame = false;
    /// Native picture size: the timeline resolution, or a bare image's own size.
    [[nodiscard]] QSize sourceSize() const;

    double m_zoom = 0.0;
    QPointF m_pan;
    static constexpr double kMinVisible = 48.0;
};

} // namespace kk
