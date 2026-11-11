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
#include <QWidget>

#include <algorithm>

namespace kk {

namespace {

constexpr int kMaxCount = 999;

} // namespace

// ---- VimMachine ----------------------------------------------------------------------

VimMachine::VimMachine(QObject* parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &VimMachine::timeoutNow);
    m_ownedKeymap = std::make_unique<VimKeymap>();
    m_ownedKeymap->loadDefaults();
    m_keymap = m_ownedKeymap.get();
}

VimMachine::~VimMachine() = default;

void VimMachine::setKeymap(VimKeymap* keymap)
{
    if (m_keymap == keymap || !keymap)
        return;
    m_keymap = keymap;
    m_ownedKeymap.reset();
    connect(keymap, &VimKeymap::changed, this, [this] { cancelPending(); });
}

QString VimMachine::currentContext() const
{
    const QString c = m_context ? m_context() : QStringLiteral("timeline");
    return c.isEmpty() ? QStringLiteral("timeline") : c;
}

const VimTrie& VimMachine::currentTrie() const
{
    return m_keymap->trie(
        currentContext(), m_mode == Mode::Visual ? QLatin1Char('v') : QLatin1Char('n'));
}

int VimMachine::effectiveTimeout() const
{
    return m_timeoutOverride > 0 ? m_timeoutOverride : m_keymap->timeoutMs();
}

bool VimMachine::hasBuiltin(const QString& name)
{
    return VimKeymap::builtinNames().contains(name);
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
    vimkeys::KeyInfo info;
    info.key = key;
    info.modifiers = modifiers;
    info.text = text;
    return wouldConsume(info);
}

bool VimMachine::handleKey(int key, Qt::KeyboardModifiers modifiers, const QString& text)
{
    vimkeys::KeyInfo info;
    info.key = key;
    info.modifiers = modifiers;
    info.text = text;
    return handleKey(info);
}

bool VimMachine::wouldConsume(const vimkeys::KeyInfo& key) const
{
    if (!m_host)
        return false;
    // The command line owns its keys.
    if (m_mode == Mode::Command)
        return false;
    const QString token = m_translator.token(key);
    if (token.isEmpty())
        return false;
    const bool escape = token == QLatin1String("<Esc>") || token == QLatin1String("<C-[>")
        || (token == QLatin1String("<C-c>") && busy());
    if (escape)
        return m_mode == Mode::Visual || m_mode == Mode::Insert || !m_pending.isEmpty();
    if (m_mode == Mode::Insert)
        return false;
    const VimTrie& trie = currentTrie();
    if (!m_keys.isEmpty())
        return vimkeys::isCharToken(token) || token == QLatin1String("<Space>")
            || trie.match(m_keys + QStringList {token}).kind != VimMatch::Kind::None;
    if (vimkeys::isCharToken(token))
        return true; // an unbound printable key does nothing, as in vim
    return trie.startsWith(token);
}

