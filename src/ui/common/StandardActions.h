// SPDX-License-Identifier: GPL-3.0-or-later
// The catalogue of all user commands (ids, texts, menus, icons) registered at start-up.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace kk {

class ActionRegistry;

/// A menu of the main window: category id used by ActionRegistry + translated title.
struct MenuCategory
{
    QString id; ///< e.g. "file"
    QString title; ///< e.g. "&File"
};

/// Menus in menu-bar order (File, Edit, Trim, Timeline, Clip, Mark, View, Playback, Help).
[[nodiscard]] QVector<MenuCategory> standardMenus();

/// Registers every command of PLAN §5.7 and the main window. Behaviour is connected by the
/// owning modules (see ui/common/readme.md for the id list and who owns what).
void registerStandardActions(ActionRegistry& registry);

/// True while the keyboard focus is in a node editor (a widget with the property
/// "kkNodeEditor"): Edit > Cut, Copy, Paste, Delete and Select All then act on its nodes
/// (CompositingPage) and not on the timeline (TimelineController).
[[nodiscard]] bool nodeEditorHasFocus();

/// Gives every standard command its one-line description as the status tip
/// (ActionDescriptions.cpp), unless it already has one. Call it once all commands are registered.
/// Returns the ids of descriptions that match no registered command (empty when the table is in
/// step; a test checks).
QStringList describeStandardActions(ActionRegistry& registry);

} // namespace kk
