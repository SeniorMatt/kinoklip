// SPDX-License-Identifier: GPL-3.0-or-later
// VimListHost — the Vim grammar over any list, tree or grid (spec section 83): the Media Pool
// bins and clips, the Effects library, the Edit Index, the render queue, ... A panel needs no
// code of its own: the layer takes the QAbstractItemView that has the focus.
//
// The view is the "timeline" the grammar knows with one track: every visible row (a tree is
// flattened in the order it is drawn, closed branches skipped) is a clip one frame long, and
// the cursor frame is the current row. So `j k gg G {n}G <C-d> <C-u>` are the row motions
// (linewise: `dj` takes two rows), `V` is V-LINE, `3dd` takes three rows, and `d y x p` with
// their counts, registers and `.` come from the same machine. The operators are
// carried out with the keys the view already understands (Delete, F2, Ctrl+V), so a panel's own
// confirmations and undo steps stay in force.
#pragma once

#include "ui/common/VimHost.h"

#include <QAbstractItemView>
#include <QModelIndex>
#include <QPointer>
#include <QString>

#include <functional>

namespace kk {

class ActionRegistry;

class VimListHost final : public VimHost
{
public:
    explicit VimListHost(ActionRegistry* actions);

    /// The view the keys work on now (set by the layer when the focus moves).
    void setView(QAbstractItemView* view);
    [[nodiscard]] QAbstractItemView* view() const { return m_view; }

    /// Moves the current row of @p view by @p delta (clamped), whatever the view's own arrow
    /// keys do (a grid goes by item, not by screen row). Without a current row the first (or,
    /// for a negative @p delta, the last) is chosen.
    static void step(QAbstractItemView* view, int delta);
    /// The rows shown: indexes of column 0, a tree in drawing order.
    [[nodiscard]] QVector<QModelIndex> rows() const;
    /// The text of row @p row.
    [[nodiscard]] QString rowText(int row) const;
    [[nodiscard]] int currentRow() const;

    // ---- Commands for the keys of the list context ----

    /// `o` / <CR>: activates the current row (opens a bin, loads a clip, applies an effect).
    void open();
    /// `l`: expands a closed branch, else activates.
    void right();
    /// `h`: collapses an open branch, else moves to the parent row. False when the view has
    /// no branches (the caller may go up a level of its own, like the media pool's bins).
    bool left();
    /// `-`: the parent row (a tree), else the view's own "up" key.
    bool parentRow();
    /// `za zo zc zR zM`: toggle / open / close the current branch; open / close all.
    enum class Fold {
        Toggle,
        Open,
        Close,
        OpenAll,
        CloseAll
    };
    void fold(Fold what);
    /// `zt zz zb`: scrolls the current row to the top / middle / bottom.
    void scrollTo(int where);
    /// `r`: starts renaming the current row (the view's edit key, F2).
    void rename();

    /// `/pattern` in a view without a search field: remembers it and moves to the first match
    /// after the current row. False when no row matches.
    bool search(const QString& pattern, bool forward = true);
    [[nodiscard]] QString pattern() const { return m_pattern; }
    void clearSearch() { m_pattern.clear(); }

    // ---- VimHost ----

    bool trigger(const QString& actionId) override;
    [[nodiscard]] int64_t playhead() const override;
    void setPlayhead(int64_t frame) override;
    void clearSelection() override;
    void openCommandLine() override;
    void openSearch() override;
    void runEx(const QString& line) override;
    [[nodiscard]] QVector<VimClipSpan> clipSpans(int track) const override;
    [[nodiscard]] QString clipName(quint64 id) const override;
    bool searchFor(const QString& pattern, bool forward) override { return search(pattern, forward); }
    [[nodiscard]] int64_t timelineEnd() const override;
    [[nodiscard]] QVector<quint64> selectedClips() const override;
    [[nodiscard]] int pageRows() const override;
    void focusChanged(QWidget* focus) override;
    /// The `list_*` commands: open, right, left, parent, new, rename, fold_*, scroll_*.
    bool hostCommand(const QString& name, int count, bool hasCount) override;
    bool operate(const VimOperation& operation) override;
    bool pasteRegister(QChar reg, int count, bool insert) override;
    void showVisual(const VimVisual& visual) override;
    void showMessage(const QString& text) override;
    bool searchStep(bool forward, int count) override;

    /// Set by the window: `:`, `/`, an ex line bound to a key, a message for the mode line.
    std::function<void()> onCommandLine;
    std::function<void()> onSearch;
    std::function<void(const QString&)> onEx;
    std::function<void(const QString&)> onMessage;
    /// `h` and `-` on a row with no parent: go up a level of the panel's own (the media pool's
    /// bin); true when it did.
    std::function<bool()> onUpLevel;
    /// `a`: add a new item (a bin); true when the panel could.
    std::function<bool()> onNewItem;

private:
    void sendKey(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void selectRows(int first, int last);

    ActionRegistry* m_actions;
    QPointer<QAbstractItemView> m_view;
    QString m_pattern;
    mutable QVector<QModelIndex> m_rows; ///< cached by rows(); rebuilt each call
};

} // namespace kk
