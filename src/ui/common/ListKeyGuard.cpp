// SPDX-License-Identifier: GPL-3.0-or-later
// ListKeyGuard — list and tree panels keep their own editing and navigation keys.
#include "ui/common/ListKeyGuard.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QKeyEvent>

namespace kk {

ListKeyGuard::ListKeyGuard(QObject* parent)
    : QObject(parent)
{
    qApp->installEventFilter(this);
}

ListKeyGuard::~ListKeyGuard()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

bool ListKeyGuard::keyBelongsToList(const QWidget* focus, const QKeyEvent& key)
{
    // The focus widget is the view itself (its viewport never takes focus).
    const auto* view = qobject_cast<const QAbstractItemView*>(focus);
    if (!view || view->property("kkTimelineKeys").toBool())
        return false;
    const Qt::KeyboardModifiers mods = key.modifiers() & ~Qt::KeypadModifier;
    switch (key.key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        return (mods & ~Qt::ShiftModifier) == Qt::NoModifier;
    case Qt::Key_A:
        return mods == Qt::ControlModifier;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        // Plain, Shift (extend) and Ctrl (move the current item only), as item views use them.
        return (mods & ~(Qt::ShiftModifier | Qt::ControlModifier)) == Qt::NoModifier;
    default:
        return false;
    }
}

bool ListKeyGuard::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::ShortcutOverride)
        return false;
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || widget != QApplication::focusWidget())
        return false;
    if (keyBelongsToList(widget, *static_cast<QKeyEvent*>(event)))
        event->accept(); // the key press goes to the view, not to a window shortcut
    return false; // the view's own filters (media pool, Edit Index) still see the event
}

} // namespace kk
