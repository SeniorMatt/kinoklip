// SPDX-License-Identifier: GPL-3.0-or-later
// VimInput — the vim key layer (spec sections 62 and 83): multi-key sequences (gg, dd, yy),
// counts (5l) and modes (NORMAL, VISUAL, INSERT, COMMAND) that plain shortcut presets cannot
// express.
//
// Two parts:
// - VimMachine: the state machine. It knows keys, counts and modes and asks a VimHost to do
//   things. It never edits anything itself: every edit is an existing action (ActionRegistry
//   ids such as edit.ripple_delete) triggered through the host.
// - VimInput: the application-wide event filter. Active only while the Vim mode option is on
//   (QSettings "vim/enabled", the action settings.vim_mode; independent of the keyboard
//   preset) AND the focus is in a timeline or viewer (a widget with the property kkVimScope)
//   that is not a text field, with no dialog or popup open. Everywhere else keys behave as
//   always. INSERT mode (and any text field) makes the layer passive: the preset's own
//   shortcuts work.
#pragma once

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

class QKeyEvent;
class QTimer;
class QWidget;

namespace kk {

class ActionRegistry;

/// What the vim layer needs from the editor. Implemented for the timeline in
/// ui/timeline/VimTimelineHost.h; tests use a fake.
class VimHost
{
public:
    virtual ~VimHost() = default;
    /// Triggers the action @p actionId if it exists and is enabled. Returns whether it ran.
    virtual bool trigger(const QString& actionId) = 0;
    [[nodiscard]] virtual int64_t playhead() const = 0;
    virtual void setPlayhead(int64_t frame) = 0;
    /// Selects the clip under the playhead and the next @p count - 1 clips of its track
    /// (the target video track first, else the top-most track with a clip there). False when
    /// there is no clip under the playhead.
    virtual bool selectClipsAtPlayhead(int count) = 0;
    /// Selects the clips that touch the frames @p a .. @p b (either order) on every track.
    virtual void selectRange(int64_t a, int64_t b) = 0;
    virtual void clearSelection() = 0;
    /// Moves the target video track by @p delta (+1 = the track above).
    virtual void moveTarget(int delta) = 0;
    /// `:` was typed.
    virtual void openCommandLine() = 0;
    /// Runs an ex command line (a key bound to ":w<CR>" or "ex:marker add"); the window shows
    /// the result. Default: nothing.
    virtual void runEx(const QString& line) { Q_UNUSED(line); }
    /// `/` was typed. The host opens a search if the editor has one.
    virtual void openSearch() {}
};

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
    void feed(const QString& token);
    void resolve();
    void finish(); ///< clears count and pending after a command
    void clearKeys();
    void runBinding(const VimBinding* binding, const QString& arg, int count);
    void runItem(const QString& item, const VimBinding* binding, const QString& arg, int count);
    bool runBuiltin(const QString& name, const QString& arg, int count);
    void afterMotion();
    void enterVisual();
    void leaveVisual(bool clearSelection);
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
    int m_exactLen = 0; ///< length of the longest binding already complete in m_keys
    const VimBinding* m_exactBinding = nullptr;
    QString m_exactArg;
    bool m_whichKey = false;
    int64_t m_anchor = 0; ///< VISUAL: the frame where v was typed
    std::map<QChar, int64_t> m_marks; ///< m{a-z}
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
