// SPDX-License-Identifier: GPL-3.0-or-later
// Theme — the real KDE Breeze look: widget style, KDE colour schemes, Breeze icons, panel chrome.
#pragma once

#include <QColor>
#include <QString>
#include <QVector>

class QApplication;

namespace kk {
class ActionRegistry;
class SettingsPageRegistry;
} // namespace kk

namespace kk::theme {

/// Id of the default colour scheme (KDE file base name): Kinoklip's own scheme, compiled
/// into the app (resources/color-schemes/Kinoklip.colors), so it exists in every build.
inline constexpr char kDefaultColorScheme[] = "Kinoklip";
/// Used when the default scheme cannot be found (then the style's own palette).
inline constexpr char kFallbackColorScheme[] = "BreezeDark";
/// Default widget style (QStyleFactory key); Fusion is used if the Breeze plugin is missing.
inline constexpr char kDefaultStyle[] = "Breeze";

/// One selectable colour scheme. The empty id is the "system" scheme (KDE: the scheme of the
/// desktop session / kdeglobals; fallback build: the platform palette).
struct ColorSchemeInfo
{
    QString id;
    QString name; ///< display name (translated where KDE provides it)
};

/// True when built with KDE Frameworks (KColorSchemeManager, KIconThemes); false for the
/// palette-only fallback (e.g. Qt 6.4 without KF6).
[[nodiscard]] bool haveKdeFrameworks();

/// Must run before QApplication is constructed: sets up the bundled Breeze icon theme with
/// KDE's recolouring icon engine (KIconTheme::initTheme(), as Kdenlive does on Windows/macOS).
/// No-op without KF6.
void initBeforeApplication();

/// Overrides style and/or colour scheme for this run only (command line --style /
/// --color-scheme; screenshots). Empty strings keep the stored choice. Call before apply().
void setSessionOverrides(const QString& style, const QString& colorScheme);

/// Applies the stored (or overridden) widget style, colour scheme, icon theme and the panel
/// style sheet. Call once after constructing QApplication; calling again re-applies.
void apply(QApplication& app);

/// QStyleFactory keys the user can choose (Breeze first when present).
[[nodiscard]] QStringList availableStyles();
/// The active widget style key.
[[nodiscard]] QString currentStyle();
/// Stores @p style and switches the running application to it (re-applies the colours).
void setStyle(const QString& style);

/// Every colour scheme found (system entry first, then by name). KF6: KColorSchemeManager's
/// model (color-schemes dirs in XDG_DATA_DIRS and the app's ":/org.kde.kcolorscheme"
/// resources) plus any compiled-in scheme the model lacks; fallback: the schemes compiled
/// into the app (resources/color-schemes) plus those in the XDG data dirs.
[[nodiscard]] QVector<ColorSchemeInfo> availableColorSchemes();
/// Id of the active colour scheme ("" = system).
[[nodiscard]] QString currentColorScheme();
/// Stores @p id and switches the running application to it (palette, icons, style sheet).
void setColorScheme(const QString& id);

/// True if the active palette is dark (window background lightness < 50 %).
[[nodiscard]] bool isDark();

/// Registers one checkable action per colour scheme / widget style in the categories
/// "settings.color_scheme" and "settings.style" (shown as Workspace > Color Theme / Style;
/// menus are data, see docs/archive/plan.md 5.9). Ids: settings.color_scheme.<id|system>,
/// settings.style.<lower-case key>. Triggering one switches and persists the choice.
void registerActions(ActionRegistry& registry);

/// Registers the "appearance" settings page (colour theme + widget style combo boxes); it
/// applies through the actions of registerActions() so menus and page stay in sync.
void registerSettingsPage(SettingsPageRegistry& pages, ActionRegistry& registry);

/// Action id for colour scheme @p id / style @p style (see registerActions()).
[[nodiscard]] QString colorSchemeActionId(const QString& id);
[[nodiscard]] QString styleActionId(const QString& style);

} // namespace kk::theme
