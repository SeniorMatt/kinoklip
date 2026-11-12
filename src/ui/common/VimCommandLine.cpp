// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimCommandLine.h"

#include <QKeyEvent>

#include <algorithm>

namespace kk {

namespace {

/// The longest text all of @p list start with.
QString commonPrefix(const QStringList& list)
{
    if (list.isEmpty())
        return {};
    QString prefix = list.first();
    for (const QString& s : list) {
        qsizetype n = 0;
        while (n < prefix.size() && n < s.size() && prefix.at(n) == s.at(n))
            ++n;
        prefix.truncate(n);
    }
    return prefix;
}

} // namespace

VimCommandLine::VimCommandLine(QWidget* parent)
    : QLineEdit(parent)
{
    connect(this, &QLineEdit::textEdited, this, [this] {
        resetCompletion();
        resetHistoryWalk();
    });
}

void VimCommandLine::addHistory(const QString& line)
{
    const QString text = line.trimmed();
    if (text.isEmpty())
        return;
    m_history.removeAll(text);
    m_history.append(text);
    while (m_history.size() > kMaxHistory)
        m_history.removeFirst();
}

void VimCommandLine::begin(const QString& initial)
{
    resetCompletion();
    resetHistoryWalk();
    setText(initial);
}

void VimCommandLine::resetCompletion()
{
    if (m_candidates.isEmpty() && m_candidateIndex < 0)
        return;
    m_candidates.clear();
    m_candidateIndex = -1;
    emit candidatesChanged({}, -1);
}

void VimCommandLine::resetHistoryWalk()
{
    m_historyPos = -1;
    m_typed.clear();
}

void VimCommandLine::setCandidates(const QStringList& list, int index)
{
    m_candidates = list;
    m_candidateIndex = index;
    emit candidatesChanged(list, index);
}

void VimCommandLine::complete(bool forward)
{
    if (!m_completer)
        return;
    if (m_candidates.size() > 1) {
        // Step through the listed matches.
        const int n = int(m_candidates.size());
        const int next = m_candidateIndex < 0
            ? (forward ? 0 : n - 1)
            : ((m_candidateIndex + (forward ? 1 : -1)) % n + n) % n;
        setText(m_completeBase.left(m_completeStart) + m_candidates[next] + m_completeTail);
        setCursorPosition(int(m_completeStart + m_candidates[next].size()));
        setCandidates(m_candidates, next);
        return;
    }
    const QString before = text().left(cursorPosition());
    const QString after = text().mid(cursorPosition());
    const VimCompletion found = m_completer(before, int(before.size()));
    if (found.candidates.isEmpty())
        return;
    m_completeBase = before;
    m_completeTail = after;
    m_completeStart = found.start;
    const QString typed = before.mid(found.start);
    if (found.candidates.size() == 1) {
        const QString only = found.candidates.first();
        setText(before.left(found.start) + only + after);
        setCursorPosition(int(found.start + only.size()));
        return;
    }
    // Several matches: extend to what they share, list them, the next Tab picks the first.
    const QString common = commonPrefix(found.candidates);
    if (common.size() > typed.size()) {
        setText(before.left(found.start) + common + after);
        setCursorPosition(int(found.start + common.size()));
        m_completeBase = text().left(cursorPosition());
        setCandidates(found.candidates, -1);
        return;
    }
    setCandidates(found.candidates, -1);
    complete(forward);
}

void VimCommandLine::recall(bool older, bool matchPrefix)
{
    if (m_history.isEmpty())
        return;
    if (m_historyPos < 0)
        m_typed = text();
    const QString prefix = matchPrefix ? m_typed : QString();
    int pos = m_historyPos < 0 ? int(m_history.size()) : m_historyPos;
    const int step = older ? -1 : 1;
    for (pos += step; pos >= 0 && pos < m_history.size(); pos += step) {
        if (m_history[pos].startsWith(prefix))
            break;
    }
    if (pos < 0)
        return; // already at the oldest match: stay
    m_historyPos = pos >= m_history.size() ? -1 : pos;
    const QString shown = m_historyPos < 0 ? m_typed : m_history[m_historyPos];
    setText(shown);
    setCursorPosition(int(shown.size()));
    resetCompletion();
}

bool VimCommandLine::ownsKey(const QKeyEvent* key)
{
    const Qt::KeyboardModifiers mods = key->modifiers() & ~Qt::KeypadModifier;
    switch (key->key()) {
    case Qt::Key_Escape:
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        return true;
    case Qt::Key_Backspace:
        return mods == Qt::NoModifier;
    case Qt::Key_B:
    case Qt::Key_E:
    case Qt::Key_H:
    case Qt::Key_L:
    case Qt::Key_W:
    case Qt::Key_U:
    case Qt::Key_N:
    case Qt::Key_P:
    case Qt::Key_J:
    case Qt::Key_K:
    case Qt::Key_C:
    case Qt::Key_BracketLeft:
        return mods == Qt::ControlModifier;
    default:
        return false;
    }
}

bool VimCommandLine::event(QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride && ownsKey(static_cast<QKeyEvent*>(event))) {
        event->accept(); // the key arrives as a key press; no window shortcut takes it
        return true;
    }
    return QLineEdit::event(event);
}

