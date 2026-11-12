// SPDX-License-Identifier: GPL-3.0-or-later
// VimInput — the vim key layer (spec sections 62 and 83): multi-key sequences (gg, dd, yy),
// counts (5l) and modes (NORMAL, VISUAL, INSERT, COMMAND) that plain shortcut presets cannot
// express.
//
// Two parts:
// - VimMachine: the state machine. It knows keys, counts and modes and asks a VimHost to do
//   things. It never edits anything itself: it parses [register][count]operator[count]motion
//   (or text object), turns it into a range (VimGrammar.h) and hands the operation to the host,
//   which does it as one undo step; other keys trigger actions (ActionRegistry ids).
// - VimInput: the application-wide event filter. Active only while the Vim mode option is on
//   (QSettings "vim/enabled", the action settings.vim_mode; independent of the keyboard
//   preset) AND the focus is in a timeline or viewer (a widget with the property kkVimScope)
//   that is not a text field, with no dialog or popup open. Everywhere else keys behave as
//   always. INSERT mode (and any text field) makes the layer passive: the preset's own
//   shortcuts work.
#pragma once

#include "ui/common/VimHost.h"
#include "ui/common/VimKeymap.h"
#include "ui/common/VimKeys.h"

#include <QChar>
#include <QEvent>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>

class QKeyEvent;
class QTimer;
class QWidget;

namespace kk {

class ActionRegistry;

class VimMachine : public QObject
{
    Q_OBJECT
public:
    enum class Mode {
        Normal,
        Visual,
        Insert,
        Command
    };
    using VisualKind = VimVisual::Kind;

    /// Without a keymap the machine uses the default table (no user changes).
    explicit VimMachine(QObject* parent = nullptr);
    ~VimMachine() override;
    void setHost(VimHost* host) { m_host = host; }
    /// The key table; not owned. Changes of the table drop half-typed keys.
    void setKeymap(VimKeymap* keymap);
    [[nodiscard]] VimKeymap* keymap() const { return m_keymap; }
    /// Tells the context of the focus ("timeline", "viewer", ...); default "timeline".
    void setContextProvider(std::function<QString()> provider) { m_context = std::move(provider); }
    /// Milliseconds to wait for the next key of an ambiguous sequence; 0 = the keymap's value.
    void setTimeoutMs(int ms) { m_timeoutOverride = ms; }
    void setPhysicalKeys(bool on) { m_translator.setPhysical(on); }

    [[nodiscard]] Mode mode() const { return m_mode; }
    /// Count and pending keys as typed so far ("3d", "SPC f"), empty when nothing is pending.
    [[nodiscard]] QString pending() const { return m_pending; }
    /// "NORMAL", "VISUAL", "INSERT", "COMMAND", with the pending keys appended ("NORMAL  3d").
    [[nodiscard]] QString statusText() const;
    /// The mode name alone.
    [[nodiscard]] static QString modeName(Mode mode);
    /// What the mode line shows: "VISUAL", "V-LINE" (V) or "V-BLOCK" (Ctrl+V) in VISUAL, else
    /// modeName().
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] VisualKind visualKind() const { return m_visualKind; }
    /// The register typed with `"` and waiting for its command (0 = none).
    [[nodiscard]] QChar pendingRegister() const { return m_reg; }
    /// True while an operator waits for its motion or text object (`d`, `2y`).
    [[nodiscard]] bool operatorPending() const { return !m_opName.isEmpty(); }
    /// The place of a mark (m{a-z}), if set.
    [[nodiscard]] std::optional<VimPos> mark(QChar name) const;

    /// INSERT: the layer is passive until Esc. COMMAND is set by the window while its `:` line
    /// is open.
    void enterInsert();
    void setCommandMode(bool open);
    /// True while something would be undone by Esc: pending keys / count, VISUAL, INSERT, COMMAND.
    [[nodiscard]] bool busy() const { return m_mode != Mode::Normal || !m_pending.isEmpty(); }

    /// True if the layer would take this key in the current state: a printable key (bound or
    /// not, as in vim an unbound key does nothing), Space and other keys that start a
    /// binding (the leader, Ctrl+R), the keys that continue a pending sequence, Ctrl+[ and
    /// Esc while something is pending, and Ctrl+C while busy(). Keys that start nothing (arrow
    /// keys, F keys, Return, Ctrl chords of the profile) stay ordinary shortcuts. In INSERT only
    /// Esc is taken.
    [[nodiscard]] bool wouldConsume(const vimkeys::KeyInfo& key) const;
    [[nodiscard]] bool wouldConsume(int key, Qt::KeyboardModifiers modifiers, const QString& text) const;
    /// Handles a key that wouldConsume() accepted. Returns whether it was taken.
    bool handleKey(const vimkeys::KeyInfo& key);
    bool handleKey(int key, Qt::KeyboardModifiers modifiers, const QString& text);

