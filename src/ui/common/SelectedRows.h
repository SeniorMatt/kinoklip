// SPDX-License-Identifier: GPL-3.0-or-later
// SelectedRows — the selected rows of a view, whatever its selection behaviour.
#pragma once

#include <QItemSelectionModel>
#include <QSet>

namespace kk {

/// One index per selected row (at @p column), in selection order. Unlike
/// QItemSelectionModel::selectedRows(), which only reports rows whose every column is selected,
/// this also works for views that select single cells: a QListView in icon or list mode over a
/// multi-column model selects only the cell it shows (owner report on 0.4.0: Media Pool >
/// Delete did nothing in the thumbnail view).
inline QModelIndexList selectedRowIndexes(const QItemSelectionModel* selection, int column = 0)
{
    QModelIndexList rows;
    if (!selection)
        return rows;
    QSet<QPersistentModelIndex> seen;
    for (const QModelIndex& index : selection->selectedIndexes()) {
        const QModelIndex cell = index.siblingAtColumn(column);
        if (!cell.isValid() || seen.contains(cell))
            continue;
        seen.insert(cell);
        rows.append(cell);
    }
    return rows;
}

} // namespace kk
