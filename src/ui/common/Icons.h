// SPDX-License-Identifier: GPL-3.0-or-later
// Icons — Breeze theme icons for standard actions, own palette-tinted SVGs for editor glyphs.
#pragma once

#include <QColor>
#include <QIcon>
#include <QString>
#include <QStringList>

#include <functional>

class QPainter;

namespace kk {

/// Icon for our name @p name ("play", "blade", ...). If @p name maps to a freedesktop/Breeze
/// icon (themeIconName()) that the current icon theme has, that theme icon is returned
/// (Breeze, recoloured from the palette by KIconEngine in the KF6 build). Otherwise the own
/// resources/icons/<name>.svg, tinted at paint time from the application palette:
/// normal = WindowText, selected = HighlightedText, disabled = faded WindowText.
/// Unknown names return a null QIcon (and log a warning once).
[[nodiscard]] QIcon icon(const QString& name);

/// The Breeze / freedesktop icon name used for @p name, or empty if we draw our own.
[[nodiscard]] QString themeIconName(const QString& name);
/// Our names that map to theme icons (for tests).
[[nodiscard]] QStringList iconNames();

/// Same icon drawn in a fixed colour @p color for every mode/state (e.g. a red record dot).
[[nodiscard]] QIcon icon(const QString& name, const QColor& color);

/// Draws a glyph into a @p size (device-independent px) canvas with pen/brush colour @p color.
using GlyphPainter
    = std::function<void(QPainter& painter, const QSizeF& size, const QColor& color)>;

/// Icon painted on demand by @p draw in the CURRENT application palette, so it follows colour
/// scheme switches (an icon pre-rendered into a QPixmap keeps the old scheme's colours).
/// Colours per mode: normal = WindowText at @p normalOpacity, active = WindowText, selected =
/// HighlightedText, disabled = the disabled WindowText. Rendered pixmaps are kept in
/// QPixmapCache under @p cacheKey + size + colour, so painting stays cheap.
[[nodiscard]] QIcon paletteIcon(
    const QString& cacheKey, GlyphPainter draw, qreal normalOpacity = 1.0);

/// True if an icon with this name exists in the resources.
[[nodiscard]] bool hasIcon(const QString& name);

} // namespace kk
