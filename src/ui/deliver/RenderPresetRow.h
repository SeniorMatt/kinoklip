// SPDX-License-Identifier: GPL-3.0-or-later
// RenderPresetRow — the Deliver page's row of render preset tiles (icon above a short name),
// scrolled with arrow buttons like Resolve's preset row instead of a scroll bar.
#pragma once

#include <QIcon>
#include <QString>
#include <QWidget>

class QButtonGroup;
class QHBoxLayout;
class QScrollArea;
class QToolButton;

namespace kk {

/// A horizontal row of checkable preset tiles. Tiles have one fixed size; names that do not
/// fit on two lines are elided and the full name is in the tooltip. When the row is wider
/// than the panel, left/right arrow buttons appear at its ends (no scroll bar, which used to
/// eat half of the row's height and clip the labels).
///
/// Thread-safety: GUI thread only.
class RenderPresetRow : public QWidget
{
    Q_OBJECT
public:
    explicit RenderPresetRow(QWidget* parent = nullptr);

    /// Removes every tile.
    void clear();
    /// Adds a tile for preset @p id with @p name (shown, elided to two lines), @p icon and
    /// @p toolTip (description).
    void addPreset(
        const QString& id, const QString& name, const QIcon& icon, const QString& toolTip);
    /// Checks the tile of @p id (none for an unknown or empty id) and scrolls it into view.
    void setCurrent(const QString& id);
    /// Id of the checked tile, or empty.
    [[nodiscard]] QString current() const;
    /// Number of tiles.
    [[nodiscard]] int count() const;
    /// The tile of @p id (tests, screenshots), or nullptr.
    [[nodiscard]] QToolButton* tile(const QString& id) const;

signals:
    /// The user clicked the tile of @p id.
    void presetClicked(const QString& id);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void scrollBy(int direction);
    void updateArrows();

    QScrollArea* m_scroll = nullptr;
    QWidget* m_strip = nullptr;
    QHBoxLayout* m_stripLayout = nullptr;
    QButtonGroup* m_group = nullptr;
    QToolButton* m_left = nullptr;
    QToolButton* m_right = nullptr;
};

} // namespace kk
