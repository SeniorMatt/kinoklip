// SPDX-License-Identifier: GPL-3.0-or-later
// MenuLayouts — the menu bar's two arrangements. "Resolve" is the table in StandardActions.cpp
// (menus as in DaVinci Resolve); "Streamlined" is the table in MenuLayouts.cpp: shorter menus,
// grouped by task, plain names, one page menu (docs/behavior-spec.md section 82).
// Only the arrangement and some menu labels differ: the actions, their ids, shortcuts, tool
// tips and the Vim command picker are the same in both.
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QAction;
class QMenu;

namespace kk {

class ActionRegistry;

namespace menulayout {

/// QSettings key of the choice (stored as the layout id).
inline constexpr char kSettingsKey[] = "ui/menuLayout";

enum class Layout {
    Resolve, ///< the menus as in DaVinci Resolve (default)
    Streamlined ///< shorter menus grouped by task
};

[[nodiscard]] QString layoutId(Layout layout); ///< "resolve" | "streamlined"
[[nodiscard]] Layout layoutFromId(const QString& id); ///< unknown ids give Resolve
/// The stored layout.
[[nodiscard]] Layout layout();
[[nodiscard]] bool streamlined();
/// Stores @p layout and, if it changed, emits Notifier::changed() so the menu bar is rebuilt.
void setLayout(Layout layout);

class Notifier : public QObject
{
    Q_OBJECT
signals:
    void changed();
};
[[nodiscard]] Notifier& notifier();

/// One entry of a Streamlined menu.
struct Node
{
    enum class Kind {
        Action, ///< a registered command (id), optionally with another menu label
        Separator,
        Submenu, ///< a submenu with a title and children
        Category ///< a submenu filled from a registry category (colour schemes, recent files)
    };
    Kind kind = Kind::Action;
    QString id; ///< Action: action id; Category: registry category
    QString label; ///< menu text instead of the action's own (Action), title (Submenu, Category)
    QList<Node> children;
};

struct Menu
{
    QString id; ///< "file", "edit", ...; the menu's object name is "menu:<id>"
    QString title; ///< with "&" mnemonic
    QList<Node> entries;
};

/// The Streamlined menus in menu-bar order, without the page menu (which goes before Help).
[[nodiscard]] QList<Menu> streamlinedMenus();
/// Streamlined menu of page @p pageId ("cut", "compositing", "color"); empty id for other pages.
[[nodiscard]] Menu pageMenu(const QString& pageId);

/// Commands deliberately not in the Streamlined menu bar, each with the reason: the numbered
/// tracks (keys and track headers), generic duplicates, commands that live in a panel's menu.
/// An id ending in '*' is a prefix.
struct HiddenAction
{
    const char* id;
    const char* reason;
};
[[nodiscard]] QList<HiddenAction> hiddenActions();
[[nodiscard]] bool isHidden(const QString& actionId);

/// Fills @p menu with @p nodes. Entries whose action is not registered are skipped. An entry
/// with a label of its own is a mirror of the action (same checks, enabled state, icon, key;
/// triggering it triggers the action). @p path names the submenus ("menu:file.import").
void fill(QMenu* menu, const QList<Node>& nodes, ActionRegistry& registry, const QString& path);

/// Colour and flag palettes are not lists of commands: they may be longer than 12 entries, but
/// the Streamlined layout lays them out in groups of four. True if a line goes before palette
/// entry @p index (0-based).
[[nodiscard]] bool paletteBreakBefore(int index);

/// Adds the command @p id to @p menu under @p label (the action's own text when empty), the way
/// fill() adds an Action node. Returns the entry, or nullptr if the command is not registered.
/// For context menus, which are built in code.
QAction* addCommand(
    QMenu* menu, ActionRegistry& registry, const QString& id, const QString& label = {});

/// The action id a menu entry stands for: the registry id for a mirror, empty if @p entry is not
/// a mirror.
[[nodiscard]] QString mirroredId(const QAction* entry);

} // namespace menulayout
} // namespace kk
