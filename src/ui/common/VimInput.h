// SPDX-License-Identifier: GPL-3.0-or-later
// VimInput — the vim key layer (spec section 62): multi-key sequences (gg, dd, yy), counts
// (5l) and modes (NORMAL, VISUAL) that plain shortcut presets cannot express.
//
// Two parts:
// - VimMachine: the state machine. It knows keys, counts and modes and asks a VimHost to do
//   things. It never edits anything itself: every edit is an existing action (ActionRegistry
//   ids such as edit.ripple_delete) triggered through the host.
// - VimInput: the application-wide event filter. Active only while the keyboard profile is
//   "vim" AND the focus is in a timeline or viewer (a widget with the property kkVimScope) that
//   is not a text field, with no dialog or popup open. Everywhere else keys behave as always.
#pragma once

#include <QChar>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>

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
    /// `/` was typed. The host opens a search if the editor has one.
    virtual void openSearch() {}
};

class VimMachine : public QObject
{
    Q_OBJECT
public:
    enum class Mode { Normal, Visual };

    explicit VimMachine(QObject* parent = nullptr);
    void setHost(VimHost* host) { m_host = host; }

    [[nodiscard]] Mode mode() const { return m_mode; }
    /// Count and pending keys as typed so far ("3d"), empty when nothing is pending.
    [[nodiscard]] QString pending() const { return m_pending; }
    /// "NORMAL", "VISUAL", with the pending keys appended ("NORMAL  3d").
    [[nodiscard]] QString statusText() const;

    /// True if @p text (a typed character) with these modifiers would be taken by the layer in
    /// the current state. Keys with Ctrl / Alt / Meta, Space, Return and the cursor keys are
    /// never taken (they stay ordinary shortcuts); Escape is taken only while something is
    /// pending or VISUAL is on.
    [[nodiscard]] bool wouldConsume(int key, Qt::KeyboardModifiers modifiers, const QString& text) const;
    /// Handles a key that wouldConsume() accepted. Returns whether it was taken.
    bool handleKey(int key, Qt::KeyboardModifiers modifiers, const QString& text);

    /// Drops a half-typed sequence and count (the focus moved away).
    void cancelPending();
    /// Back to NORMAL with nothing pending (selection of VISUAL cleared).
    void reset();

signals:
    void statusChanged();

private:
    void dispatch(QChar c);
    void second(QChar first, QChar c);
    void finish(); ///< clears count and pending after a command
    int count() const { return m_count > 0 ? m_count : 1; }
    void repeat(const char* actionId, int times);
    bool motion(QChar c);
    void afterMotion();
    void enterVisual();
    void leaveVisual(bool clearSelection);

    VimHost* m_host = nullptr;
    Mode m_mode = Mode::Normal;
    int m_count = 0; ///< 0 = none typed
    QString m_pending; ///< count digits and the first key of a sequence
    QChar m_first; ///< the first key of a sequence being typed
    int64_t m_anchor = 0; ///< VISUAL: the frame where v was typed
    std::map<QChar, int64_t> m_marks; ///< m{a-z}
};

class VimInput : public QObject
{
    Q_OBJECT
public:
    /// The keyboard profile id this layer follows.
    static constexpr char kProfile[] = "vim";
    /// Property (bool) of the widgets whose focus turns the layer on (timelines, viewers).
    static constexpr char kScopeProperty[] = "kkVimScope";

    /// Follows the active profile of @p actions. Installs itself on the application.
    explicit VimInput(ActionRegistry* actions, QObject* parent = nullptr);
    ~VimInput() override;

    void setHost(VimHost* host) { m_machine.setHost(host); m_host = host; }
    [[nodiscard]] VimMachine& machine() { return m_machine; }
    /// True while the Vim profile is the active one.
    [[nodiscard]] bool active() const { return m_active; }
    /// True if the layer takes keys for @p focus now (scope widget, no text field, no dialog).
    [[nodiscard]] bool inScope(const QWidget* focus) const;
    /// What the mode indicator shows: empty while the profile is not "vim".
    [[nodiscard]] QString statusText() const;

signals:
    void statusChanged(const QString& text);
    void activeChanged(bool active);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void followProfile();

    ActionRegistry* m_actions = nullptr;
    VimMachine m_machine;
    VimHost* m_host = nullptr;
    bool m_active = false;
};

} // namespace kk
