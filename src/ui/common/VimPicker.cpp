// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimPicker.h"

#include "ui/common/Metrics.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {

constexpr int kMaxShown = 400; ///< rows kept in the list (the filter narrows further)

/// The score of one word as a subsequence of @p text, -1 when it does not match. A greedy walk
/// that prefers the start of words and runs of consecutive letters.
int wordScore(const QString& word, const QString& text)
{
    if (word.isEmpty())
        return 0;
    bool caseSensitive = false;
    for (const QChar c : word)
        caseSensitive = caseSensitive || c.isUpper();
    const Qt::CaseSensitivity cs = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    // A plain substring is the best case.
    if (const qsizetype at = text.indexOf(word, 0, cs); at >= 0) {
        int score = 1000 - int(std::min<qsizetype>(at, 200)) - int(text.size() - word.size());
        if (at == 0 || !text.at(at - 1).isLetterOrNumber())
            score += 300;
        return score;
    }
    int score = 0;
    qsizetype pos = 0;
    qsizetype previous = -2;
    for (const QChar c : word) {
        qsizetype found = -1;
        for (qsizetype i = pos; i < text.size(); ++i) {
            const bool same = caseSensitive ? text.at(i) == c : text.at(i).toLower() == c.toLower();
            if (same) {
                found = i;
                break;
            }
        }
        if (found < 0)
            return -1;
        score += 10;
        if (found == previous + 1)
            score += 15;
        if (found == 0 || !text.at(found - 1).isLetterOrNumber())
            score += 20;
        previous = found;
        pos = found + 1;
    }
    return std::max(1, score - int(text.size() / 4));
}

} // namespace

int VimPicker::score(const QString& pattern, const QString& text)
{
    int total = 0;
    for (const QString& word : pattern.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const int s = wordScore(word, text);
        if (s < 0)
            return -1;
        total += s;
    }
    return total;
}

VimPicker::VimPicker(QWidget* window)
    : QFrame(window)
    , m_window(window)
{
    setObjectName(QStringLiteral("VimPicker"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::NoFocus);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM);
    m_title = new QLabel(this);
    m_title->setStyleSheet(QStringLiteral("QLabel { font-weight: 600; }"));
    m_field = new QLineEdit(this);
    m_field->setObjectName(QStringLiteral("VimPickerField"));
    m_field->setClearButtonEnabled(false);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("VimPickerList"));
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setUniformItemSizes(true);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_count = new QLabel(this);
    layout->addWidget(m_title);
    layout->addWidget(m_field);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_count);
    m_field->installEventFilter(this);
    connect(m_field, &QLineEdit::textChanged, this, [this] { refilter(); });
    connect(m_list, &QListWidget::itemActivated, this, [this] { acceptCurrent(false); });
    hide();
}

QString VimPicker::title() const
{
    return m_title->text();
}

QString VimPicker::filter() const
{
    return m_field->text();
}

void VimPicker::setFilter(const QString& text)
{
    m_field->setText(text);
}

QStringList VimPicker::visibleTexts() const
{
    QStringList texts;
    for (const int i : m_shown)
        texts.append(m_items[i].text);
    return texts;
}

int VimPicker::currentRow() const
{
    return m_list->currentRow();
}

QString VimPicker::currentText() const
{
    const int row = m_list->currentRow();
    return row >= 0 && row < m_shown.size() ? m_items[m_shown[row]].text : QString();
}

void VimPicker::open(const QString& title, const QVector<VimPickItem>& items, Chosen chosen)
{
    if (!isVisible())
        m_returnFocus = QApplication::focusWidget();
    m_title->setText(title);
    m_items = items;
    m_chosen = std::move(chosen);
    m_field->blockSignals(true);
    m_field->clear();
    m_field->blockSignals(false);
    refilter();
    place();
    raise();
    show();
    m_field->setFocus(Qt::OtherFocusReason);
    emit openedChanged(true);
}

void VimPicker::closePicker()
{
    if (!isVisible())
        return;
    hide();
    m_chosen = nullptr;
    if (m_returnFocus)
        m_returnFocus->setFocus(Qt::OtherFocusReason);
    m_returnFocus.clear();
    emit openedChanged(false);
}

