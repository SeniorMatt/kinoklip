// SPDX-License-Identifier: GPL-3.0-or-later
// ShuttleControl — the J/K/L shuttle state machine (pure logic, time injected, unit-tested).
#pragma once

#include <cstdint>

namespace kk {

/// Turns J/K/L key presses into playback commands, the way NLE editors expect:
///
/// | Input                         | Result                                                   |
/// |-------------------------------|----------------------------------------------------------|
/// | L                             | play forward 1x; again 2x, 4x, 8x (capped); from reverse: 1x |
/// | J                             | the same in reverse (-1x, -2x, -4x, -8x)                  |
/// | K                             | pause                                                    |
/// | K held + tap L / J            | step one frame forward / back                            |
/// | K held + hold L / J           | after kHoldMs: slow play at +/- kSlowRate until released |
///
/// The "held" information comes from key events (setKeyHeld(), fed by an event filter that
/// watches the physical keys); trigger() is called when the action fires (key press or menu).
/// Without held information (menu use) J/K/L simply shuttle.
///
/// Thread-safety: none needed (GUI thread). Time is passed in as milliseconds of any
/// monotonic clock so tests can drive it deterministically.
class ShuttleControl
{
public:
    /// The three shuttle keys.
    enum class Key {
        J,
        K,
        L
    };

    /// What the engine should do after an input.
    struct Command
    {
        enum class Kind {
            None, ///< nothing changes
            SetRate, ///< play at `rate` (0 = pause)
            Step ///< pause and move by `step` frames
        };
        Kind kind = Kind::None;
        double rate = 0.0;
        int step = 0;
    };

    static constexpr double kMaxRate = 8.0; ///< fastest shuttle speed (multiple of real time)
    static constexpr double kSlowRate = 0.5; ///< K + held L/J speed
    static constexpr int kHoldMs = 300; ///< L/J held this long with K down -> slow play

    /// The action of @p key fired while the engine runs at @p currentRate (0 = paused).
    Command trigger(Key key, double currentRate, int64_t nowMs);
    /// Physical key state from the event filter (auto-repeat events must be filtered out).
    Command setKeyHeld(Key key, bool held, int64_t nowMs);
    /// Promotes a held K+L/K+J step to slow play once kHoldMs passed. Call from a timer
    /// (holdDeadlineMs() says when); returns Kind::None while nothing is due.
    Command poll(int64_t nowMs);
    /// Time (ms) at which poll() will have something to do, or -1.
    [[nodiscard]] int64_t holdDeadlineMs() const { return m_pendingDir != 0 ? m_deadlineMs : -1; }
    /// True while K is physically held.
    [[nodiscard]] bool isKHeld() const { return m_held[int(Key::K)]; }
    /// True while K+L/J slow play (started by this class) is active.
    [[nodiscard]] bool isSlowPlaying() const { return m_slowDir != 0; }

    /// Rate after pressing L at @p currentRate (1, 2, 4, 8; from <= 0 back to 1).
    [[nodiscard]] static double nextForwardRate(double currentRate);
    /// Rate after pressing J at @p currentRate (-1, -2, -4, -8; from >= 0 back to -1).
    [[nodiscard]] static double nextReverseRate(double currentRate);

private:
    bool m_held[3] = {false, false, false};
    int m_pendingDir = 0; ///< +1/-1: a K+L/J step waiting to become slow play at m_deadlineMs
    int64_t m_deadlineMs = 0;
    int m_slowDir = 0; ///< +1/-1 while slow play started by K+L/J runs
};

} // namespace kk
