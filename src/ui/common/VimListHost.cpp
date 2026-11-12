// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimListHost.h"

#include "ui/common/ActionRegistry.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QListView>
#include <QListWidget>
#include <QMetaMethod>
#include <QScrollBar>
#include <QTableWidget>
#include <QTreeView>
#include <QTreeWidget>

#include <algorithm>

namespace kk {

namespace {

/// QObject::isSignalConnected() is protected; this reaches it through a derived type.
struct SignalPeek : QObject
{
    static bool connected(const QObject* object, const QMetaMethod& signal)
    {
        return (object->*(&SignalPeek::isSignalConnected))(signal);
    }
};

/// The rows of a tree under @p parent in drawing order.
void collect(const QAbstractItemView* view, const QTreeView* tree, const QModelIndex& parent,
    QVector<QModelIndex>& out)
{
    const QAbstractItemModel* model = view->model();
    const int n = model->rowCount(parent);
    for (int r = 0; r < n; ++r) {
        const QModelIndex index = model->index(r, 0, parent);
        if (tree && tree->isRowHidden(r, parent))
            continue;
        if (!tree && qobject_cast<const QListView*>(view)
            && static_cast<const QListView*>(view)->isRowHidden(r))
            continue;
        out.append(index);
        if (tree && model->hasChildren(index) && tree->isExpanded(index))
            collect(view, tree, index, out);
    }
}

} // namespace

VimListHost::VimListHost(ActionRegistry* actions)
    : m_actions(actions)
{ }

void VimListHost::setView(QAbstractItemView* view)
{
    m_view = view;
}

void VimListHost::step(QAbstractItemView* view, int delta)
{
    if (!view)
        return;
    VimListHost host(nullptr);
    host.setView(view);
    const int n = int(host.rows().size());
    if (n == 0)
        return;
    const int target
        = view->currentIndex().isValid() ? host.currentRow() + delta : (delta >= 0 ? 0 : n - 1);
    host.setPlayhead(std::clamp(target, 0, n - 1));
}

QVector<QModelIndex> VimListHost::rows() const
{
    QVector<QModelIndex> out;
    if (!m_view || !m_view->model())
        return out;
    if (const auto* tree = qobject_cast<const QTreeView*>(m_view.data())) {
        collect(m_view, tree, tree->rootIndex(), out);
    } else if (qobject_cast<const QListView*>(m_view.data())) {
        collect(m_view, nullptr, m_view->rootIndex(), out);
    } else {
        // A table or other view: the rows of column 0.
        const int n = m_view->model()->rowCount(m_view->rootIndex());
        for (int r = 0; r < n; ++r)
            out.append(m_view->model()->index(r, 0, m_view->rootIndex()));
    }
    return out;
}

QString VimListHost::rowText(int row) const
{
    const QVector<QModelIndex> list = rows();
    if (row < 0 || row >= list.size())
        return {};
    return list[row].data(Qt::DisplayRole).toString();
}

int VimListHost::currentRow() const
{
    if (!m_view)
        return 0;
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid())
        return 0;
    const QVector<QModelIndex> list = rows();
    const QModelIndex first = current.siblingAtColumn(0);
    for (int i = 0; i < list.size(); ++i) {
        if (list[i] == first)
            return i;
    }
    return 0;
}

void VimListHost::sendKey(int key, Qt::KeyboardModifiers modifiers)
{
    if (!m_view)
        return;
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(m_view, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(m_view, &release);
}

// ---- Commands ----

void VimListHost::open()
{
    if (!m_view)
        return;
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid())
        return;
    // The panels of the program open on a double click; a view that listens for "activated"
    // (the platform's Enter) gets that instead.
    bool wantsActivated = false;
    if (qobject_cast<QTreeWidget*>(m_view)) // these connect `activated` themselves
        wantsActivated
            = SignalPeek::connected(m_view, QMetaMethod::fromSignal(&QTreeWidget::itemActivated));
    else if (qobject_cast<QListWidget*>(m_view))
        wantsActivated
            = SignalPeek::connected(m_view, QMetaMethod::fromSignal(&QListWidget::itemActivated));
    else if (qobject_cast<QTableWidget*>(m_view))
        wantsActivated
            = SignalPeek::connected(m_view, QMetaMethod::fromSignal(&QTableWidget::itemActivated));
    else
        wantsActivated
            = SignalPeek::connected(m_view, QMetaMethod::fromSignal(&QAbstractItemView::activated));
    if (wantsActivated)
        emit m_view->activated(current);
    else
        emit m_view->doubleClicked(current);
}

void VimListHost::right()
{
    if (!m_view)
        return;
    if (auto* tree = qobject_cast<QTreeView*>(m_view.data())) {
        const QModelIndex current = tree->currentIndex();
        if (current.isValid() && tree->model()->hasChildren(current)
            && !tree->isExpanded(current)) {
            tree->expand(current);
            return;
        }
    }
    open();
}

bool VimListHost::left()
{
    auto* tree = qobject_cast<QTreeView*>(m_view.data());
    if (!tree)
        return false;
    const QModelIndex current = tree->currentIndex();
    if (current.isValid() && tree->isExpanded(current)) {
        tree->collapse(current);
        return true;
    }
    return parentRow();
}

