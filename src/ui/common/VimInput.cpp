// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/common/VimInput.h"

#include "ui/common/ActionRegistry.h"
#include "ui/common/VimGrammar.h"
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
#include <climits>
#include <cstdlib>

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
    QChar mode = QLatin1Char('n');
    if (!m_opName.isEmpty())
        mode = QLatin1Char('o');
    else if (m_mode == Mode::Visual)
        mode = QLatin1Char('v');
    return m_keymap->trie(currentContext(), mode);
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
    const QString name = displayName();
    return m_pending.isEmpty() ? name : name + QStringLiteral("  ") + m_pending;
}

void VimMachine::enterInsert()
{
    const bool wasVisual = m_mode == Mode::Visual;
    cancelPending();
    if (wasVisual)
        leaveVisual(true);
    m_mode = Mode::Insert;
    emit statusChanged();
}

QString VimMachine::displayName() const
{
    if (m_mode == Mode::Visual) {
        if (m_visualKind == VisualKind::Line)
            return tr("V-LINE");
        if (m_visualKind == VisualKind::Block)
            return tr("V-BLOCK");
    }
    return modeName(m_mode);
}

std::optional<VimPos> VimMachine::mark(QChar name) const
{
    const auto it = m_marks.find(name);
    if (it == m_marks.end())
        return std::nullopt;
    return it->second;
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
    QString text = m_head;
    if (m_regPending)
        text += QLatin1Char('"');
    text += m_countText;
    for (const QString& t : m_keys)
        text += vimkeys::isCharToken(t) ? t : vimkeys::displayToken(t);
    m_pending = text;
}

namespace {

/// A character that names a register: a-z (A-Z count as the lower case), 0, _ .
bool isRegisterName(const QString& token)
{
    if (token.size() != 1)
        return false;
    const QChar c = token.at(0);
    return c.isLetter() ? c.unicode() < 128 : (c == QLatin1Char('0') || c == QLatin1Char('_'));
}

VimOp opFromName(const QString& name)
{
    if (name == QLatin1String("op_change"))
        return VimOp::Change;
    if (name == QLatin1String("op_yank"))
        return VimOp::Yank;
    if (name == QLatin1String("op_lift"))
        return VimOp::Lift;
    return VimOp::Delete;
}

} // namespace

bool VimMachine::isOperatorBinding(const VimBinding* binding, QString* name)
{
    if (!binding || binding->run.size() != 1)
        return false;
    const QString item = VimKeymap::normaliseRunItem(binding->run.first());
    if (!item.startsWith(QLatin1String("vim:op_")))
        return false;
    if (name)
        *name = item.mid(4);
    return true;
}

void VimMachine::feed(const QString& token)
{
    m_timer->stop();
    setWhichKey(false);
    if (m_regPending) {
        m_regPending = false;
        if (isRegisterName(token)) {
            m_reg = token.at(0).toLower();
            m_head += QLatin1Char('"') + token;
            updatePending();
            emit statusChanged();
        } else {
            finish(); // not a register: the whole command is dropped
        }
        return;
    }
    if (m_keys.isEmpty() && token == QLatin1String("\"") && m_reg.isNull() && m_opName.isEmpty()) {
        m_regPending = true;
        updatePending();
        emit statusChanged();
        return;
    }
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
    m_cmdKeys.append(token);
    updatePending();
    // `dd`, `yy`, `cc`: the operator's key typed again works on the clip under the cursor.
    if (!m_opName.isEmpty() && m_keys.size() == 1 && token == m_opToken) {
        Call call = takeCall(nullptr, QString());
        call.operatorRepeat = true;
        runCall(call);
        return;
    }
    resolve();
}

VimMachine::Call VimMachine::takeCall(const VimBinding* binding, const QString& arg)
{
    Call call;
    call.binding = binding;
    call.arg = arg;
    const int before = m_opName.isEmpty() ? 0 : m_opCount;
    call.hasCount = before > 0 || m_count > 0;
    call.count = std::min(kMaxCount * 10, std::max(1, before) * std::max(1, m_count));
    call.reg = m_reg;
    call.op = m_opName;
    call.opToken = m_opToken;
    call.keys = m_cmdKeys;
    if (m_mode == Mode::Visual && m_host) {
        const VimPos cur = cursor();
        call.visual = m_visualKind;
        call.visualFrames = std::abs(cur.frame - m_anchor.frame) + 1;
        call.visualTracks = std::abs(cur.track - m_anchor.track);
    }
    endCommand();
    emit statusChanged();
    return call;
}