void VimPicker::place()
{
    if (!m_window)
        return;
    const int width = std::min(m_window->width() - 2 * metrics::kSpaceL, 720);
    const int height = std::min(m_window->height() - 2 * metrics::kSpaceL, 420);
    resize(width, height);
    move((m_window->width() - width) / 2, std::max(metrics::kSpaceL, m_window->height() / 6));
}

void VimPicker::refilter()
{
    const QString pattern = m_field->text();
    QVector<QPair<int, int>> scored; // score, index
    scored.reserve(m_items.size());
    for (int i = 0; i < m_items.size(); ++i) {
        const VimPickItem& item = m_items[i];
        const int s = score(pattern,
            item.detail.isEmpty() ? item.text : item.text + QLatin1Char(' ') + item.detail);
        if (s >= 0)
            scored.append({s, i});
    }
    if (!pattern.trimmed().isEmpty()) {
        std::stable_sort(scored.begin(), scored.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
    }
    m_shown.clear();
    m_list->clear();
    for (const auto& pair : std::as_const(scored)) {
        if (m_shown.size() >= kMaxShown)
            break;
        const VimPickItem& item = m_items[pair.second];
        m_shown.append(pair.second);
        m_list->addItem(
            item.detail.isEmpty() ? item.text : item.text + QStringLiteral("    ") + item.detail);
    }
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
    m_count->setText(tr("%1 of %2").arg(m_shown.size()).arg(m_items.size()));
}

void VimPicker::moveSelection(int delta)
{
    const int n = m_list->count();
    if (n == 0)
        return;
    const int row = ((m_list->currentRow() + delta) % n + n) % n;
    m_list->setCurrentRow(row);
}

bool VimPicker::acceptCurrent(bool alternative)
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_shown.size())
        return false;
    const VimPickItem item = m_items[m_shown[row]];
    const Chosen chosen = m_chosen;
    closePicker();
    if (chosen)
        chosen(item, alternative);
    return true;
}

bool VimPicker::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_field
        && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        auto* key = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers mods = key->modifiers() & ~Qt::KeypadModifier;
        const bool ctrl = mods == Qt::ControlModifier;
        const int k = key->key();
        const bool ours = k == Qt::Key_Escape || k == Qt::Key_Down || k == Qt::Key_Up
            || k == Qt::Key_PageDown || k == Qt::Key_PageUp || k == Qt::Key_Backtab
            || k == Qt::Key_Return || k == Qt::Key_Enter
            || (k == Qt::Key_Tab && mods == Qt::NoModifier)
            || (ctrl
                && (k == Qt::Key_C || k == Qt::Key_BracketLeft || k == Qt::Key_J || k == Qt::Key_N
                    || k == Qt::Key_K || k == Qt::Key_P || k == Qt::Key_D || k == Qt::Key_U));
        if (event->type() == QEvent::ShortcutOverride) {
            if (ours) {
                event->accept(); // no window shortcut takes it; the key press follows
                return true;
            }
            return false;
        }
        if (k == Qt::Key_Escape || (ctrl && (k == Qt::Key_C || k == Qt::Key_BracketLeft))) {
            closePicker();
            return true;
        }
        if (k == Qt::Key_Down || (ctrl && (k == Qt::Key_J || k == Qt::Key_N))
            || (k == Qt::Key_Tab && mods == Qt::NoModifier)) {
            moveSelection(1);
            return true;
        }
        if (k == Qt::Key_Up || (ctrl && (k == Qt::Key_K || k == Qt::Key_P))
            || k == Qt::Key_Backtab) {
            moveSelection(-1);
            return true;
        }
        if (k == Qt::Key_PageDown || (ctrl && k == Qt::Key_D)) {
            moveSelection(
                std::max(1, m_list->height() / 2 / std::max(1, m_list->sizeHintForRow(0))));
            return true;
        }
        if (k == Qt::Key_PageUp || (ctrl && k == Qt::Key_U)) {
            moveSelection(
                -std::max(1, m_list->height() / 2 / std::max(1, m_list->sizeHintForRow(0))));
            return true;
        }
        if (k == Qt::Key_Return || k == Qt::Key_Enter) {
            acceptCurrent(mods == Qt::AltModifier);
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace kk