void VimListHost::focusChanged(QWidget* focus)
{
    for (QWidget* w = focus; w; w = w->parentWidget()) {
        if (auto* view = qobject_cast<QAbstractItemView*>(w)) {
            m_view = view;
            return;
        }
    }
}

bool VimListHost::parentRow()
{
    if (!m_view)
        return false;
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid())
        return false;
    const QModelIndex parent = current.parent();
    if (!parent.isValid() || parent == m_view->rootIndex())
        return false;
    m_view->setCurrentIndex(parent);
    return true;
}

void VimListHost::fold(Fold what)
{
    auto* tree = qobject_cast<QTreeView*>(m_view.data());
    if (!tree)
        return;
    if (what == Fold::OpenAll) {
        tree->expandAll();
        return;
    }
    if (what == Fold::CloseAll) {
        tree->collapseAll();
        return;
    }
    const QModelIndex current = tree->currentIndex();
    if (!current.isValid())
        return;
    const bool open = tree->isExpanded(current);
    if (what == Fold::Open || (what == Fold::Toggle && !open))
        tree->expand(current);
    else
        tree->collapse(current);
}

void VimListHost::scrollTo(int where)
{
    if (!m_view)
        return;
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid())
        return;
    m_view->scrollTo(current,
        where < 0       ? QAbstractItemView::PositionAtTop
            : where > 0 ? QAbstractItemView::PositionAtBottom
                        : QAbstractItemView::PositionAtCenter);
}

void VimListHost::rename()
{
    sendKey(Qt::Key_F2);
}

bool VimListHost::hostCommand(const QString& name, int, bool)
{
    if (!name.startsWith(QLatin1String("list_")) || !m_view)
        return name.startsWith(QLatin1String("list_"));
    if (name == QLatin1String("list_open")) {
        open();
    } else if (name == QLatin1String("list_right")) {
        right();
    } else if (name == QLatin1String("list_left")) {
        if (!left() && onUpLevel)
            onUpLevel();
    } else if (name == QLatin1String("list_parent")) {
        if (!parentRow() && onUpLevel)
            onUpLevel();
    } else if (name == QLatin1String("list_new")) {
        if (!onNewItem || !onNewItem())
            showMessage(QObject::tr("Nothing to add here"));
    } else if (name == QLatin1String("list_rename")) {
        rename();
    } else if (name == QLatin1String("list_fold_toggle")) {
        fold(Fold::Toggle);
    } else if (name == QLatin1String("list_fold_open")) {
        fold(Fold::Open);
    } else if (name == QLatin1String("list_fold_close")) {
        fold(Fold::Close);
    } else if (name == QLatin1String("list_fold_open_all")) {
        fold(Fold::OpenAll);
    } else if (name == QLatin1String("list_fold_close_all")) {
        fold(Fold::CloseAll);
    } else if (name == QLatin1String("list_scroll_top")) {
        scrollTo(-1);
    } else if (name == QLatin1String("list_scroll_middle")) {
        scrollTo(0);
    } else if (name == QLatin1String("list_scroll_bottom")) {
        scrollTo(1);
    } else {
        return false;
    }
    return true;
}

// ---- Search ----

bool VimListHost::search(const QString& pattern, bool forward)
{
    m_pattern = pattern.trimmed();
    return !m_pattern.isEmpty() && searchStep(forward, 1);
}

QString VimListHost::clipName(quint64 id) const
{
    const QVector<QModelIndex> list = rows();
    if (id == 0 || id > quint64(list.size()))
        return {};
    return list[int(id) - 1].data(Qt::DisplayRole).toString();
}