void VimMachine::dispatch(const VimBinding* binding, const QString& arg)
{
    QString opName;
    if (m_mode == Mode::Normal && m_opName.isEmpty() && isOperatorBinding(binding, &opName)) {
        beginOperator(opName, m_keys.last());
        return;
    }
    runCall(takeCall(binding, arg));
}

void VimMachine::beginOperator(const QString& name, const QString& token)
{
    m_opCount = m_count;
    m_head += m_countText;
    for (const QString& t : m_keys)
        m_head += vimkeys::isCharToken(t) ? t : vimkeys::displayToken(t);
    m_opName = name;
    m_opToken = token;
    m_count = 0;
    m_countText.clear();
    m_keys.clear();
    m_exactLen = 0;
    m_exactBinding = nullptr;
    m_exactArg.clear();
    updatePending();
    emit statusChanged();
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
            m_cmdKeys.resize(m_cmdKeys.size() - rest.size());
            m_keys.resize(m_exactLen);
            m_exactLen = 0;
            m_exactBinding = nullptr;
            dispatch(binding, arg);
            for (const QString& t : rest)
                feed(t);
            return;
        }
        finish(); // unbound: swallowed (an operator is dropped), like an unmapped key in vim
        return;
    }
    case VimMatch::Kind::Exact:
        dispatch(match.binding, match.arg);
        return;
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
        m_exactLen = 0;
        m_exactBinding = nullptr;
        dispatch(binding, arg);
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
    m_keys.clear();
    m_exactLen = 0;
    m_exactBinding = nullptr;
    m_exactArg.clear();
    setWhichKey(false);
}

void VimMachine::endCommand()
{
    clearKeys();
    m_count = 0;
    m_countText.clear();
    m_pending.clear();
    m_head.clear();
    m_reg = QChar();
    m_regPending = false;
    m_opName.clear();
    m_opToken.clear();
    m_opCount = 0;
    m_cmdKeys.clear();
}

void VimMachine::finish()
{
    endCommand();
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
    m_lastJump.reset();
    m_lastVisual = {};
    m_dot = {};
    cancelPending();
    if (wasVisual)
        leaveVisual(true);
    if (wasInsert) {
        m_mode = Mode::Normal;
        emit statusChanged();
    }
}

// ---- Cursor and VISUAL ---------------------------------------------------------------

VimPos VimMachine::cursor() const
{
    return {m_host->playhead(), m_host->cursorTrack()};
}

void VimMachine::moveCursor(VimPos to)
{
    if (to.frame != m_host->playhead())
        m_host->setPlayhead(std::max<int64_t>(0, to.frame));
    if (to.track != m_host->cursorTrack())
        m_host->setCursorTrack(to.track);
}

void VimMachine::showVisual()
{
    VimVisual v;
    v.kind = m_visualKind;
    v.anchor = m_anchor;
    v.cursor = cursor();
    m_host->showVisual(v);
}

void VimMachine::enterVisualKind(VisualKind kind)
{
    if (m_mode == Mode::Visual) {
        if (m_visualKind == kind) {
            leaveVisual(true);
            return;
        }
        m_visualKind = kind;
        showVisual();
        emit statusChanged();
        return;
    }
    m_mode = Mode::Visual;
    m_visualKind = kind;
    m_anchor = cursor();
    showVisual();
    emit statusChanged();
}

void VimMachine::leaveVisual(bool clearSelection)
{
    if (m_mode == Mode::Visual) {
        m_lastVisual.kind = m_visualKind;
        m_lastVisual.anchor = m_anchor;
        m_lastVisual.cursor = cursor();
    }
    m_mode = Mode::Normal;
    m_visualKind = VisualKind::None;
    m_host->showVisual({});
    if (clearSelection)
        m_host->clearSelection();
    emit statusChanged();
}

VimRange VimMachine::visualRange() const
{
    const VimPos cur = cursor();
    const int64_t lo = std::min(m_anchor.frame, cur.frame);
    const int64_t hi = std::max(m_anchor.frame, cur.frame) + 1;
    const int first = std::min(m_anchor.track, cur.track);
    const int last = std::max(m_anchor.track, cur.track);
    VimRange range;
    if (m_visualKind == VisualKind::Line) {
        range.kind = VimRange::Kind::Clips;
        range.clips = vimgrammar::clipsTouching(*m_host, first, last, lo, hi);
        return range;
    }
    range.kind = VimRange::Kind::Frames;
    range.start = lo;
    range.end = hi;
    range.trackFirst = m_visualKind == VisualKind::Block ? first : cur.track;
    range.trackLast = m_visualKind == VisualKind::Block ? last : cur.track;
    return range;
}

