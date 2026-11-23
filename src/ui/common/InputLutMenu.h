// SPDX-License-Identifier: GPL-3.0-or-later
// InputLutMenu — the "3D LUT" submenu of the Media Pool and the Color page clip thumbnails
// (Resolve's right-click > 3D LUT): None, the LUT library of the Color page's LUT browser
// (its folders, subfolders as submenus) and Browse… for any other .cube file.
#pragma once

#include <QString>
#include <QStringList>

#include <functional>

class QMenu;
class QWidget;

namespace kk::inputlut {

/// Fills @p menu: "None" (checked when @p current is empty), the library (the entry of
/// @p current checked; a file outside the library gets its own checked entry) and "Browse…".
/// @p pick gets the chosen path (empty = None) when the user picks an entry.
void populate(QMenu* menu, const QString& current, QWidget* dialogParent,
    std::function<void(const QString& path)> pick);

} // namespace kk::inputlut
