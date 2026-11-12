// SPDX-License-Identifier: GPL-3.0-or-later
// VimFormHost — the keys of context "form" (spec section 83, 5.6): the Inspector, the Compositing
// inspector, the Deliver and project settings, any panel that is a column of labelled controls.
//
// The panel sets the property kkVimContext = "form" on itself. Its rows are found by walking the
// widgets: a widget with the property VimRowOps::kRowProperty marks a row that implements
// VimRowOps (an Inspector parameter, a section header); every other visible control (check box,
// combo box, spin box, slider, line edit, button, value field) not part of such a row is a row of
// its own, so any dialog-like panel works without code. Rows on one visual line are the items of
// that line: j / k change the line, h / l the item (a line with one item: h / l step its value).
//
// The cursor is virtual: the keyboard focus stays on the panel (so text fields do not catch
// j and k), and a frame is drawn round the row. A click on a control moves the cursor there.
#pragma once

#include "ui/common/VimPanelHost.h"

#include <QList>
#include <QPointer>
#include <QRect>
#include <QVariant>

class QWidget;

namespace kk {

class VimRowOps;
class VimCursorFrame;

class VimFormHost final : public VimPanelHost
{
public:
    explicit VimFormHost(ActionRegistry* actions);
    ~VimFormHost() override;

    /// One row: its anchor widget (the label of a parameter, the header of a section, or the
    /// control itself), the keyboard operations of the row (nullptr for a plain control) and the
    /// place it takes in the panel.
    struct Item
    {
        QPointer<QWidget> anchor;
        VimRowOps* ops = nullptr;
        QRect rect; ///< in the coordinates of the panel
    };

    /// Makes @p root (a widget with kkVimContext "form") the panel the keys work on.
    void setRoot(QWidget* root);
    [[nodiscard]] QWidget* root() const { return m_root; }
    /// The visible rows of the panel by visual line, top to bottom.
    [[nodiscard]] QList<QList<Item>> lines() const;
    /// The row the cursor is on; an empty item when the panel has no rows.
    [[nodiscard]] Item current() const;
    /// The name of the row for messages.
    [[nodiscard]] QString rowName(const Item& item) const;
    /// Moves the cursor to line @p line (clamped), item @p item of it, and shows it.
    void moveTo(int line, int item = 0);
    /// Where the cursor is: line and item (-1 without rows).
    [[nodiscard]] int cursorLine() const;
    [[nodiscard]] int cursorItem() const;
    /// Shows or hides the cursor frame (Vim mode on / off).
    void setCursorVisible(bool visible);

    void focusChanged(QWidget* focus) override;
    bool hostCommand(const QString& name, int count, bool hasCount) override;

private:
    void sync(QWidget* focus);
    void show();
    bool move(int lines);
    bool step(const Item& item, int steps, double scale);
    bool edit(const Item& item);
    bool activate(const Item& item);
    bool reset(const Item& item);
    bool yank(const Item& item);
    bool paste(const Item& item);
    bool fold(int mode);
    bool foldAll(int mode);
    bool nextTab(int delta);
    bool keyframe(const QString& what, const Item& item);

    QPointer<QWidget> m_root;
    QPointer<QWidget> m_cursor; ///< the anchor of the row the cursor is on
    QPointer<QWidget> m_lastFocus; ///< the focus the cursor was last synced to
    int m_line = 0; ///< fallback when the anchor is gone (a rebuilt panel)
    int m_item = 0;
    QVariant m_register; ///< `yy`
    QString m_registerName;
    QPointer<VimCursorFrame> m_frame; ///< a child of the row it frames: deleted with it
    bool m_visible = true;
};

} // namespace kk
