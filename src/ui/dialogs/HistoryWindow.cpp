// SPDX-License-Identifier: GPL-3.0-or-later
// HistoryWindow implementation.
#include "ui/dialogs/HistoryWindow.h"

#include "core/Project.h"
#include "ui/common/Metrics.h"
#include "ui/common/SchemeColors.h"

#include <QEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QUndoStack>
#include <QUndoView>
#include <QVBoxLayout>

namespace kk {

namespace {

/// Initial size of the window (px): a column of step names.
constexpr int kDefaultWidth = 320;
constexpr int kDefaultHeight = 420;

/// Draws the steps after the current one (the redo steps) dimmed, like a history list that
/// shows where "now" is.
class HistoryDelegate : public QStyledItemDelegate
{
public:
    HistoryDelegate(QUndoStack* stack, QObject* parent)
        : QStyledItemDelegate(parent)
        , m_stack(stack)
    { }

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        // Row 0 is the clean state; row n is the state after command n - 1.
        if (index.row() > m_stack->index())
            option->palette.setColor(QPalette::Text, schemeColors().textDisabled);
    }

private:
    QUndoStack* m_stack;
};

} // namespace

HistoryWindow::HistoryWindow(Project* project, QWidget* parent)
    : QDialog(parent, Qt::Tool)
    , m_project(project)
{
    setObjectName(QStringLiteral("HistoryWindow"));
    setWindowTitle(tr("History"));
    setModal(false);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceS, metrics::kSpaceS, metrics::kSpaceS, metrics::kSpaceS);
    m_view = new QUndoView(project->undoStack());
    m_view->setObjectName(QStringLiteral("HistoryList"));
    m_view->setEmptyLabel(tr("Original State"));
    m_view->setItemDelegate(new HistoryDelegate(project->undoStack(), m_view));
    m_view->setAlternatingRowColors(true);
    layout->addWidget(m_view);
    resize(kDefaultWidth, kDefaultHeight);
    applyColors();

    // Never move the stack under an open edit (Project::undo() refuses that too).
    m_view->viewport()->installEventFilter(this);
    m_view->installEventFilter(this);
    // The dimmed redo rows follow the current index.
    connect(project->undoStack(), &QUndoStack::indexChanged, m_view->viewport(),
        qOverload<>(&QWidget::update));
    connect(SchemeColorsNotifier::instance(), &SchemeColorsNotifier::changed, this,
        &HistoryWindow::applyColors);
}

bool HistoryWindow::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type t = event->type();
    if ((t == QEvent::MouseButtonPress || t == QEvent::MouseButtonDblClick || t == QEvent::KeyPress)
        && m_project->isEditing()) {
        event->accept();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

bool HistoryWindow::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        applyColors();
    return QDialog::event(event);
}

void HistoryWindow::applyColors()
{
    // The list is a plain view: the palette carries the scheme; the dimmed rows ask
    // schemeColors() when painted, so only a repaint is needed here.
    m_view->viewport()->update();
}

} // namespace kk
