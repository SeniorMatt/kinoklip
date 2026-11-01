// SPDX-License-Identifier: GPL-3.0-or-later
// ShuttleControl — J/K/L state machine.
#include "playback/ShuttleControl.h"

#include <algorithm>

namespace kk {

double ShuttleControl::nextForwardRate(double currentRate)
{
    // From pause, slow play or reverse: start at real time. Otherwise double, capped.
    if (currentRate < 1.0) {
        return 1.0;
    }
    return std::min(kMaxRate, currentRate * 2.0);
}

double ShuttleControl::nextReverseRate(double currentRate)
{
    if (currentRate > -1.0) {
        return -1.0;
    }
    return std::max(-kMaxRate, currentRate * 2.0);
}

ShuttleControl::Command ShuttleControl::trigger(Key key, double currentRate, int64_t nowMs)
{
    Command cmd;
    if (key == Key::K) {
        m_pendingDir = 0;
        m_slowDir = 0;
        cmd.kind = Command::Kind::SetRate;
        cmd.rate = 0.0;
        return cmd;
    }
    const int dir = key == Key::L ? 1 : -1;
    if (m_held[int(Key::K)]) {
        // K held: a tap steps one frame; keeping the key down turns into slow play (poll()).
        m_slowDir = 0;
        m_pendingDir = dir;
        m_deadlineMs = nowMs + kHoldMs;
        cmd.kind = Command::Kind::Step;
        cmd.step = dir;
        return cmd;
    }
    m_pendingDir = 0;
    m_slowDir = 0;
    cmd.kind = Command::Kind::SetRate;
    cmd.rate = dir > 0 ? nextForwardRate(currentRate) : nextReverseRate(currentRate);
    return cmd;
}

ShuttleControl::Command ShuttleControl::setKeyHeld(Key key, bool held, int64_t nowMs)
{
    (void)nowMs; // part of the API for symmetry; releases act immediately
    m_held[int(key)] = held;
    Command cmd;
    if (held) {
        return cmd;
    }
    // Releasing K or the direction key ends a pending hold and stops slow play.
    const int dir = key == Key::L ? 1 : key == Key::J ? -1 : 0;
    if (key == Key::K || dir == m_pendingDir) {
        m_pendingDir = 0;
    }
    if (m_slowDir != 0 && (key == Key::K || dir == m_slowDir)) {
        m_slowDir = 0;
        cmd.kind = Command::Kind::SetRate;
        cmd.rate = 0.0;
    }
    return cmd;
}

ShuttleControl::Command ShuttleControl::poll(int64_t nowMs)
{
    Command cmd;
    if (m_pendingDir == 0 || nowMs < m_deadlineMs) {
        return cmd;
    }
    const Key dirKey = m_pendingDir > 0 ? Key::L : Key::J;
    if (m_held[int(Key::K)] && m_held[int(dirKey)]) {
        m_slowDir = m_pendingDir;
        cmd.kind = Command::Kind::SetRate;
        cmd.rate = kSlowRate * m_slowDir;
    }
    m_pendingDir = 0;
    return cmd;
}

} // namespace kk
