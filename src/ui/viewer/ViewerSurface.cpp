// SPDX-License-Identifier: GPL-3.0-or-later
// ViewerSurface factory, OpenGL availability check and shared placeholder drawing.
#include "ui/viewer/ViewerSurface.h"

#include "ui/viewer/GlViewerSurface.h"
#include "ui/viewer/RasterViewerSurface.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QOpenGLContext>
#include <QPainter>

namespace cw {

namespace {
Q_LOGGING_CATEGORY(lcViewer, "cw.ui.viewer")

constexpr int kPictureMargin = 8; // px around the letterboxed picture
const QColor kSurroundColor(0x0b, 0x0b, 0x0c); // slightly lifted black around the picture
const QColor kEmptyTextColor(0x5a, 0x5a, 0x60);
} // namespace

bool ViewerSurface::openGLAvailable()
{
    static const bool available = [] {
        if (qEnvironmentVariableIntValue("CW_NO_OPENGL") != 0)
            return false;
        // These platform plugins may create GL contexts but cannot composite QOpenGLWidgets
        // into a window ("QOpenGLWidget is not supported on this platform").
        static const QStringList kNoWidgetGlPlatforms {QStringLiteral("offscreen"),
            QStringLiteral("minimal"), QStringLiteral("vnc"), QStringLiteral("linuxfb")};
        if (kNoWidgetGlPlatforms.contains(QGuiApplication::platformName()))
            return false;
        // A throw-away context: cheap, done once. Platforms without GL (offscreen, minimal)
        // fail create() instead of crashing later inside QOpenGLWidget.
        QOpenGLContext context;
        context.setFormat(QSurfaceFormat::defaultFormat());
        if (!context.create())
            return false;
        const QSurfaceFormat format = context.format();
        return format.majorVersion() > 3
            || (format.majorVersion() == 3 && format.minorVersion() >= 3);
    }();
    return available;
}

ViewerSurface* ViewerSurface::create(QWidget* parent)
{
    if (openGLAvailable())
        return new GlViewerSurface(parent);
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
    m_emptyText.clear();
}

void ViewerPicture::clear(const QString& text)
{
    m_frame = QImage();
    m_emptyText = text;
}

bool ViewerPicture::setAspect(QSize aspect)
{
    if (aspect.isEmpty() || aspect == m_aspect)
        return false;
    m_aspect = aspect;
    return true;
}

QRect ViewerPicture::pictureRect(QSize widgetSize) const
{
    const QSize source = m_frame.isNull() ? m_aspect : m_frame.size();
    const QSize available = widgetSize.shrunkBy(
        QMargins(kPictureMargin, kPictureMargin, kPictureMargin, kPictureMargin));
    const QSize fitted = source.scaled(available, Qt::KeepAspectRatio);
    return QRect(QPoint((widgetSize.width() - fitted.width()) / 2,
                     (widgetSize.height() - fitted.height()) / 2),
        fitted);
}

void ViewerPicture::paint(QPainter& painter, QSize widgetSize, bool fillSurround) const
{
    if (fillSurround)
        painter.fillRect(QRect(QPoint(0, 0), widgetSize), kSurroundColor);
    const QRect target = pictureRect(widgetSize);
    if (!m_frame.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, m_frame);
        return;
    }
    painter.fillRect(target, Qt::black);
    if (!m_emptyText.isEmpty()) {
        painter.setPen(kEmptyTextColor);
        painter.drawText(target, Qt::AlignCenter, m_emptyText);
    }
}

} // namespace cw
