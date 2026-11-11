// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimInput.h"

#include "ui/common/ActionRegistry.h"
#include "ui/common/VimOptions.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextEdit>
#include <QTimer>

#include <algorithm>

namespace kk {

namespace {

constexpr int kMaxCount = 999;

bool isPrintableKey(const QString& text)
{
    return text.size() == 1 && text.at(0).isPrint() && !text.at(0).isSpace();
}

} // namespace

// ---- VimMachine ----------------------------------------------------------------------

VimMachine::VimMachine(QObject* parent)
    : QObject(parent)
{
}

QString VimMachine::modeName(Mode mode)
{
    switch (mode) {
    case Mode::Visual:
        return tr("VISUAL");
    case Mode::Insert:
        return tr("INSERT");
    case Mode::Command:
        return tr("COMMAND");
    case Mode::Normal:
        break;
    }
    return tr("NORMAL");
}

QString VimMachine::statusText() const
{
    const QString name = modeName(m_mode);
    return m_pending.isEmpty() ? name : name + QStringLiteral("  ") + m_pending;
}

void VimMachine::enterInsert()
{
    const bool wasVisual = m_mode == Mode::Visual;
    cancelPending();
    if (wasVisual && m_host)
        m_host->clearSelection();
    m_mode = Mode::Insert;
    emit statusChanged();
}

void VimMachine::setCommandMode(bool open)
{
    if (open == (m_mode == Mode::Command))
        return;
    if (open) {
        cancelPending();
        m_mode = Mode::Command;
    } else {
        m_mode = Mode::Normal;
    }
    emit statusChanged();
}

bool VimMachine::wouldConsume(int key, Qt::KeyboardModifiers modifiers, const QString& text) const
{
    if (!m_host)
        return false;
    // The command line owns its keys.
    if (m_mode == Mode::Command)
        return false;
    const bool escape = key == Qt::Key_Escape
        || (modifiers == Qt::ControlModifier && key == Qt::Key_BracketLeft)
        || (modifiers == Qt::ControlModifier && key == Qt::Key_C && busy());
    if (escape)
        return m_mode == Mode::Visual || m_mode == Mode::Insert || !m_pending.isEmpty();
    if (m_mode == Mode::Insert)
        return false;
    if (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return false;
    return isPrintableKey(text);
}

bool VimMachine::handleKey(int key, Qt::KeyboardModifiers modifiers, const QString& text)
{
    if (!wouldConsume(key, modifiers, text))
        return false;
    const bool escape = key == Qt::Key_Escape || key == Qt::Key_BracketLeft || key == Qt::Key_C;
    if (escape && (key == Qt::Key_Escape || modifiers & Qt::ControlModifier)) {
        // One rule, innermost first: pending keys, then VISUAL, then INSERT.
        if (!m_pending.isEmpty()) {
            cancelPending();
        } else if (m_mode == Mode::Visual) {
            leaveVisual(true);
        } else if (m_mode == Mode::Insert) {
            m_mode = Mode::Normal;
            emit statusChanged();
        }
        return true;
    }
    const QChar c = text.at(0);
    if (!m_first.isNull()) {
        const QChar first = m_first;
        m_first = QChar();
        second(first, c);
        return true;
    }
    if ((c >= QLatin1Char('1') && c <= QLatin1Char('9')) || (c == QLatin1Char('0') && m_count > 0)) {
        m_count = std::min(kMaxCount, m_count * 10 + (c.unicode() - '0'));
        m_pending += c;
        emit statusChanged();
        return true;
    }
    dispatch(c);
    return true;
}

void VimMachine::finish()
{
    m_count = 0;
    m_first = QChar();
    m_pending.clear();
    emit statusChanged();
}

void VimMachine::cancelPending()
{
    if (m_count == 0 && m_pending.isEmpty() && m_first.isNull())
        return;
    finish();
}

void VimMachine::reset()
{
    const bool wasVisual = m_mode == Mode::Visual;
    const bool wasInsert = m_mode == Mode::Insert;
    m_marks.clear();
    cancelPending();
    if (wasVisual)
        leaveVisual(true);
    if (wasInsert) {
        m_mode = Mode::Normal;
        emit statusChanged();
    }
}

void VimMachine::repeat(const char* actionId, int times)
{
    for (int i = 0; i < times; ++i) {
        if (!m_host->trigger(QLatin1String(actionId)))
            break;
    }
}

void VimMachine::enterVisual()
{
    m_mode = Mode::Visual;
    m_anchor = m_host->playhead();
    m_host->selectRange(m_anchor, m_anchor);
}

void VimMachine::leaveVisual(bool clearSelection)
{
    m_mode = Mode::Normal;
    if (clearSelection)
        m_host->clearSelection();
    emit statusChanged();
}

void VimMachine::afterMotion()
{
    if (m_mode == Mode::Visual)
        m_host->selectRange(m_anchor, m_host->playhead());
}

/// Motions: the same in NORMAL and VISUAL (VISUAL extends the range after each).
bool VimMachine::motion(QChar c)
{
    switch (c.unicode()) {
    case 'h':
        repeat("transport.prev_frame", count());
        break;
    case 'l':
        repeat("transport.next_frame", count());
        break;
    case 'H':
        repeat("transport.back_1s", count());
        break;
    case 'L':
        repeat("transport.forward_1s", count());
        break;
    case 'w':
        repeat("transport.next_edit", count());
        break;
    case 'b':
        repeat("transport.prev_edit", count());
        break;
    case '0':
        m_host->trigger(QStringLiteral("transport.go_to_start"));
        break;
    case '$':
    case 'G':
        m_host->trigger(QStringLiteral("transport.go_to_end"));
        break;
    case 'n':
        repeat("transport.next_marker", count());
        break;
    case 'N':
        repeat("transport.prev_marker", count());
        break;
    default:
        return false;
    }
    afterMotion();
    return true;
}

void VimMachine::dispatch(QChar c)
{
    // Keys that wait for a second key.
    if (QStringLiteral("gdyzm'`").contains(c)) {
        const bool visualOperator = m_mode == Mode::Visual && (c == QLatin1Char('d') || c == QLatin1Char('y'));
        if (!visualOperator) {
            m_first = c;
            m_pending += c;
            emit statusChanged();
            return;
        }
    }
    const int times = count();
    if (motion(c)) {
        // moved
    } else if (m_mode == Mode::Visual) {
        switch (c.unicode()) {
        case 'v':
            leaveVisual(true);
            break;
        case 'd':
        case 'x':
            m_host->trigger(QStringLiteral("edit.ripple_delete"));
            leaveVisual(false);
            break;
        case 'X':
            m_host->trigger(QStringLiteral("edit.delete"));
            leaveVisual(false);
            break;
        case 'y':
            m_host->trigger(QStringLiteral("edit.copy"));
            leaveVisual(true);
            break;
        default:
            break; // any other key does nothing in VISUAL, like an unbound key in vim
        }
    } else {
        switch (c.unicode()) {
        case 'j':
            m_host->moveTarget(-times);
            break;
        case 'k':
            m_host->moveTarget(times);
            break;
        case 'x':
            m_host->trigger(QStringLiteral("edit.ripple_delete"));
            break;
        case 'X':
            m_host->trigger(QStringLiteral("edit.delete"));
            break;
        case 'p':
            m_host->trigger(QStringLiteral("edit.paste_insert"));
            break;
        case 'P':
            m_host->trigger(QStringLiteral("edit.paste"));
            break;
        case 'u':
            repeat("edit.undo", times);
            break;
        case 'i':
        case 'a':
            enterInsert(); // typing mode: the preset's own keys work (Esc comes back)
            return;
        case 's':
            m_host->trigger(QStringLiteral("edit.split"));
            break;
        case 'M':
            m_host->trigger(QStringLiteral("mark.add_marker"));
            break;
        case '>':
            repeat("trim.nudge_right", times);
            break;
        case '<':
            repeat("trim.nudge_left", times);
            break;
        case 'v':
            enterVisual();
            break;
        case ':':
            m_host->openCommandLine();
            break;
        case '/':
            m_host->openSearch();
            break;
        default:
            break; // unbound: swallowed, like an unmapped key in vim
        }
    }
    finish();
}

void VimMachine::second(QChar first, QChar c)
{
    const int times = count();
    switch (first.unicode()) {
    case 'g':
        if (c == QLatin1Char('g')) {
            m_host->trigger(QStringLiteral("transport.go_to_start"));
            afterMotion();
        } else if (m_mode == Mode::Normal && c == QLatin1Char('i')) {
            m_host->trigger(QStringLiteral("edit.insert")); // source edits: gi go ga for now
        } else if (m_mode == Mode::Normal && c == QLatin1Char('o')) {
            m_host->trigger(QStringLiteral("edit.overwrite"));
        } else if (m_mode == Mode::Normal && c == QLatin1Char('a')) {
            m_host->trigger(QStringLiteral("edit.append_at_end"));
        }
        break;
    case 'd':
        if (c == QLatin1Char('d') && m_mode == Mode::Normal) {
            // The clip under the playhead (and the next ones for a count) is selected, then the
            // ordinary Ripple Delete runs: one undo step.
            if (m_host->selectClipsAtPlayhead(times))
                m_host->trigger(QStringLiteral("edit.ripple_delete"));
        }
        break;
    case 'y':
        if (c == QLatin1Char('y') && m_mode == Mode::Normal) {
            if (m_host->selectClipsAtPlayhead(times))
                m_host->trigger(QStringLiteral("edit.copy"));
        }
        break;
    case 'z':
        if (c == QLatin1Char('i'))
            m_host->trigger(QStringLiteral("timeline.zoom_in"));
        else if (c == QLatin1Char('o'))
            m_host->trigger(QStringLiteral("timeline.zoom_out"));
        else if (c == QLatin1Char('f'))
            m_host->trigger(QStringLiteral("timeline.zoom_fit"));
        break;
    case 'm':
        if (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            m_marks[c] = m_host->playhead();
        break;
    case '\'':
    case '`':
        if (const auto it = m_marks.find(c); it != m_marks.end()) {
            m_host->setPlayhead(it->second);
            afterMotion();
        }
        break;
    default:
        break;
    }
    finish();
}

// ---- VimInput ------------------------------------------------------------------------

VimInput::VimInput(ActionRegistry* actions, QObject* parent)
    : QObject(parent)
    , m_actions(actions)
    , m_machine(this)
{
    connect(&m_machine, &VimMachine::statusChanged, this, [this] {
        emit statusChanged(statusText());
        emit displayChanged();
    });
    QAction* option = m_actions && m_actions->contains(QLatin1String(vimopts::kActionId))
        ? m_actions->action(QLatin1String(vimopts::kActionId))
        : nullptr;
    if (option) {
        m_active = option->isChecked();
        connect(option, &QAction::toggled, this, [this](bool on) { setActive(on); });
    } else {
        m_active = vimopts::enabled();
    }
    if (qApp) {
        qApp->installEventFilter(this);
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
            if (!inScope(now))
                m_machine.cancelPending();
            emit displayChanged();
        });
    }
}

VimInput::~VimInput()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

void VimInput::setActive(bool on)
{
    if (on == m_active)
        return;
    m_active = on;
    if (!on)
        m_machine.reset();
    emit activeChanged(on);
    emit statusChanged(statusText());
    emit displayChanged();
}

QString VimInput::statusText() const
{
    return m_active ? m_machine.statusText() : QString();
}

bool VimInput::isTextField(const QWidget* focus)
{
    if (!focus)
        return false;
    if (qobject_cast<const QAbstractSpinBox*>(focus) || qobject_cast<const QTextEdit*>(focus)
        || qobject_cast<const QPlainTextEdit*>(focus) || focus->inherits("QLineEdit")
        || focus->inherits("QKeySequenceEdit"))
        return true;
    if (const auto* combo = qobject_cast<const QComboBox*>(focus); combo && combo->isEditable())
        return true;
    for (const QWidget* w = focus; w; w = w->parentWidget()) {
        if (w->inherits("QLineEdit") || w->inherits("QAbstractSpinBox"))
            return true; // an editor embedded in a view or a spin box
    }
    return false;
}

VimMachine::Mode VimInput::displayMode(const QWidget* focus) const
{
    const VimMachine::Mode mode = m_machine.mode();
    if (mode == VimMachine::Mode::Command)
        return mode;
    if (isTextField(focus) && !(focus && focus->property(kCommandLineProperty).toBool()))
        return VimMachine::Mode::Insert;
    return mode;
}

bool VimInput::inScope(const QWidget* focus) const
{
    if (!m_active || !focus)
        return false;
    if (QApplication::activeModalWidget() || QApplication::activePopupWidget())
        return false;
    // Anything the user types text into keeps its keys.
    if (isTextField(focus))
        return false;
    for (const QWidget* w = focus; w; w = w->parentWidget()) {
        if (w->property(kScopeProperty).toBool())
            return true;
    }
    return false;
}

void VimInput::focusPanelHome(QWidget* from)
{
    QWidget* frame = nullptr;
    for (QWidget* w = from; w; w = w->parentWidget()) {
        if (w->inherits("kk::PanelFrame")) {
            frame = w;
            break;
        }
    }
    if (!frame)
        return;
    // The panel's main widget (timeline, viewer) when it has one, else the panel itself.
    for (QWidget* child : frame->findChildren<QWidget*>()) {
        if (child->property(kScopeProperty).toBool() && child->isVisible()
            && child->focusPolicy() != Qt::NoFocus) {
            child->setFocus(Qt::OtherFocusReason);
            return;
        }
    }
    frame->setFocus(Qt::OtherFocusReason);
}

/// Esc, one rule from the innermost thing outwards (spec 83): pending keys, VISUAL, INSERT are
/// the machine's; this handles what is left: a text field is left (its text stays) and a
/// control deep inside a panel gives the focus back to the panel. Returns whether the key was
/// taken (only when the machine used it).
bool VimInput::handleEscape(QKeyEvent* key, QEvent::Type type)
{
    QWidget* focus = QApplication::focusWidget();
    if (!focus || QApplication::activeModalWidget() || QApplication::activePopupWidget())
        return false;
    if (focus->property(kCommandLineProperty).toBool())
        return false;
    const bool textField = isTextField(focus);
    if (!textField && m_machine.wouldConsume(key->key(), key->modifiers(), key->text())) {
        if (!inScope(focus)) {
            // Outside the timeline and viewers the machine still comes back to NORMAL, but
            // the key is left to the widget (it closes a popup, cancels an edit).
            if (type == QEvent::KeyPress)
                m_machine.handleKey(key->key(), key->modifiers(), key->text());
            return false;
        }
        if (type == QEvent::ShortcutOverride) {
            key->accept();
            return true;
        }
        return m_machine.handleKey(key->key(), key->modifiers(), key->text());
    }
    if (type != QEvent::KeyPress || key->key() != Qt::Key_Escape)
        return false;
    // Rules 4 and 5: not taken (a drag is cancelled, a popup closes, a numeric field cancels
    // its typing as always); afterwards the focus goes back to the panel when it is still on
    // a field or control. Deferred so the widget's own Esc handling runs first.
    bool numeric = false;
    for (const QWidget* w = focus; w; w = w->parentWidget())
        numeric = numeric || w->inherits("kk::ValueField");
    bool inEditor = false;
    for (const QWidget* w = focus; w; w = w->parentWidget())
        inEditor = inEditor || qobject_cast<const QAbstractItemView*>(w);
    if (numeric || inEditor || focus->property(kScopeProperty).toBool())
        return false;
    QPointer<QWidget> from = focus;
    QTimer::singleShot(0, this, [from] {
        if (from && QApplication::focusWidget() == from.data())
            focusPanelHome(from);
    });
    return false;
}

bool VimInput::eventFilter(QObject*, QEvent* event)
{
    if (!m_active)
        return false;
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)
        return false;
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->key() == Qt::Key_Escape)
        return handleEscape(key, event->type());
    if (!m_host || !inScope(QApplication::focusWidget()))
        return false;
    if (!m_machine.wouldConsume(key->key(), key->modifiers(), key->text()))
        return false;
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept(); // no shortcut fires; the key arrives as a KeyPress next
        return true;
    }
    return m_machine.handleKey(key->key(), key->modifiers(), key->text());
}

} // namespace kk
