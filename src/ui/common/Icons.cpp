// SPDX-License-Identifier: GPL-3.0-or-later
// Icon lookup: Breeze theme icons for standard actions, tinted own SVGs for editor glyphs.
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

namespace kk {

namespace {

Q_LOGGING_CATEGORY(lcIcons, "kk.ui.icons", QtInfoMsg)

QString iconPath(const QString& name)
{
    return QStringLiteral(":/icons/%1.svg").arg(name);
}

/// Our icon name -> freedesktop / Breeze icon name. Names not listed here (blade, trim,
/// insert/overwrite, mark in/out, edit points, snapping, single viewer, the page and some panel
/// glyphs) have no fitting Breeze icon and keep our own SVG, tinted from the palette.
/// tests/app/test_theme.cpp checks that every target exists in the Breeze theme.
struct ThemeIcon
{
    const char* ours;
    const char* theme;
};
// clang-format off
constexpr ThemeIcon kThemeIcons[] = {
    {"add",              "list-add"},
    {"audio",            "audio-volume-high"},
    {"audio_file",       "audio-x-generic"},
    {"edit",             "document-edit"},
    {"export",           "document-export"},
    {"film",             "video-x-generic"},
    {"folder",           "folder"},
    {"smart_bin",        "folder-calculate"},
    {"power_bin",        "folder-favorites"},
    {"flag",             "flag"},
    {"fullscreen",       "view-fullscreen"},
    {"go_end",           "media-skip-forward"},
    {"go_start",         "media-skip-backward"},
    {"go_up",            "go-up"},
    {"grid_view",        "view-list-icons"},
    {"hidden",           "view-hidden"},
    {"image",            "image-x-generic"},
    {"import",           "document-import"},
    {"keyframe_next",    "keyframe-next"},
    {"keyframe_on",      "keyframe-add"},
    {"keyframe_prev",    "keyframe-previous"},
    {"link",             "link"},
    {"list_view",        "view-list-details"},
    {"lock",             "object-locked"},
    {"loop",             "media-playlist-repeat"},
    {"marker",           "bookmark-new"},
    {"mute",             "audio-volume-muted"},
    {"next_frame",       "media-seek-forward"},
    {"panel_edit_index", "view-list-text"},
    {"panel_inspector",  "document-properties"},
    {"panel_metadata",   "documentinfo"},
    {"panel_mixer",      "view-media-equalizer"},
    {"pause",            "media-playback-pause"},
    {"play",             "media-playback-start"},
    {"prev_frame",       "media-seek-backward"},
    {"remove",           "list-remove"},
    {"search",           "edit-find"},
    {"select",           "tool-pointer"},
    {"solo",             "audio-headphones"},
    {"split",            "edit-cut"},
    {"stop",             "media-playback-stop"},
    {"title_clip",       "draw-text"},
    {"unlock",           "object-unlocked"},
    {"visible",          "view-visible"},
    {"warning",          "dialog-warning"},
    {"zoom_fit",         "zoom-fit-best"},
    {"zoom_in",          "zoom-in"},
    {"zoom_out",         "zoom-out"},
};
// clang-format on

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
        const QString key = QStringLiteral("kkicon:%1:%2x%3:%4")
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
    QString key() const override { return QStringLiteral("kk.tintedsvg"); }

private:
    /// Same colours Breeze icons get from KIconEngine: text colour normally, highlighted text
    /// on selections, faded when disabled. Checked/hovered states are shown by the widget
    /// style's button background (Breeze convention), not by recolouring the glyph.
    QColor tintFor(QIcon::Mode mode, QIcon::State state) const
    {
        Q_UNUSED(state);
        const QPalette pal = QGuiApplication::palette();
        QColor c = m_fixedColor.isValid() ? m_fixedColor
            : mode == QIcon::Selected     ? pal.color(QPalette::HighlightedText)
                                          : pal.color(QPalette::WindowText);
        if (mode == QIcon::Disabled)
            c.setAlphaF(0.35f);
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

/// Engine of paletteIcon(): looks the colour up at paint time.
class PaletteIconEngine final : public QIconEngine
{
public:
    PaletteIconEngine(QString key, GlyphPainter draw, qreal normalOpacity)
        : m_key(std::move(key))
        , m_draw(std::move(draw))
        , m_normalOpacity(normalOpacity)
    { }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, dpr));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State, qreal scale) override
    {
        const QPalette pal = QGuiApplication::palette();
        QColor c;
        switch (mode) {
        case QIcon::Disabled:
            c = pal.color(QPalette::Disabled, QPalette::WindowText);
            break;
        case QIcon::Selected:
            c = pal.color(QPalette::Active, QPalette::HighlightedText);
            break;
        case QIcon::Active:
            c = pal.color(QPalette::Active, QPalette::WindowText);
            break;
        case QIcon::Normal:
            c = pal.color(QPalette::Active, QPalette::WindowText);
            c.setAlphaF(float(c.alphaF() * m_normalOpacity));
            break;
        }
        const QSize device = (QSizeF(size) * scale).toSize();
        const QString key = QStringLiteral("kkpal:%1:%2x%3:%4")
                                .arg(m_key)
                                .arg(device.width())
                                .arg(device.height())
                                .arg(c.rgba(), 0, 16);
        QPixmap cached;
        if (QPixmapCache::find(key, &cached))
            return cached;
        QPixmap pm(device);
        pm.setDevicePixelRatio(scale);
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            m_draw(p, QSizeF(size), c);
        }
        QPixmapCache::insert(key, pm);
        return pm;
    }

    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }
    QIconEngine* clone() const override
    {
        return new PaletteIconEngine(m_key, m_draw, m_normalOpacity);
    }
    QString key() const override { return QStringLiteral("kk.paletteicon"); }

private:
    QString m_key;
    GlyphPainter m_draw;
    qreal m_normalOpacity;
};

} // namespace

QIcon paletteIcon(const QString& cacheKey, GlyphPainter draw, qreal normalOpacity)
{
    return QIcon(new PaletteIconEngine(cacheKey, std::move(draw), normalOpacity));
}

QString themeIconName(const QString& name)
{
    for (const ThemeIcon& entry : kThemeIcons) {
        if (name == QLatin1String(entry.ours))
            return QLatin1String(entry.theme);
    }
    return {};
}

QIcon icon(const QString& name)
{
    // Theme icons follow theme and palette changes by themselves (QIconLoaderEngine reloads
    // on a theme switch, KIconEngine recolours per paint), so they can be cached by callers.
    const QString themeName = themeIconName(name);
    if (!themeName.isEmpty() && QIcon::hasThemeIcon(themeName))
        return QIcon::fromTheme(themeName);
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

QStringList iconNames()
{
    QStringList names;
    for (const ThemeIcon& entry : kThemeIcons)
        names.append(QLatin1String(entry.ours));
    return names;
}

} // namespace kk
