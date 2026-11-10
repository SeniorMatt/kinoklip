// SPDX-License-Identifier: GPL-3.0-or-later
// MenuReference — docs/menu-reference.md written by the application itself (spec section 63).
#pragma once

#include <QString>
#include <QStringList>

namespace kk {

class MainWindow;

namespace menureference {

/// What walking the window found.
struct Reference
{
    QString markdown; ///< the whole page
    int entries = 0; ///< menu entries (rows of the menu tables)
    int menus = 0; ///< menu tables
    int panels = 0; ///< panel rows (a panel shown on several pages counts for each page)
    /// Entries (as "Menu > Entry") and panels without a description. Empty when complete.
    QStringList undescribed;
};

/// Walks the menu bar of @p window (submenus too), the registry actions that no menu shows and
/// the panels of every page, and writes them as Markdown. Everything comes from the live
/// objects: the menu tables and the descriptions set where the actions and panels are built.
/// The window must have been made from default settings for the result to be reproducible.
[[nodiscard]] Reference build(MainWindow& window);

/// Runs build() and writes the page to @p path (UTF-8, "\n" line ends). False with @p error set
/// when the file cannot be written. Entries without a description are an error too, so the
/// file is never written incomplete; the list is in @p error.
bool writeFile(MainWindow& window, const QString& path, QString* error);

} // namespace menureference
} // namespace kk
