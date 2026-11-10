// SPDX-License-Identifier: GPL-3.0-or-later
// MediaPoolDelegate — paints one media pool grid tile: poster frame (or a placeholder glyph),
// duration badge, offline marker and the name, at the grid's thumbnail size.
#pragma once

#include <QStyledItemDelegate>

namespace kk {

/// Grid tile painter for MediaPoolModel items. All tiles have the same size (sizeHint() depends
/// only on the poster size) so the view can use uniform item sizes; painting only blits the
/// cached, pre-scaled poster and draws a few rectangles and two text runs. At small poster
/// sizes the badges that no longer fit (duration, "Linked") are left out; the name stays.
class MediaPoolDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    /// Creates the delegate.
    explicit MediaPoolDelegate(QObject* parent = nullptr);

    /// The tile size for posters of @p poster size.
    [[nodiscard]] static QSize tileSize(QSize poster);
    /// The poster area of the tile at @p tile for posters of @p poster size (the hover preview
    /// maps the pointer's horizontal place over it to the clip's length).
    [[nodiscard]] static QRect posterRect(const QRect& tile, QSize poster);
    /// The name line of the tile at @p tile (a double-click there renames a folder).
    [[nodiscard]] static QRect nameRect(const QRect& tile);

    /// Paints posters of @p size (MediaPoolModel::setPosterSize()); the default is
    /// MediaPoolModel::kPosterSize.
    void setPosterSize(QSize size) { m_poster = size; }
    [[nodiscard]] QSize posterSize() const { return m_poster; }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(
        const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;

private:
    QSize m_poster;
};

} // namespace kk
