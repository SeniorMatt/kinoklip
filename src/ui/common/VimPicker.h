// SPDX-License-Identifier: GPL-3.0-or-later
// VimPicker — the Telescope-like picker of the Vim layer (spec section 83): a filter field over
// a list, opened by <leader>ff (media), fc (commands), fe (effects), ft (timelines), fm
// (markers), fd (bins), fh (help), fk (keymaps) and by `q:` (command history). The idea is
// telescope.nvim's (https://github.com/nvim-telescope/telescope.nvim, MIT); nothing is copied.
//
// The text typed is split at spaces; every word must match the item as a fuzzy subsequence
// (smart case: a capital letter makes that word case sensitive), and the best matches come
// first. <C-j> <C-n> Down move down, <C-k> <C-p> Up move up, Return chooses, Alt+Return chooses
// "show only" (an alternative the caller defines), Esc <C-c> <C-[> close.
#pragma once

#include <QFrame>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <functional>

class QLineEdit;
class QListWidget;
class QLabel;

namespace kk {

struct VimPickItem
{
    QString text; ///< what is matched and shown
    QString detail; ///< shown dimmed after the text, also matched ("Ctrl+S", "V1")
    QString id; ///< the caller's key
    QVariant data;
};

class VimPicker : public QFrame
{
    Q_OBJECT
public:
    /// @p window is the main window the picker floats over.
    explicit VimPicker(QWidget* window);

    /// Called with the chosen item and whether Alt was held. Not called on Esc.
    using Chosen = std::function<void(const VimPickItem& item, bool alternative)>;

    /// Opens the picker with @p items. The focus goes to its filter field and returns to the
    /// previous widget when it closes.
    void open(const QString& title, const QVector<VimPickItem>& items, Chosen chosen);
    void closePicker();
    [[nodiscard]] bool isOpen() const { return isVisible(); }

    [[nodiscard]] QString title() const;
    [[nodiscard]] QString filter() const;
    void setFilter(const QString& text);
    /// The texts shown now, best match first.
    [[nodiscard]] QStringList visibleTexts() const;
    [[nodiscard]] int currentRow() const;
    [[nodiscard]] QString currentText() const;
    /// Moves the selection by @p delta rows (wraps round, like telescope).
    void moveSelection(int delta);
    /// Chooses the current row; false when the list is empty.
    bool acceptCurrent(bool alternative = false);
    [[nodiscard]] QLineEdit* filterField() const { return m_field; }

    /// How well @p pattern matches @p text as a fuzzy subsequence: -1 for no match, else a
    /// score (higher is better; consecutive letters and word starts count more). Each
    /// space-separated word of the pattern must match; a word with a capital letter is case
    /// sensitive. An empty pattern matches with 0.
    [[nodiscard]] static int score(const QString& pattern, const QString& text);

signals:
    /// Emitted after the picker opened and after it closed (the window shows COMMAND meanwhile).
    void openedChanged(bool open);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refilter();
    void place();

    QWidget* m_window = nullptr;
    QLabel* m_title = nullptr;
    QLineEdit* m_field = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_count = nullptr;
    QVector<VimPickItem> m_items;
    QVector<int> m_shown; ///< indexes into m_items
    Chosen m_chosen;
    QPointer<QWidget> m_returnFocus;
};

} // namespace kk