void VimMachine::selectObject(const VimRange& range)
{
    int64_t lo = range.start;
    int64_t hi = range.end - 1;
    int first = range.trackFirst;
    int last = range.trackLast;
    if (range.kind == VimRange::Kind::Clips) {
        lo = INT64_MAX;
        hi = -1;
        first = INT_MAX;
        last = -1;
        for (int t = 0; t < m_host->trackCount(); ++t) {
            for (const VimClipSpan& c : m_host->clipSpans(t)) {
                if (!range.clips.contains(c.id))
                    continue;
                lo = std::min(lo, c.start);
                hi = std::max(hi, c.end - 1);
                first = std::min(first, t);
                last = std::max(last, t);
            }
        }
        if (last < 0)
            return;
    } else if (range.allTracks) {
        first = 0;
        last = std::max(0, m_host->trackCount() - 1);
    }
    m_anchor = {lo, first};
    moveCursor({hi, last});
    showVisual();
}

// ---- Running commands ----------------------------------------------------------------

void VimMachine::recordDot(const Call& call)
{
    if (m_skipRecord || m_replaying || !m_changed)
        return;
    m_dot.valid = true;
    m_dot.reg = call.reg;
    m_dot.count = call.count;
    m_dot.hasCount = call.hasCount;
    m_dot.keys = call.keys;
    m_dot.visual = call.visual;
    m_dot.frames = call.visualFrames;
    m_dot.tracks = call.visualTracks;
}

void VimMachine::runCall(const Call& call)
{
    if (!m_host)
        return;
    m_changed = false;
    if (!m_replaying)
        m_skipRecord = false; // `.` sets it again; the commands it replays leave it alone
    // A command that edits is one undo step, whatever it does inside (a count, a paste after
    // a delete): the host groups them.
    bool group = call.operatorRepeat || !call.op.isEmpty();
    if (call.binding) {
        group = group || call.binding->dot;
        for (const QString& item : call.binding->run) {
            const QString n = VimKeymap::normaliseRunItem(item);
            group = group || n.startsWith(QLatin1String("vim:op_")) || n.startsWith(QLatin1String("vim:cmd_"))
                || n == QLatin1String("vim:visual_paste") || n == QLatin1String("vim:repeat");
        }
    }
    if (group)
        m_host->beginUndoGroup();
    if (call.operatorRepeat) {
        runOperatorRepeat(call);
    } else if (call.binding) {
        for (const QString& item : call.binding->run)
            runItem(item, call);
        if (call.binding->dot)
            markChange();
    }
    if (group)
        m_host->endUndoGroup();
    recordDot(call);
}

void VimMachine::runItem(const QString& rawItem, const Call& call)
{
    const QString item = VimKeymap::normaliseRunItem(rawItem);
    if (item.startsWith(QLatin1String("vim:"))) {
        runBuiltin(item.mid(4), call);
    } else if (item.startsWith(QLatin1String("action:"))) {
        const QString id = item.mid(7);
        const int times = call.binding->repeat ? call.count : 1;
        for (int i = 0; i < times; ++i) {
            if (!m_host->trigger(id))
                break;
        }
    } else if (item.startsWith(QLatin1String("ex:"))) {
        m_host->runEx(item.mid(3));
    }
}

bool VimMachine::runMotion(const QString& name, const Call& call)
{
    const VimPos from = cursor();
    std::optional<vimgrammar::Motion> motion;
    bool jump = false;
    if (name == QLatin1String("goto_mark")) {
        if (!call.arg.isEmpty()) {
            if (const auto place = mark(call.arg.at(0)))
                motion = vimgrammar::Motion {*place, vimgrammar::Motion::Type::Exclusive, false};
        }
        jump = true;
    } else if (name == QLatin1String("jump_back")) {
        if (m_lastJump)
            motion = vimgrammar::Motion {*m_lastJump, vimgrammar::Motion::Type::Exclusive, false};
        jump = true;
    } else {
        motion = vimgrammar::motion(*m_host, name, from, call.count, call.hasCount);
        jump = name == QLatin1String("track_first") || name == QLatin1String("track_last");
    }
    if (!motion)
        return false; // nothing to go to: an operator waiting for it is dropped
    if (!call.op.isEmpty()) {
        // `cw` works like `ce` (Neovim change.txt, "cw"): it changes the clip, not the gap after.
        if (call.op == QLatin1String("op_change") && name == QLatin1String("clip_next")) {
            const VimClipSpan under = vimgrammar::clipAt(*m_host, from.track, from.frame);
            if (under.id) {
                if (call.count == 1 && from.frame == under.end - 1)
                    motion = vimgrammar::Motion {from, vimgrammar::Motion::Type::Inclusive, false};
                else
                    motion = vimgrammar::motion(*m_host, QStringLiteral("clip_end"), from,
                        call.count, call.hasCount);
                if (!motion)
                    return false;
            }
        }
        applyOperator(opFromName(call.op), vimgrammar::rangeOfMotion(*m_host, from, *motion), call);
        return true;
    }
    if (jump)
        m_lastJump = from;
    moveCursor(motion->to);
    if (m_mode == Mode::Visual)
        showVisual();
    return true;
}

