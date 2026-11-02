// SPDX-License-Identifier: GPL-3.0-or-later
// EditIndexFilter — row filtering of the Edit Index.
#include "ui/editindex/EditIndexFilter.h"

#include "ui/editindex/EditIndexModel.h"

namespace kk {

EditIndexFilter::EditIndexFilter(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setSortRole(EditIndexModel::SortRole);
}

void EditIndexFilter::setSearchText(const QString& text)
{
    if (text == m_text)
        return;
    m_text = text;
    refilter();
}

void EditIndexFilter::setMarkerColor(const QColor& color)
{
    if (color == m_color)
        return;
    m_color = color;
    refilter();
}

void EditIndexFilter::refilter()
{
    // Same compatibility switch as MediaPoolFilterModel::refilter(): Qt 6.12 deprecates
    // invalidateFilter(); the filter state is already updated, endFilterChange() re-evaluates.
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    beginFilterChange();
    endFilterChange();
#else
    invalidateFilter();
#endif
}

bool EditIndexFilter::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    const QAbstractItemModel* model = sourceModel();
    const auto* index = qobject_cast<const EditIndexModel*>(model);
    if (index && index->mode() == EditIndexModel::Mode::Markers && m_color.isValid()) {
        const QColor c = model->index(sourceRow, 0, sourceParent)
                             .data(EditIndexModel::MarkerColorRole)
                             .value<QColor>();
        if (c != m_color)
            return false;
    }
    if (m_text.isEmpty())
        return true;
    for (int column = 0; column < model->columnCount(sourceParent); ++column) {
        const QString cell = model->index(sourceRow, column, sourceParent).data().toString();
        if (cell.contains(m_text, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

} // namespace kk
