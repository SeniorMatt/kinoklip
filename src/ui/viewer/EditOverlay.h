// SPDX-License-Identifier: GPL-3.0-or-later
// EditOverlay — the edit types shown over the timeline viewer while a clip is dragged onto it
// (spec 4.14): insert, overwrite, replace, fit to fill, place on top, append at end, ripple
// overwrite. Dropping on a tile performs that edit.
#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

namespace kk {

class ViewerSurface;

/// Transparent, input-free child of a viewer surface: draws the tiles; the viewer panel feeds
/// it the drag position (setHover()) and asks tileAt() on drop.
///
/// Tiles are data (tiles()): an id (the edit command's ActionRegistry id, e.g. "edit.insert"),
/// the label and the key shown in the tile. Layout: two rows over the whole viewer (four
/// tiles, then three), the hovered tile highlighted.
///
/// Thread-safety: GUI thread only.
class EditOverlay : public QWidget
{
    Q_OBJECT
public:
    /// One tile.
    struct Tile
    {
        QString actionId; ///< edit command, e.g. "edit.overwrite"
        QString label; ///< "Overwrite"
        QString key; ///< shortcut text shown under the label ("F10"), may be empty
    };

    explicit EditOverlay(ViewerSurface* surface);

    /// Replaces the tiles (in reading order).
    void setTiles(const QVector<Tile>& tiles);
    [[nodiscard]] const QVector<Tile>& tiles() const { return m_tiles; }
    /// Highlights the tile under widget position @p pos (none if outside every tile).
    void setHover(const QPoint& pos);
    /// Index of the highlighted tile, or -1.
    [[nodiscard]] int hovered() const { return m_hover; }
    /// Index of the tile at widget position @p pos, or -1.
    [[nodiscard]] int tileAt(const QPoint& pos) const;
    /// Rectangle of tile @p index (widget pixels).
    [[nodiscard]] QRect tileRect(int index) const;
    /// Follows the surface size.
    void refresh();

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    ViewerSurface* m_surface;
    QVector<Tile> m_tiles;
    int m_hover = -1;
};

} // namespace kk
