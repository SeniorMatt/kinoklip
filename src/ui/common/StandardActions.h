// SPDX-License-Identifier: GPL-3.0-or-later
// The catalogue of all user commands (ids, texts, menus, icons) registered at start-up.
#pragma once

#include <QString>
#include <QVector>

namespace cw {

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
/// owning modules (see ui/common/README.md for the id list and who owns what).
void registerStandardActions(ActionRegistry& registry);

} // namespace cw
