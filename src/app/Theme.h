// SPDX-License-Identifier: GPL-3.0-or-later
// Theme — the app's own dark look (Fusion + palette + small style sheet), or the system look.
#pragma once

#include <QColor>

class QApplication;

namespace kk::theme {

/// The single accent colour of the dark theme (selection, checked tool buttons, playhead).
[[nodiscard]] QColor accentColor();

/// Applies the theme chosen in the settings: the own dark theme by default, or the platform
/// style and palette (e.g. Breeze on KDE Plasma) when useSystemColors() is true. A small style
/// sheet for the panel chrome is generated from the active palette in both cases.
/// Call once after constructing QApplication; calling again switches live.
void apply(QApplication& app);

/// True if the user chose "Use System Colors" (prefs::kUseSystemColors).
[[nodiscard]] bool useSystemColors();
/// Stores the choice and re-applies the theme to the running application.
void setUseSystemColors(bool on);

} // namespace kk::theme
