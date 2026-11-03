// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectCardDelegate — paints a project (or folder) of the Project Manager as a card.
#pragma once

#include <QStyledItemDelegate>

namespace kk {

/// A card: the poster frame (16:9, letterboxed on the viewer surround colour) or a folder /
/// placeholder glyph, the name, the last modified date and "duration · resolution · fps".
/// Badges on the poster: "Open" for the project open in the editor, "Media offline" when media
/// files are missing; an unreadable project shows a warning and its error instead of a poster.
/// Colours from kk::schemeColors(); the card width is set by the window's size slider.
/// Inline rename (F2) puts the line edit over the name.
class ProjectCardDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ProjectCardDelegate(QObject* parent = nullptr);

    /// Card width in px (the poster is 16:9 inside it); clamped to kMinWidth..kMaxWidth.
    void setCardWidth(int width);
    [[nodiscard]] int cardWidth() const { return m_width; }
    /// Full card size for the current width (the view's grid uses it plus spacing).
    [[nodiscard]] QSize cardSize() const;

    static constexpr int kMinWidth = 120;
    static constexpr int kMaxWidth = 400;
    static constexpr int kDefaultWidth = 220;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    /// Fills the rename editor once: metadata or a poster arriving while the user types
    /// (dataChanged on the row) must not reset the text.
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    /// The same guard for any delegate (the list view's).
    static bool editorAlreadyFilled(QWidget* editor);
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override;

private:
    [[nodiscard]] QRect posterRect(const QRect& card) const;
    [[nodiscard]] QRect nameRect(const QRect& card, const QFontMetrics& fm) const;

    int m_width = kDefaultWidth;
};

} // namespace kk
