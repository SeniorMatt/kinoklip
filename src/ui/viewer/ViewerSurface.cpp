// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerSurface factory, OpenGL availability check and shared letterbox / raster drawing.
#include "ui/viewer/ViewerSurface.h"

#include "ui/common/SchemeColors.h"
#include "ui/viewer/GlViewerSurface.h"
#include "ui/viewer/RasterViewerSurface.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QOpenGLContext>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {
Q_LOGGING_CATEGORY(lcViewer, "kk.ui.viewer", QtInfoMsg)

constexpr int kPictureMargin = 8; // px around the letterboxed picture
} // namespace

bool ViewerSurface::openGLAvailable()
{
    static const bool available = [] {
        if (qEnvironmentVariableIntValue("KK_NO_OPENGL") != 0) {
            return false;
        }
        // These platform plugins may create GL contexts but cannot composite QOpenGLWidgets
        // into a window ("QOpenGLWidget is not supported on this platform").
        static const QStringList kNoWidgetGlPlatforms {QStringLiteral("offscreen"),
            QStringLiteral("minimal"), QStringLiteral("vnc"), QStringLiteral("linuxfb")};
        if (kNoWidgetGlPlatforms.contains(QGuiApplication::platformName())) {
            return false;
        }
        // A throw-away context: cheap, done once. Platforms without GL (offscreen, minimal)
        // fail create() instead of crashing later inside QOpenGLWidget.
        QOpenGLContext context;
        context.setFormat(QSurfaceFormat::defaultFormat());
        if (!context.create()) {
            return false;
        }
        const QSurfaceFormat format = context.format();
        return format.majorVersion() > 3
            || (format.majorVersion() == 3 && format.minorVersion() >= 3);
    }();
    return available;
}

ViewerSurface* ViewerSurface::create(QWidget* parent)
{
    if (openGLAvailable()) {
        return new GlViewerSurface(parent);
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        qCInfo(lcViewer) << "OpenGL 3.3 widgets not available on" << QGuiApplication::platformName()
                         << "- using the raster viewer surface";
    }
    return new RasterViewerSurface(parent);
}

// ---- ViewerPicture ----

void ViewerPicture::setFrame(const QImage& frame)
{
    m_frame = frame;
    m_hasExternalFrame = false;
    m_emptyText.clear();
}

void ViewerPicture::clear(const QString& text)
{
    m_frame = QImage();
    m_hasExternalFrame = false;
    m_emptyText = text;
}

bool ViewerPicture::setAspect(QSize aspect)
{
    if (aspect.isEmpty() || (aspect == m_aspect && m_aspectSet)) {
        return false;
    }
    m_aspect = aspect;
    m_aspectSet = true;
    return true;
}

bool ViewerPicture::setZoom(double zoom)
{
    zoom = zoom <= 0.0 ? 0.0 : std::clamp(zoom, kMinZoom, kMaxZoom);
    if (zoom == m_zoom) {
        return false;
    }
    m_zoom = zoom;
    return true;
}

bool ViewerPicture::setPan(QPointF pan)
{
    if (pan == m_pan) {
        return false;
    }
    m_pan = pan;
    return true;
}

QSize ViewerPicture::sourceSize() const
{
    // The native picture size is the timeline resolution when known (frames may be smaller
    // renders at Half/Quarter quality); bare images (tests, setFrame()) use their own size.
    return m_aspectSet || m_frame.isNull() ? m_aspect : m_frame.size();
}

QRectF ViewerPicture::pictureRectF(QSize widgetSize, qreal dpr, double zoom, QPointF pan) const
{
    const QSize source = sourceSize();
    QSizeF fitted;
    if (zoom > 0.0) {
        // zoom = device pixels per timeline pixel; widget pixels = device / dpr.
        const double scale = zoom / std::max<qreal>(dpr, 0.01);
        fitted = QSizeF(source.width() * scale, source.height() * scale);
    } else {
        const QSize available = widgetSize.shrunkBy(
            QMargins(kPictureMargin, kPictureMargin, kPictureMargin, kPictureMargin));
        const QSize size = source.scaled(available, Qt::KeepAspectRatio);
        // Fit is always centred, on whole pixels (integer division, as pictureRect() draws).
        return QRectF(QPointF((widgetSize.width() - size.width()) / 2,
                          (widgetSize.height() - size.height()) / 2),
            QSizeF(size));
    }
    return QRectF(QPointF((widgetSize.width() - fitted.width()) / 2.0 + pan.x(),
                      (widgetSize.height() - fitted.height()) / 2.0 + pan.y()),
        fitted);
}