bool VimMachine::handleKey(const vimkeys::KeyInfo& key)
{
    if (!wouldConsume(key))
        return false;
    const QString token = m_translator.token(key);
    if (token == QLatin1String("<Esc>") || token == QLatin1String("<C-[>")
        || token == QLatin1String("<C-c>")) {
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
    feed(token);
    return true;
}

void VimMachine::updatePending()
{
    QString text = m_countText;
    for (const QString& t : m_keys)
        text += vimkeys::isCharToken(t) ? t : vimkeys::displayToken(t);
    // Keys after the first are separated for readability when named keys are involved.
    m_pending = text;
}

void VimMachine::feed(const QString& token)
{
    m_timer->stop();
    setWhichKey(false);
    if (m_keys.isEmpty() && vimkeys::isCharToken(token)) {
        const QChar c = token.at(0);
        if ((c >= QLatin1Char('1') && c <= QLatin1Char('9'))
            || (c == QLatin1Char('0') && m_count > 0)) {
            m_count = std::min(kMaxCount, m_count * 10 + (c.unicode() - '0'));
            m_countText += c;
            updatePending();
            emit statusChanged();
            return;
        }
    }
    m_keys.append(token);
    updatePending();
    resolve();
}

void VimMachine::resolve()
{
    const VimMatch match = currentTrie().match(m_keys);
    switch (match.kind) {
    case VimMatch::Kind::None: {
        if (m_exactLen > 0 && m_exactBinding) {
            // The shorter binding runs, then the keys typed after it start over (the remapper
            // case of VSCodeVim: "g" bound and "gx" typed).
            const QStringList rest = m_keys.mid(m_exactLen);
            const VimBinding* binding = m_exactBinding;
            const QString arg = m_exactArg;
            const int count = m_count > 0 ? m_count : 1;
            clearKeys();
            runBinding(binding, arg, count);
            for (const QString& t : rest)
                feed(t);
            return;
        }
        finish(); // unbound: swallowed, like an unmapped key in vim
        return;
    }
    case VimMatch::Kind::Exact: {
        const int count = m_count > 0 ? m_count : 1;
        const VimBinding* binding = match.binding;
        const QString arg = match.arg;
        finish();
        runBinding(binding, arg, count);
        return;
    }
    case VimMatch::Kind::ExactAndPartial:
        m_exactLen = int(m_keys.size());
        m_exactBinding = match.binding;
        m_exactArg = match.arg;
        m_timer->start(effectiveTimeout());
        emit statusChanged();
        return;
    case VimMatch::Kind::Partial:
        m_timer->start(effectiveTimeout());
        emit statusChanged();
        return;
    }
}

void VimMachine::timeoutNow()
{
    m_timer->stop();
    if (m_keys.isEmpty())
        return;
    if (m_exactBinding && m_exactLen == int(m_keys.size())) {
        const VimBinding* binding = m_exactBinding;
        const QString arg = m_exactArg;
        const int count = m_count > 0 ? m_count : 1;
        finish();
        runBinding(binding, arg, count);
        return;
    }
    setWhichKey(true); // a prefix with no short form: show what can follow
}

void VimMachine::setWhichKey(bool on)
{
    if (m_whichKey == on)
        return;
    m_whichKey = on;
    emit whichKeyChanged();
}

QString VimMachine::whichKeyPrefix() const
{
    QStringList parts;
    for (const QString& t : m_keys)
        parts.append(vimkeys::displayToken(t));
    return parts.join(QLatin1Char(' '));
}

QList<VimContinuation> VimMachine::whichKeyEntries() const
{
    return currentTrie().continuations(m_keys);
}

void VimMachine::clearKeys()
{
    m_timer->stop();
    m_count = 0;
    m_countText.clear();
    m_keys.clear();
    m_pending.clear();
    m_exactLen = 0;
    m_exactBinding = nullptr;
    m_exactArg.clear();
    setWhichKey(false);
}

void VimMachine::finish()
{
    clearKeys();
    emit statusChanged();
}

void VimMachine::cancelPending()
{
    if (m_count == 0 && m_pending.isEmpty() && m_keys.isEmpty() && !m_whichKey)
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

void VimMachine::enterVisual()
{
    m_mode = Mode::Visual;
    m_anchor = m_host->playhead();
    m_host->selectRange(m_anchor, m_anchor);
    emit statusChanged();
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

void VimMachine::runBinding(const VimBinding* binding, const QString& arg, int count)
{
    if (!m_host || !binding)
        return;
    for (const QString& item : binding->run)
        runItem(item, binding, arg, count);
    // Every motion extends the VISUAL range.
    if (binding->type == QLatin1String("motion"))
        afterMotion();
}

void VimMachine::runItem(
    const QString& rawItem, const VimBinding* binding, const QString& arg, int count)
{
    const QString item = VimKeymap::normaliseRunItem(rawItem);
    if (item.startsWith(QLatin1String("vim:"))) {
        runBuiltin(item.mid(4), arg, count);
    } else if (item.startsWith(QLatin1String("action:"))) {
        const QString id = item.mid(7);
        const int times = binding->repeat ? count : 1;
        for (int i = 0; i < times; ++i) {
            if (!m_host->trigger(id))
                break;
        }
    } else if (item.startsWith(QLatin1String("ex:"))) {
        m_host->runEx(item.mid(3));
    }
}

bool VimMachine::runBuiltin(const QString& name, const QString& arg, int count)
{
    if (name == QLatin1String("set_mark")) {
        if (!arg.isEmpty())
            m_marks[arg.at(0)] = m_host->playhead();
    } else if (name == QLatin1String("goto_mark")) {
        if (!arg.isEmpty()) {
            if (const auto it = m_marks.find(arg.at(0)); it != m_marks.end())
                m_host->setPlayhead(it->second);
        }
    } else if (name == QLatin1String("target_down")) {
        m_host->moveTarget(-count);
    } else if (name == QLatin1String("target_up")) {
        m_host->moveTarget(count);
    } else if (name == QLatin1String("delete_clips")) {
        // The clip under the playhead (and the next ones for a count) is selected, then the
        // ordinary Ripple Delete runs: one undo step.
        if (m_host->selectClipsAtPlayhead(count))
            m_host->trigger(QStringLiteral("edit.ripple_delete"));
    } else if (name == QLatin1String("yank_clips")) {
        if (m_host->selectClipsAtPlayhead(count))
            m_host->trigger(QStringLiteral("edit.copy"));
    } else if (name == QLatin1String("visual_start")) {
        enterVisual();
    } else if (name == QLatin1String("visual_leave")) {
        leaveVisual(true);
    } else if (name == QLatin1String("visual_delete")) {
        m_host->trigger(QStringLiteral("edit.ripple_delete"));
        leaveVisual(false);
    } else if (name == QLatin1String("visual_delete_gap")) {
        m_host->trigger(QStringLiteral("edit.delete"));
        leaveVisual(false);
    } else if (name == QLatin1String("visual_yank")) {
        m_host->trigger(QStringLiteral("edit.copy"));
        leaveVisual(true);
    } else if (name == QLatin1String("insert_mode")) {
        enterInsert(); // typing mode: the profile's own keys work (Esc comes back)
    } else if (name == QLatin1String("command_line")) {
        m_host->openCommandLine();
    } else if (name == QLatin1String("search")) {
        m_host->openSearch();
    } else {
        return false;
    }
    return true;
}

// ---- VimInput ------------------------------------------------------------------------

VimInput::VimInput(ActionRegistry* actions, QObject* parent)
    : QObject(parent)
    , m_actions(actions)
    , m_machine(this)
{
    if (m_actions) {
        m_keymap = VimKeymap::of(*m_actions);
        m_machine.setKeymap(m_keymap);
        connect(m_keymap, &VimKeymap::optionsChanged, this,
            [this] { m_machine.setPhysicalKeys(vimopts::physicalKeys()); });
    }
    m_machine.setPhysicalKeys(vimopts::physicalKeys());
    m_machine.setContextProvider([] {
        const QString context = contextOf(QApplication::focusWidget());
        return context.isEmpty() ? QStringLiteral("global") : context;
    });
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
    return !contextOf(focus).isEmpty();
}

QString VimInput::contextOf(const QWidget* focus)
{
    for (const QWidget* w = focus; w; w = w->parentWidget()) {
        if (const QString context = w->property(kScopeProperty).toString(); !context.isEmpty())
            return context;
    }
    return {};
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
        if (!child->property(kScopeProperty).toString().isEmpty() && child->isVisible()
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
    const vimkeys::KeyInfo info = vimkeys::KeyInfo::from(key);
    if (!textField && m_machine.wouldConsume(info)) {
        if (!inScope(focus)) {
            // Outside the timeline and viewers the machine still comes back to NORMAL, but
            // the key is left to the widget (it closes a popup, cancels an edit).
            if (type == QEvent::KeyPress)
                m_machine.handleKey(info);
            return false;
        }
        if (type == QEvent::ShortcutOverride) {
            key->accept();
            return true;
        }
        return m_machine.handleKey(info);
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
    if (numeric || inEditor || !focus->property(kScopeProperty).toString().isEmpty())
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
    const vimkeys::KeyInfo info = vimkeys::KeyInfo::from(key);
    if (!m_machine.wouldConsume(info))
        return false;
    if (event->type() == QEvent::ShortcutOverride) {
        event->accept(); // no shortcut fires; the key arrives as a KeyPress next
        return true;
    }
    return m_machine.handleKey(info);
}

} // namespace kk