bool VimListHost::searchStep(bool forward, int count)
{
    const QVector<QModelIndex> list = rows();
    if (list.isEmpty())
        return false;
    const int n = int(list.size());
    int at = currentRow();
    for (int i = 0; i < std::max(1, count); ++i) {
        bool found = false;
        for (int k = 1; k <= n; ++k) {
            const int row = ((at + (forward ? k : -k)) % n + n) % n; // wraps round
            if (m_pattern.isEmpty()
                || list[row]
                       .data(Qt::DisplayRole)
                       .toString()
                       .contains(m_pattern, Qt::CaseInsensitive)) {
                at = row;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    setPlayhead(at);
    if (!m_pattern.isEmpty())
        showMessage(QObject::tr("/%1  row %2 of %3").arg(m_pattern).arg(at + 1).arg(n));
    return true;
}

// ---- VimHost ----

bool VimListHost::trigger(const QString& actionId)
{
    if (!m_actions || !m_actions->contains(actionId))
        return false;
    QAction* a = m_actions->action(actionId);
    if (!a->isEnabled())
        return false;
    a->trigger();
    return true;
}

void VimListHost::clearSelection()
{
    if (!m_view || !m_view->selectionModel())
        return;
    const QModelIndex current = m_view->currentIndex();
    m_view->selectionModel()->clearSelection();
    if (current.isValid())
        m_view->selectionModel()->select(
            current, QItemSelectionModel::Select | QItemSelectionModel::Rows);
}

void VimListHost::openCommandLine()
{
    if (onCommandLine)
        onCommandLine();
}

void VimListHost::openSearch()
{
    if (onSearch)
        onSearch();
}

void VimListHost::runEx(const QString& line)
{
    if (onEx)
        onEx(line);
}

void VimListHost::showMessage(const QString& text)
{
    if (onMessage)
        onMessage(text);
}

int64_t VimListHost::playhead() const
{
    return currentRow();
}

void VimListHost::setPlayhead(int64_t frame)
{
    const QVector<QModelIndex> list = rows();
    if (list.isEmpty() || !m_view)
        return;
    const QModelIndex target = list[std::clamp<int64_t>(frame, 0, list.size() - 1)];
    // The cursor moves and the selection is that row (a VISUAL selection is set by the layer
    // with showVisual() after every move). Not QAbstractItemView::setCurrentIndex(): it reads
    // the keyboard modifiers, and Ctrl (as in <C-j>) would toggle instead of select.
    if (QItemSelectionModel* selection = m_view->selectionModel())
        selection->setCurrentIndex(
            target, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    else
        m_view->setCurrentIndex(target);
    m_view->scrollTo(target);
}

int64_t VimListHost::timelineEnd() const
{
    return int64_t(rows().size());
}

QVector<VimClipSpan> VimListHost::clipSpans(int track) const
{
    // One track; every row is a clip of one frame, its id the row number + 1.
    QVector<VimClipSpan> spans;
    if (track != 0)
        return spans;
    const int n = int(rows().size());
    spans.reserve(n);
    for (int i = 0; i < n; ++i)
        spans.append({quint64(i) + 1, i, i + 1});
    return spans;
}

QVector<quint64> VimListHost::selectedClips() const
{
    QVector<quint64> ids;
    if (!m_view || !m_view->selectionModel())
        return ids;
    const QVector<QModelIndex> list = rows();
    for (int i = 0; i < list.size(); ++i) {
        if (m_view->selectionModel()->isSelected(list[i]))
            ids.append(quint64(i) + 1);
    }
    return ids;
}

int VimListHost::pageRows() const
{
    if (!m_view)
        return 10;
    const QVector<QModelIndex> list = rows();
    if (list.isEmpty())
        return 10;
    const int rowHeight = std::max(1, m_view->visualRect(list.first()).height());
    return std::max(1, m_view->viewport()->height() / rowHeight);
}

void VimListHost::selectRows(int first, int last)
{
    if (!m_view || !m_view->selectionModel())
        return;
    const QVector<QModelIndex> list = rows();
    if (list.isEmpty())
        return;
    first = std::clamp(first, 0, int(list.size()) - 1);
    last = std::clamp(last, 0, int(list.size()) - 1);
    if (first > last)
        std::swap(first, last);
    QItemSelection selection;
    for (int i = first; i <= last; ++i)
        selection.select(list[i], list[i]);
    m_view->selectionModel()->select(
        selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

void VimListHost::showVisual(const VimVisual& visual)
{
    if (visual.kind == VimVisual::Kind::None)
        return;
    selectRows(int(visual.anchor.frame), int(visual.cursor.frame));
}

bool VimListHost::operate(const VimOperation& operation)
{
    if (!m_view)
        return false;
    // The rows of the range, as row numbers.
    QVector<int> picked;
    if (operation.range.kind == VimRange::Kind::Clips) {
        for (const quint64 id : operation.range.clips)
            picked.append(int(id) - 1);
    } else {
        for (int64_t r = operation.range.start; r < operation.range.end; ++r)
            picked.append(int(r));
    }
    std::sort(picked.begin(), picked.end());
    picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
    if (picked.isEmpty())
        return false;
    const QVector<QModelIndex> list = rows();
    if (picked.first() < 0 || picked.last() >= list.size())
        return false;
    selectRows(picked.first(), picked.last());
    // Not all rows of a span need be picked (clips of a register): keep to the span.
    if (operation.op == VimOp::Yank) {
        QStringList texts;
        for (const int row : std::as_const(picked))
            texts.append(list[row].data(Qt::DisplayRole).toString());
        QGuiApplication::clipboard()->setText(texts.join(QLatin1Char('\n')));
        showMessage(QObject::tr("%1 yanked")
                        .arg(picked.size() == 1 ? texts.first()
                                                : QObject::tr("%1 rows").arg(picked.size())));
        return true;
    }
    if (operation.op == VimOp::Change) {
        m_view->setCurrentIndex(list[picked.first()]);
        sendKey(Qt::Key_F2);
        return true;
    }
    // Delete and Lift: the view's own Delete key (it asks first where the panel must).
    sendKey(Qt::Key_Delete);
    return true;
}

bool VimListHost::pasteRegister(QChar, int, bool)
{
    // Ctrl+V: the panel pastes what is on the system clipboard (the media pool imports files).
    sendKey(Qt::Key_V, Qt::ControlModifier);
    return true;
}

} // namespace kk