bool VimMachine::runObject(const QString& name, const Call& call)
{
    const auto range = vimgrammar::object(*m_host, name, cursor(), call.count);
    if (!range)
        return false;
    if (!call.op.isEmpty())
        applyOperator(opFromName(call.op), *range, call);
    else if (m_mode == Mode::Visual)
        selectObject(*range);
    return true;
}

void VimMachine::applyOperator(VimOp op, const VimRange& range, const Call& call)
{
    const bool visual = m_mode == Mode::Visual;
    if (visual)
        leaveVisual(true);
    if (range.isEmpty())
        return;
    VimOperation operation;
    operation.op = op;
    operation.reg = call.reg;
    operation.range = range;
    if (!m_host->operate(operation))
        return;
    if (op != VimOp::Yank)
        markChange();
    if (op == VimOp::Yank) {
        if (range.kind == VimRange::Kind::Frames) {
            m_host->setPlayhead(range.start);
            m_host->showMessage(tr("%1 frames yanked").arg(range.end - range.start));
        } else {
            m_host->showMessage(tr("%n clip(s) yanked", nullptr, int(range.clips.size())));
        }
    }
    if (op == VimOp::Change)
        enterInsert();
}

void VimMachine::runOperatorRepeat(const Call& call)
{
    // The clip under the cursor (count: and the next ones); in a gap, the gap.
    const auto range = vimgrammar::object(*m_host, QStringLiteral("obj_iw"), cursor(), call.count);
    if (range)
        applyOperator(opFromName(call.op), *range, call);
}

void VimMachine::runCommandOperator(VimOp op, const Call& call, bool toEnd)
{
    VimRange range;
    const VimPos at = cursor();
    if (toEnd) {
        const VimClipSpan under = vimgrammar::clipAt(*m_host, at.track, at.frame);
        if (!under.id)
            return;
        range.kind = VimRange::Kind::Frames;
        range.start = at.frame;
        range.end = under.end;
        range.trackFirst = range.trackLast = at.track;
    } else {
        const QVector<quint64> selected = call.hasCount ? QVector<quint64>() : m_host->selectedClips();
        if (!selected.isEmpty()) {
            range.kind = VimRange::Kind::Clips;
            range.clips = selected;
        } else {
            const auto r = vimgrammar::object(*m_host, QStringLiteral("obj_iw"), at, call.count);
            if (!r)
                return;
            range = *r;
        }
    }
    applyOperator(op, range, call);
}

void VimMachine::paste(const Call& call, bool insert)
{
    if (m_host->pasteRegister(call.reg, call.count, insert))
        markChange();
    else
        m_host->showMessage(tr("Nothing to paste"));
}

void VimMachine::visualPaste(const Call& call)
{
    const VimRange range = visualRange();
    leaveVisual(true);
    if (range.isEmpty())
        return;
    if (!m_host->registerHasContent(call.reg)) {
        m_host->showMessage(tr("Nothing to paste"));
        return; // the selection stays as it is
    }
    // The selection is replaced: it goes, the register comes in at the same place. The deleted
    // material does not replace the register (a difference from Vim, which swaps them).
    VimOperation del;
    del.op = VimOp::Delete;
    del.reg = QLatin1Char('_');
    del.range = range;
    if (!m_host->operate(del))
        return;
    markChange();
    if (!m_host->pasteRegister(call.reg, 1, true))
        m_host->showMessage(tr("Nothing to paste"));
}