    /// Drops a half-typed sequence and count (the focus moved away).
    void cancelPending();
    /// Back to NORMAL with nothing pending (selection of VISUAL cleared).
    void reset();

    // ---- Timeout and which-key ----

    /// What happens when the timeout of a pending sequence ends: the shorter binding runs, or
    /// (for a prefix only) the which-key list appears. The timer calls this; tests may too.
    void timeoutNow();
    /// True while the which-key list is up (a prefix waited for timeoutlen).
    [[nodiscard]] bool whichKeyShown() const { return m_whichKey; }
    /// The keys typed so far as shown in the which-key title ("SPC s").
    [[nodiscard]] QString whichKeyPrefix() const;
    /// The ways to continue the pending sequence, with their descriptions.
    [[nodiscard]] QList<VimContinuation> whichKeyEntries() const;

    /// Whether `vim:<name>` is a command this machine runs.
    [[nodiscard]] static bool hasBuiltin(const QString& name);

signals:
    void statusChanged();
    void whichKeyChanged();

private:
    /// Everything a command needs once its keys are complete.
    struct Call
    {
        const VimBinding* binding = nullptr;
        QString arg;
        int count = 1;
        bool hasCount = false;
        QChar reg;
        QString op; ///< the operator waiting for this motion / object ("op_delete"), else empty
        QString opToken; ///< the key that started it
        QStringList keys; ///< the keys without counts and register, for dot-repeat
        bool operatorRepeat = false; ///< `dd`: the operator key twice
        VisualKind visual = VisualKind::None; ///< VISUAL when the key was typed
        int64_t visualFrames = 0;
        int visualTracks = 0;
    };
    /// The last change, for `.`.
    struct Dot
    {
        bool valid = false;
        QChar reg;
        int count = 1;
        bool hasCount = false;
        QStringList keys;
        VimVisual::Kind visual = VimVisual::Kind::None;
        int64_t frames = 0; ///< VISUAL: width of the selection
        int tracks = 0; ///< VISUAL: tracks - 1 covered
    };

    void feed(const QString& token);
    void resolve();
    void finish(); ///< clears count and pending after a command
    void clearKeys(); ///< drops the keys typed (not the operator)
    void endCommand(); ///< drops everything of the command being typed
    [[nodiscard]] Call takeCall(const VimBinding* binding, const QString& arg);
    void runCall(const Call& call);
    void runItem(const QString& item, const Call& call);
    bool runBuiltin(const QString& name, const Call& call);
    bool runMotion(const QString& name, const Call& call);
    bool runObject(const QString& name, const Call& call);
    void beginOperator(const QString& name, const QString& token);
    void applyOperator(VimOp op, const VimRange& range, const Call& call);
    void runOperatorRepeat(const Call& call);
    void runCommandOperator(VimOp op, const Call& call, bool toEnd);
    void paste(const Call& call, bool insert);
    void visualPaste(const Call& call);
    void repeatLast(const Call& call);
    void markChange() { m_changed = true; }
    void dispatch(const VimBinding* binding, const QString& arg);
    void recordDot(const Call& call);
    void selectObject(const VimRange& range);
    [[nodiscard]] VimPos cursor() const;
    void moveCursor(VimPos to);
    void enterVisualKind(VisualKind kind);
    void leaveVisual(bool clearSelection);
    void showVisual();
    [[nodiscard]] VimRange visualRange() const;
    [[nodiscard]] static bool isOperatorBinding(const VimBinding* binding, QString* name);
    [[nodiscard]] const VimTrie& currentTrie() const;
    [[nodiscard]] QString currentContext() const;
    [[nodiscard]] int effectiveTimeout() const;
    void updatePending();
    void setWhichKey(bool on);

