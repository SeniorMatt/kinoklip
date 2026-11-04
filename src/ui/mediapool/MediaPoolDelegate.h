// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolDelegate — paints one media pool grid tile: poster frame (or a placeholder glyph),
// duration badge, offline marker and the name.
#pragma once

#include <QStyledItemDelegate>

namespace kk {

/// Grid tile painter for MediaPoolModel items. All tiles have the same size (sizeHint() is a
/// constant) so the view can use uniform item sizes; painting only blits the cached, pre-scaled
/// poster and draws a few rectangles and two text runs.
class MediaPoolDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    /// Creates the delegate.
    explicit MediaPoolDelegate(QObject* parent = nullptr);

    /// The fixed tile size.
    [[nodiscard]] static QSize tileSize();
    /// The name line of the tile at @p tile (a double-click there renames a folder).
    [[nodiscard]] static QRect nameRect(const QRect& tile);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(
        const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;
};

} // namespace kk