void VimCommandLine::keyPressEvent(QKeyEvent* event)
{
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    const bool ctrl = mods == Qt::ControlModifier;
    const int k = event->key();
    const bool completing = m_candidates.size() > 1;
    if (k == Qt::Key_Escape || (ctrl && (k == Qt::Key_C || k == Qt::Key_BracketLeft))) {
        emit cancelled();
        return;
    }
    if (k == Qt::Key_Backspace && mods == Qt::NoModifier && text().isEmpty()) {
        emit cancelled();
        return;
    }
    if (k == Qt::Key_Tab && mods == Qt::NoModifier) {
        complete(true);
        return;
    }
    if (k == Qt::Key_Backtab) {
        complete(false);
        return;
    }
    if (completing && ctrl && (k == Qt::Key_N || k == Qt::Key_P)) {
        complete(k == Qt::Key_N);
        return;
    }
    if (k == Qt::Key_Up && mods == Qt::NoModifier) {
        recall(true, true);
        return;
    }
    if (k == Qt::Key_Down && mods == Qt::NoModifier) {
        recall(false, true);
        return;
    }
    if ((k == Qt::Key_Up && mods == Qt::ShiftModifier) || k == Qt::Key_PageUp
        || (ctrl && (k == Qt::Key_P || k == Qt::Key_K))) {
        recall(true, false);
        return;
    }
    if ((k == Qt::Key_Down && mods == Qt::ShiftModifier) || k == Qt::Key_PageDown
        || (ctrl && (k == Qt::Key_N || k == Qt::Key_J))) {
        recall(false, false);
        return;
    }
    if (ctrl && k == Qt::Key_B) {
        setCursorPosition(0);
        return;
    }
    if (ctrl && k == Qt::Key_E) {
        setCursorPosition(int(text().size()));
        return;
    }
    if (ctrl && k == Qt::Key_H) {
        cursorBackward(false);
        return;
    }
    if (ctrl && k == Qt::Key_L) {
        cursorForward(false);
        return;
    }
    if (ctrl && k == Qt::Key_W) {
        // The word before the cursor and the white space in front of it.
        const int at = cursorPosition();
        int start = at;
        while (start > 0 && text().at(start - 1).isSpace())
            --start;
        while (start > 0 && !text().at(start - 1).isSpace())
            --start;
        const QString t = text();
        setText(t.left(start) + t.mid(at));
        setCursorPosition(start);
        resetCompletion();
        resetHistoryWalk();
        return;
    }
    if (ctrl && k == Qt::Key_U) {
        const QString t = text().mid(cursorPosition());
        setText(t);
        setCursorPosition(0);
        resetCompletion();
        resetHistoryWalk();
        return;
    }
    QLineEdit::keyPressEvent(event);
}

} // namespace kk