    VimHost* m_host = nullptr;
    VimKeymap* m_keymap = nullptr;
    std::unique_ptr<VimKeymap> m_ownedKeymap;
    std::function<QString()> m_context;
    mutable vimkeys::KeyTranslator m_translator;
    QTimer* m_timer = nullptr;
    int m_timeoutOverride = 0;
    Mode m_mode = Mode::Normal;
    int m_count = 0; ///< 0 = none typed
    QStringList m_keys; ///< the tokens of the sequence being typed (after the count)
    QString m_countText;
    QString m_pending; ///< count digits and keys, for display
    QString m_head; ///< display text typed before the sequence: register, count and operator
    int m_exactLen = 0; ///< length of the longest binding already complete in m_keys
    const VimBinding* m_exactBinding = nullptr;
    QString m_exactArg;
    bool m_whichKey = false;
    // register, operator
    QChar m_reg; ///< the register chosen with "
    bool m_regPending = false; ///< " typed, the register name comes next
    QString m_opName; ///< "op_delete" ... while an operator waits for its motion
    QString m_opToken; ///< the key that started it
    int m_opCount = 0; ///< the count typed before the operator
    QStringList m_cmdKeys; ///< keys of the command being typed (dot-repeat)
    // VISUAL
    VisualKind m_visualKind = VisualKind::None;
    VimPos m_anchor;
    struct LastVisual
    {
        VisualKind kind = VisualKind::None;
        VimPos anchor;
        VimPos cursor;
    } m_lastVisual;
    // marks, dot-repeat
    std::map<QChar, VimPos> m_marks; ///< m{a-z}
    std::optional<VimPos> m_lastJump; ///< where the last jump started ('')
    Dot m_dot;
    bool m_changed = false; ///< the command being run changed something
    bool m_replaying = false; ///< `.` is feeding keys: nothing is recorded
    bool m_cursorSelected = false; ///< cursor_select selected a clip for the next action
    bool m_skipRecord = false; ///< the running command is `.`: it does not become the last change
};

class VimInput : public QObject
{
    Q_OBJECT
public:
    /// Property (string) of the widgets whose focus turns the layer on: the key context
    /// ("timeline", "viewer").
    static constexpr char kScopeProperty[] = "kkVimContext";
    /// The key context of @p focus (the property of the nearest ancestor that has one), or empty.
    [[nodiscard]] static QString contextOf(const QWidget* focus);
    /// The key table (the application's, shared with Keyboard Customization).
    [[nodiscard]] VimKeymap* keymap() const { return m_keymap; }

    /// Follows the Vim mode option: the checkable action settings.vim_mode of @p actions (its
    /// check mark starts from QSettings "vim/enabled"), or the setting alone without actions.
    /// Installs itself on the application.
    explicit VimInput(ActionRegistry* actions, QObject* parent = nullptr);
    ~VimInput() override;

    void setHost(VimHost* host) { m_machine.setHost(host); m_host = host; }
    [[nodiscard]] VimMachine& machine() { return m_machine; }
    /// True while Vim mode is on.
    [[nodiscard]] bool active() const { return m_active; }
    /// Turns the layer on or off (what the option does; tests).
    void setActive(bool on);
    /// Property (bool) set on the `:` line of the window: its keys are never taken.
    static constexpr char kCommandLineProperty[] = "kkVimCommandLine";
    /// True for a widget the user types text into (line edits, text edits, spin boxes, editable
    /// combos, editors embedded in views): the layer is passive there (auto INSERT).
    [[nodiscard]] static bool isTextField(const QWidget* focus);
    /// The mode the mode line shows for @p focus: INSERT in a text field, else the machine's.
    [[nodiscard]] VimMachine::Mode displayMode(const QWidget* focus) const;
    /// Moves the focus from a field or control to the panel's main widget (the timeline or
    /// viewer of the panel, else the panel itself). The Esc rule: a field is left, not closed.
    static void focusPanelHome(QWidget* from);
    /// True if the layer takes keys for @p focus now (scope widget, no text field, no dialog).
    [[nodiscard]] bool inScope(const QWidget* focus) const;
    /// What the mode indicator shows: empty while Vim mode is off.
    [[nodiscard]] QString statusText() const;

signals:
    void statusChanged(const QString& text);
    void activeChanged(bool active);
    /// The focus moved (the mode line follows: auto INSERT in a text field).
    void displayChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool handleEscape(QKeyEvent* key, QEvent::Type type);

    ActionRegistry* m_actions = nullptr;
    VimKeymap* m_keymap = nullptr;
    VimMachine m_machine;
    VimHost* m_host = nullptr;
    bool m_active = false;
};

} // namespace kk
