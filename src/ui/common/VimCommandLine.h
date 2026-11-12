// SPDX-License-Identifier: GPL-3.0-or-later
// VimCommandLine — the line edit of the `:` and `/` lines (spec section 83): Neovim's command-line
// editing (cmdline.txt) with history and completion. The text of the line is not interpreted
// here: the window gives a completer and runs the result.
//
// Keys: Return runs, Esc <C-c> <C-[> close, <BS> on an empty line closes. <Up> <Down> recall
// older / newer lines that start with the text typed so far, <C-p> <C-n> (and <C-k> <C-j>, the
// Telescope pair) recall without the prefix; while completion matches are listed <C-n> <C-p>
// step through them. <Tab> completes (a single match is filled in, several are extended to
// their common beginning and listed, then <Tab> / <S-Tab> step through them). <C-b> <C-e> go to
// the start / end, <C-h> <C-l> one character left / right (the owner's maps), <C-w> deletes the
// word before the cursor, <C-u> everything before it.
#pragma once

#include <QLineEdit>
#include <QStringList>

#include <functional>

namespace kk {

/// What completion offers for the text before the cursor: the part of the line from @p start
/// to the cursor is replaced by one of @p candidates.
struct VimCompletion
{
    int start = 0;
    QStringList candidates;
};

class VimCommandLine : public QLineEdit
{
    Q_OBJECT
public:
    using Completer = std::function<VimCompletion(const QString& text, int cursor)>;

    explicit VimCommandLine(QWidget* parent = nullptr);

    void setCompleter(Completer completer) { m_completer = std::move(completer); }

    /// The history lines, oldest first; the `:` and the `/` line keep their own.
    void setHistory(const QStringList& history) { m_history = history; }
    [[nodiscard]] QStringList history() const { return m_history; }
    /// Adds @p line as the newest entry (an older equal entry goes; blank lines are not kept).
    void addHistory(const QString& line);
    static constexpr int kMaxHistory = 200;

    /// Starts a new entry: empty text (or @p initial), history and completion reset.
    void begin(const QString& initial = QString());

    /// Tab: completes (see above). @p forward false steps back.
    void complete(bool forward);
    /// <Up> / <Down>: older / newer history line; @p matchPrefix recalls only lines that start
    /// with what was typed before the first step.
    void recall(bool older, bool matchPrefix);

    /// The matches listed now, and the one in the line (-1: none chosen yet).
    [[nodiscard]] QStringList candidates() const { return m_candidates; }
    [[nodiscard]] int candidateIndex() const { return m_candidateIndex; }

signals:
    /// The line is to be closed without running (Esc, <C-c>, <BS> on an empty line).
    void cancelled();
    /// The list of completion matches changed (empty = hide).
    void candidatesChanged(const QStringList& candidates, int index);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;
    /// Tab is a key of the line, not a move to the next widget.
    bool focusNextPrevChild(bool) override { return false; }

private:
    /// Whether the line uses this key itself (so no window shortcut may take it).
    [[nodiscard]] static bool ownsKey(const QKeyEvent* key);
    void setCandidates(const QStringList& list, int index);
    void resetCompletion();
    void resetHistoryWalk();

    Completer m_completer;
    QStringList m_history;
    int m_historyPos = -1; ///< index into the history while walking it, -1 = the typed text
    QString m_typed; ///< the text before the first history step
    QStringList m_candidates;
    int m_candidateIndex = -1;
    int m_completeStart = 0;
    QString m_completeBase; ///< the text before the cursor when completion started
    QString m_completeTail; ///< the text after the cursor
};

} // namespace kk
