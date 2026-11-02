// SPDX-License-Identifier: GPL-3.0-or-later
// ShortcutFallback — shifted-character fallback for the unshifted shortcuts of the keymap.
#include "ui/common/ShortcutFallback.h"

#include "ui/common/ActionRegistry.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QWidget>

namespace kk {

ShortcutFallback::ShortcutFallback(ActionRegistry& registry, QObject* parent)
    : QObject(parent)
    , m_registry(registry)
{
    qApp->installEventFilter(this);
}

ShortcutFallback::~ShortcutFallback()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

int ShortcutFallback::unshiftedKey(int key)
{
    switch (key) {
    case Qt::Key_BraceLeft:
        return Qt::Key_BracketLeft;
    case Qt::Key_BraceRight:
        return Qt::Key_BracketRight;
    case Qt::Key_Less:
        return Qt::Key_Comma;
    case Qt::Key_Greater:
        return Qt::Key_Period;
    case Qt::Key_Question:
        return Qt::Key_Slash;
    case Qt::Key_Colon:
        return Qt::Key_Semicolon;
    case Qt::Key_QuoteDbl:
        return Qt::Key_Apostrophe;
    case Qt::Key_Bar:
        return Qt::Key_Backslash;
    case Qt::Key_Plus:
        return Qt::Key_Equal;
    case Qt::Key_Underscore:
        return Qt::Key_Minus;
    case Qt::Key_AsciiTilde:
        return Qt::Key_QuoteLeft;
    case Qt::Key_Exclam:
        return Qt::Key_1;
    case Qt::Key_At:
        return Qt::Key_2;
    case Qt::Key_NumberSign:
        return Qt::Key_3;
    case Qt::Key_Dollar:
        return Qt::Key_4;
    case Qt::Key_Percent:
        return Qt::Key_5;
    case Qt::Key_AsciiCircum:
        return Qt::Key_6;
    case Qt::Key_Ampersand:
        return Qt::Key_7;
    case Qt::Key_Asterisk:
        return Qt::Key_8;
    case Qt::Key_ParenLeft:
        return Qt::Key_9;
    case Qt::Key_ParenRight:
        return Qt::Key_0;
    default:
        return 0;
    }
}

namespace {

/// True if @p action may fire from a key press in @p window (its shortcut context).
bool reachableFrom(const QAction* action, const QWidget* window)
{
    if (action->shortcutContext() == Qt::ApplicationShortcut)
        return true;
    for (const QObject* o : action->associatedObjects()) {
        if (const auto* w = qobject_cast<const QWidget*>(o); w && w->window() == window)
            return true;
    }
    return false;
}

} // namespace

bool ShortcutFallback::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::KeyPress)
        return false;
    auto* ke = static_cast<QKeyEvent*>(event);
    const int base = unshiftedKey(ke->key());
    if (base == 0 || ke->isAutoRepeat())
        return false;
    // Only the first delivery (to the focus widget), never into text input or popups.
    auto* target = qobject_cast<QWidget*>(watched);
    QWidget* focus = QApplication::focusWidget();
    const bool first = focus ? target == focus : target && target->isWindow();
    if (!target || !first || QApplication::activePopupWidget())
        return false;
    if (target->testAttribute(Qt::WA_InputMethodEnabled))
        return false;
    const Qt::KeyboardModifiers mods = (ke->modifiers() & ~Qt::KeypadModifier) | Qt::ShiftModifier;
    const QKeySequence seq(QKeyCombination(mods, Qt::Key(base)));
    const QString id = m_registry.actionForShortcut(seq);
    if (id.isEmpty())
        return false;
    QAction* action = m_registry.action(id);
    if (!action || !action->isEnabled() || !reachableFrom(action, target->window()))
        return false;
    action->trigger();
    return true;
}

} // namespace kk
