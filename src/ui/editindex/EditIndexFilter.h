// SPDX-License-Identifier: GPL-3.0-or-later
// EditIndexFilter — search text and marker colour filter of the Edit Index table.
#pragma once

#include <QColor>
#include <QSortFilterProxyModel>

namespace kk {

/// Proxy over EditIndexModel: keeps rows whose any column contains the search text (case
/// insensitive) and, in Markers mode, whose marker colour is the chosen one. Sorts by
/// EditIndexModel::SortRole so timecodes and numbers sort numerically.
class EditIndexFilter : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    /// Creates an empty filter (every row passes).
    explicit EditIndexFilter(QObject* parent = nullptr);

    /// Rows must contain @p text in some column (empty = no text filter).
    void setSearchText(const QString& text);
    /// Marker rows must have colour @p color (invalid = every colour). Ignored for events.
    void setMarkerColor(const QColor& color);
    [[nodiscard]] QColor markerColor() const { return m_color; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    void refilter();

    QString m_text;
    QColor m_color;
};

} // namespace kk
