// SPDX-License-Identifier: GPL-3.0-or-later
// UiFonts — the interface fonts by role. With the "Resolve (Open Sans)" choice (default) every
// role is Open Sans (bundled, SIL OFL 1.1) at Resolve's sizes; with "System font" every role is
// exactly what the program used before the choice existed. Sizes and weights: readme.md.
#pragma once

#include <QFont>
#include <QString>

#include <functional>

class QApplication;

namespace kk::uifonts {

/// Which set of fonts the interface uses.
enum class Choice
{
    Resolve, ///< Open Sans, as DaVinci Resolve's interface (default)
    System, ///< the platform's font and the style sheet's monospace, as before the choice existed
};

/// Where a piece of text sits; each role has one font per choice. Same role = same font, size
/// and weight in every widget, painter and style sheet.
enum class Role
{
    Body, ///< labels, menus, buttons, fields, lists: the application font
    Emphasis, ///< bold body text: track names, selected items, names in lists
    PanelTitle, ///< "Inspector", "Mixer", "Media Pool"; also labels in a semi-bold style sheet
    SectionHeader, ///< inspector group headers ("Volume", "Equalizer")
    Heading, ///< dialog and settings headings
    Title, ///< the big greeting and titles of the welcome window
    Ruler, ///< the numbers on rulers (timeline, keyframes): 10 px in Resolve
    Small, ///< page bar, track headers, ruler numbers, captions
    SmallEmphasis, ///< bold small text: category captions, active curve names
    Tiny, ///< axis and meter labels, badges, node captions
    Timecode, ///< the big timecode in the timeline's top-left corner
    ViewerTimecode, ///< the timecode labels in viewer title bars, the Color page, the vim line
    Mono, ///< real monospace: logs, bug reports, code (never changes with the choice)
};

inline constexpr char kResolveId[] = "resolve";
inline constexpr char kSystemId[] = "system";
/// The bundled family name (resources/fonts).
inline constexpr char kFamily[] = "Open Sans";

[[nodiscard]] QString choiceId(Choice choice);
/// Unknown ids give Resolve (the default).
[[nodiscard]] Choice choiceFromId(const QString& id);

/// The choice in force: the session override (--interface-font) or the stored preference.
[[nodiscard]] Choice choice();
/// Stores the preference (prefs::kInterfaceFont) and clears the session override.
void setChoice(Choice choice);
/// Called after setChoice() changed the choice (Theme re-applies the application font and the
/// style sheet; tests leave it unset).
void setChangedHandler(std::function<void()> handler);
/// Overrides the choice for this run only ("resolve" / "system"; empty or unknown: none).
void setSessionOverride(const QString& id);

/// Registers the bundled Open Sans files (once).
void loadBundledFonts();
/// Font from a kdeglobals [General] font= value; a null font when it cannot be read.
[[nodiscard]] QFont parseKdeFont(const QString& value);
/// Font from a gsettings font-name value ("'Cantarell 11'"); a null font when unreadable.
[[nodiscard]] QFont parseGnomeFont(const QString& value);
/// The desktop's general font: kdeglobals, then gsettings, then QFontDatabase::systemFont.
[[nodiscard]] QFont desktopFont();
/// The application font as the platform gave it, captured before the first change.
[[nodiscard]] QFont systemFont();

/// The font of @p role for @p choice. Roles whose System font is relative to the context
/// (Small, Timecode, Emphasis ...) use @p base, the widget's own font, as the code did before; Resolve
/// fonts ignore it.
[[nodiscard]] QFont font(Role role, Choice which, const QFont& base);
/// Same for the choice in force, with @p base the system font.
[[nodiscard]] QFont font(Role role);
/// Same for the choice in force and a given base font (the widget's own font). @p scale
/// multiplies the size (zoomed painters such as node editors); 1 for everything else.
[[nodiscard]] QFont font(Role role, const QFont& base, double scale = 1.0);
/// @p f in italics (a style flag on a role font: the family and size stay the role's).
[[nodiscard]] QFont italic(QFont f, bool on = true);
/// A font of @p role with a pixel size set by the geometry (overlays that fill a rectangle).
[[nodiscard]] QFont withPixelSize(Role role, const QFont& base, int px);
/// The font declarations of @p role for a style sheet ("font-weight: 600;" ...), for the
/// choice in force, so no style sheet writes a weight of its own.
[[nodiscard]] QString css(Role role);

/// Loads the bundled fonts and sets the application font for the choice in force. Safe to call
/// again after the choice changed (the running widgets follow).
void apply(QApplication& app);

/// Style sheet rules for the labels with a role (PanelTitle, TimecodeLabel), for the choice
/// in force. Not part of the application style sheet any more (see bindLabel).
[[nodiscard]] QString styleSheetRules();

/// Gives @p label the font of @p role and keeps it in step with the interface font choice
/// (apply() refreshes every bound label). Replaces rules in the application style sheet, which
/// re-polished every widget of the program on each theme switch (and a style sheet on the label
/// itself would freeze its palette).
void bindLabel(QWidget* label, Role role);

} // namespace kk::uifonts
