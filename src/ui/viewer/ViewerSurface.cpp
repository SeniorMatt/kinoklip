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
    if (zoom == m_zoom) {
        return false;
    }
    m_zoom = std::max(0.0, zoom);
    return true;
}

QRect ViewerPicture::pictureRect(QSize widgetSize, qreal dpr) const
{
    // The native picture size is the timeline resolution when known (frames may be smaller
    // renders at Half/Quarter quality); bare images (tests, setFrame()) use their own size.
    const QSize source = m_aspectSet || m_frame.isNull() ? m_aspect : m_frame.size();
    QSize fitted;
    if (m_zoom > 0.0) {
        // zoom = device pixels per timeline pixel; widget pixels = device / dpr.
        const double scale = m_zoom / std::max<qreal>(dpr, 0.01);
        fitted = QSize(
            int(std::lround(source.width() * scale)), int(std::lround(source.height() * scale)));
    } else {
        const QSize available = widgetSize.shrunkBy(
            QMargins(kPictureMargin, kPictureMargin, kPictureMargin, kPictureMargin));
        fitted = source.scaled(available, Qt::KeepAspectRatio);
    }
    return QRect(QPoint((widgetSize.width() - fitted.width()) / 2,
                     (widgetSize.height() - fitted.height()) / 2),
        fitted);
}

void ViewerPicture::paintTexts(QPainter& painter, const QRect& picture) const
{
    const QString& text
        = hasFrame() ? m_overlay : (m_emptyText.isEmpty() ? m_overlay : m_emptyText);
    if (text.isEmpty()) {
        return;
    }
    const SchemeColors& sc = schemeColors();
    painter.setPen(
        hasFrame() ? sc.viewerText : mixColors(sc.viewerText, sc.viewerSurround, 0.45));
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
        painter.drawImage(target, m_frame);
    }
    paintTexts(painter, target);
}

} // namespace kk
