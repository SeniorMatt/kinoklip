// SPDX-License-Identifier: GPL-3.0-or-later
// Icons — loads the app's own monochrome SVG icons and tints them to match the palette.
#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace cw {

/// Icon from resources/icons/<name>.svg, tinted at paint time from the application palette:
/// normal = WindowText, disabled = disabled WindowText, checked ("On" state) = Highlight.
/// Unknown names return a null QIcon (and log a warning once).
[[nodiscard]] QIcon icon(const QString& name);

/// Same icon drawn in a fixed colour @p color for every mode/state (e.g. a red record dot).
[[nodiscard]] QIcon icon(const QString& name, const QColor& color);

/// True if an icon with this name exists in the resources.
[[nodiscard]] bool hasIcon(const QString& name);

} // namespace cw
