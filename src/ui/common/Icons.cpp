// SPDX-License-Identifier: GPL-3.0-or-later
// Tinted SVG icon engine: renders a monochrome SVG and recolours it per icon mode/state.
#include "ui/common/Icons.h"

#include <QFile>
#include <QGuiApplication>
#include <QIconEngine>
#include <QLoggingCategory>
#include <QPainter>
#include <QPalette>
#include <QPixmapCache>
#include <QSet>
#include <QSvgRenderer>

namespace cw {

namespace {

Q_LOGGING_CATEGORY(lcIcons, "cw.ui.icons")

QString iconPath(const QString& name)
{
    return QStringLiteral(":/icons/%1.svg").arg(name);
}

/// Draws the SVG (designed in a single colour) and replaces its colour with the tint.
class TintedSvgIconEngine : public QIconEngine
{
public:
    TintedSvgIconEngine(const QString& name, const QColor& fixedColor)
        : m_name(name)
        , m_fixedColor(fixedColor)
    { }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        QPixmap pm = pixmap(rect.size() * dpr, mode, state);
        pm.setDevicePixelRatio(dpr);
        painter->drawPixmap(rect, pm);
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        const QColor tint = tintFor(mode, state);
        const QString key = QStringLiteral("cwicon:%1:%2x%3:%4")
                                .arg(m_name)
                                .arg(size.width())
                                .arg(size.height())
                                .arg(tint.rgba(), 0, 16);
        QPixmap cached;
        if (QPixmapCache::find(key, &cached))
            return cached;

        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QSvgRenderer renderer(iconPath(m_name));
        if (renderer.isValid()) {
            QPainter p(&image);
            renderer.render(&p, QRectF(QPointF(0, 0), QSizeF(size)));
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(image.rect(), tint);
        }
        QPixmap result = QPixmap::fromImage(image);
        QPixmapCache::insert(key, result);
        return result;
    }

    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }
    QIconEngine* clone() const override { return new TintedSvgIconEngine(m_name, m_fixedColor); }
    QString key() const override { return QStringLiteral("cw.tintedsvg"); }

private:
    QColor tintFor(QIcon::Mode mode, QIcon::State state) const
    {
        const QPalette pal = QGuiApplication::palette();
        QColor c = m_fixedColor.isValid() ? m_fixedColor
            : state == QIcon::On          ? pal.color(QPalette::Highlight)
            : mode == QIcon::Selected     ? pal.color(QPalette::HighlightedText)
                                          : pal.color(QPalette::WindowText);
        if (mode == QIcon::Disabled) {
            c = m_fixedColor.isValid() ? m_fixedColor : pal.color(QPalette::WindowText);
            c.setAlphaF(0.35f);
        } else if (mode == QIcon::Active && state == QIcon::Off && !m_fixedColor.isValid()) {
            c = pal.color(QPalette::BrightText);
        }
        return c;
    }

    QString m_name;
    QColor m_fixedColor;
};

QIcon makeIcon(const QString& name, const QColor& color)
{
    if (!hasIcon(name)) {
        static QSet<QString> warned;
        if (!warned.contains(name)) {
            warned.insert(name);
            qCWarning(lcIcons) << "unknown icon" << name;
        }
        return {};
    }
    return QIcon(new TintedSvgIconEngine(name, color));
}

} // namespace

QIcon icon(const QString& name)
{
    return makeIcon(name, QColor());
}

QIcon icon(const QString& name, const QColor& color)
{
    return makeIcon(name, color);
}

bool hasIcon(const QString& name)
{
    return !name.isEmpty() && QFile::exists(iconPath(name));
}

} // namespace cw