void VimMachine::repeatLast(const Call& call)
{
    if (!m_dot.valid || m_replaying)
        return;
    if (call.hasCount) {
        m_dot.count = call.count;
        m_dot.hasCount = true;
    }
    const Dot dot = m_dot;
    m_replaying = true;
    m_skipRecord = true;
    if (dot.visual != VisualKind::None) {
        // Vim repeats a VISUAL change on the same amount of text from the cursor.
        m_mode = Mode::Visual;
        m_visualKind = dot.visual;
        m_anchor = cursor();
        const int track = std::clamp(m_anchor.track + int(dot.tracks), 0,
            std::max(0, m_host->trackCount() - 1));
        moveCursor({m_anchor.frame + dot.frames - 1, track});
        showVisual();
    }
    QStringList tokens;
    if (!dot.reg.isNull())
        tokens << QStringLiteral("\"") << QString(dot.reg);
    if (dot.hasCount) {
        for (const QChar digit : QString::number(dot.count))
            tokens << QString(digit);
    }
    tokens += dot.keys;
    for (const QString& t : std::as_const(tokens))
        feed(t);
    m_replaying = false;
}

bool VimMachine::runBuiltin(const QString& name, const Call& call)
{
    if (vimgrammar::isMotion(name) || name == QLatin1String("goto_mark")
        || name == QLatin1String("jump_back")) {
        if (!runMotion(name, call) && !call.op.isEmpty())
            return false;
        return true;
    }
    if (vimgrammar::isObject(name)) {
        runObject(name, call);
        return true;
    }
    if (name.startsWith(QLatin1String("op_"))) {
        // In VISUAL the operator acts on the selection; in NORMAL it was started by dispatch().
        if (m_mode == Mode::Visual)
            applyOperator(opFromName(name), visualRange(), call);
        return true;
    }
    if (name == QLatin1String("set_mark")) {
        if (!call.arg.isEmpty())
            m_marks[call.arg.at(0)] = cursor();
    } else if (name == QLatin1String("cmd_delete")) {
        runCommandOperator(VimOp::Delete, call, false);
    } else if (name == QLatin1String("cmd_lift")) {
        runCommandOperator(VimOp::Lift, call, false);
    } else if (name == QLatin1String("cmd_yank")) {
        runCommandOperator(VimOp::Yank, call, false);
    } else if (name == QLatin1String("cmd_delete_to_end")) {
        runCommandOperator(VimOp::Delete, call, true);
    } else if (name == QLatin1String("cmd_paste")) {
        paste(call, true);
    } else if (name == QLatin1String("cmd_paste_over")) {
        paste(call, false);
    } else if (name == QLatin1String("visual_paste")) {
        visualPaste(call);
    } else if (name == QLatin1String("repeat")) {
        repeatLast(call);
    } else if (name == QLatin1String("cursor_select")) {
        m_cursorSelected = m_host->selectCursorClip();
    } else if (name == QLatin1String("cursor_unselect")) {
        m_host->restoreSelection(m_cursorSelected);
        m_cursorSelected = false;
    } else if (name == QLatin1String("copy_timecode")) {
        const QString tc = m_host->timecode(m_host->playhead());
        m_host->copyToClipboard(tc);
        m_host->showMessage(tr("Copied %1").arg(tc));
    } else if (name == QLatin1String("visual_char")) {
        enterVisualKind(VisualKind::Char);
    } else if (name == QLatin1String("visual_line")) {
        enterVisualKind(VisualKind::Line);
    } else if (name == QLatin1String("visual_block")) {
        enterVisualKind(VisualKind::Block);
    } else if (name == QLatin1String("visual_other_end")) {
        if (m_mode == Mode::Visual) {
            const VimPos old = cursor();
            moveCursor(m_anchor);
            m_anchor = old;
            showVisual();
        }
    } else if (name == QLatin1String("visual_reselect")) {
        if (m_mode != Mode::Visual && m_lastVisual.kind != VisualKind::None) {
            m_mode = Mode::Visual;
            m_visualKind = m_lastVisual.kind;
            m_anchor = m_lastVisual.anchor;
            moveCursor(m_lastVisual.cursor);
            showVisual();
            emit statusChanged();
        }
    } else if (name == QLatin1String("insert_mode")) {
        enterInsert(); // typing mode: the profile's own keys work (Esc comes back)
    } else if (name == QLatin1String("command_line")) {
        m_host->openCommandLine();
    } else if (name == QLatin1String("search")) {
        m_host->openSearch();
    } else if (name == QLatin1String("search_next") || name == QLatin1String("search_prev")) {
        if (!m_host->searchStep(name == QLatin1String("search_next"), call.count))
            m_host->showMessage(tr("No match"));
    } else if (m_commandHandler) {
        return m_commandHandler(name, call.count, call.hasCount, call.arg);
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