QRect ViewerPicture::pictureRect(QSize widgetSize, qreal dpr) const
{
    const QRectF r = pictureRectF(widgetSize, dpr, m_zoom, m_pan);
    if (m_zoom <= 0.0)
        return r.toRect(); // whole pixels already
    // Zoomed: the exact rectangle's corner and size, each rounded (so overlays drawn through
    // this rectangle stay within a pixel of the exact picture mapping at any zoom and pan).
    return QRect(QPoint(int(std::floor(r.x() + 0.5)), int(std::floor(r.y() + 0.5))),
        QSize(int(std::lround(r.width())), int(std::lround(r.height()))));
}

double ViewerPicture::effectiveZoom(QSize widgetSize, qreal dpr) const
{
    if (m_zoom > 0.0) {
        return m_zoom;
    }
    const QSize source = sourceSize();
    if (source.isEmpty()) {
        return 1.0;
    }
    return pictureRectF(widgetSize, dpr, 0.0, {}).width() * dpr / source.width();
}

QPointF ViewerPicture::clampPan(QPointF pan, double zoom, QSize widgetSize, qreal dpr) const
{
    if (zoom <= 0.0) {
        return {};
    }
    const QSizeF pic = pictureRectF(widgetSize, dpr, zoom, {}).size();
    // Centre offset at which only `keep` px of the picture remain inside the widget.
    auto limit = [](double picture, double widget) {
        const double keep = std::min(kMinVisible, picture);
        return std::max(0.0, (picture + widget) / 2.0 - keep);
    };
    const double lx = limit(pic.width(), widgetSize.width());
    const double ly = limit(pic.height(), widgetSize.height());
    return {std::clamp(pan.x(), -lx, lx), std::clamp(pan.y(), -ly, ly)};
}

QPointF ViewerPicture::panForZoomAbout(
    double zoom, QPointF anchor, QSize widgetSize, qreal dpr) const
{
    const QRectF before = pictureRectF(widgetSize, dpr, m_zoom, m_pan);
    if (before.isEmpty() || zoom <= 0.0) {
        return {};
    }
    // The picture point under the anchor, as a fraction of the picture.
    const double u = (anchor.x() - before.x()) / before.width();
    const double v = (anchor.y() - before.y()) / before.height();
    const QSizeF after = pictureRectF(widgetSize, dpr, zoom, {}).size();
    const QPointF topLeft(anchor.x() - u * after.width(), anchor.y() - v * after.height());
    const QPointF pan(topLeft.x() - (widgetSize.width() - after.width()) / 2.0,
        topLeft.y() - (widgetSize.height() - after.height()) / 2.0);
    return clampPan(pan, zoom, widgetSize, dpr);
}

void ViewerPicture::paintTexts(QPainter& painter, const QRect& picture) const
{
    const QString& text
        = hasFrame() ? m_overlay : (m_emptyText.isEmpty() ? m_overlay : m_emptyText);
    if (text.isEmpty()) {
        return;
    }
    const SchemeColors& sc = schemeColors();
    painter.setPen(hasFrame() ? sc.viewerText : mixColors(sc.viewerText, sc.viewerSurround, 0.45));
    painter.drawText(picture, Qt::AlignCenter, text);
}

void ViewerPicture::paint(QPainter& painter, QSize widgetSize, bool fillSurround, qreal dpr) const
{
    if (fillSurround) {
        painter.fillRect(QRect(QPoint(0, 0), widgetSize), schemeColors().viewerSurround);
    }
    const QRect target = pictureRect(widgetSize, dpr);
    // Fixed black: the picture area is video content (letterbox), not scheme chrome.
    painter.fillRect(target, Qt::black);
    if (!m_frame.isNull()) {
        // Premultiplied frame over the black picture area = what the GL path shows.
        painter.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom <= 0.0 || m_zoom < 1.0);
        // Zoomed in: scale only the part of the frame that is visible, not the whole picture.
        const QRectF visible = QRectF(target).intersected(QRectF(QPoint(0, 0), widgetSize));
        if (visible.isEmpty()) {
            // nothing of the picture is inside the widget
        } else if (visible == QRectF(target)) {
            painter.drawImage(target, m_frame);
        } else {
            const double sx = double(m_frame.width()) / target.width();
            const double sy = double(m_frame.height()) / target.height();
            const QRectF source((visible.x() - target.x()) * sx, (visible.y() - target.y()) * sy,
                visible.width() * sx, visible.height() * sy);
            painter.drawImage(visible, m_frame, source);
        }
    }
    paintTexts(painter, target);
}

} // namespace kk
