// SPDX-License-Identifier: GPL-3.0-or-later
// MemoryGuard — how much memory the machine still has, so a render can wait instead of being
// killed by the kernel's out-of-memory handler.
#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>

namespace kk::memguard {

/// "MemAvailable" of a /proc/meminfo text in bytes; -1 when the line is missing.
[[nodiscard]] int64_t availableBytesFromMeminfo(const QByteArray& meminfo);
/// Memory available now (no swapping needed) in bytes; -1 when unknown (not Linux).
[[nodiscard]] int64_t availableBytes();

/// A render pauses while less than this is available (and resumes at kResumeBytes).
inline constexpr int64_t kPauseBytes = 768LL << 20;
inline constexpr int64_t kResumeBytes = 1024LL << 20;

/// Pause decision with hysteresis: @p paused is the current state, the result the new one.
[[nodiscard]] bool shouldPause(int64_t availableBytes, bool paused);

/// Blocks while memory is short. Calls @p onChange(true) when it starts waiting and
/// onChange(false) when it goes on; checks @p cancelled every 250 ms and returns false when
/// it is set. Returns true when the caller may go on. @p availableFn defaults to availableBytes.
bool waitForMemory(const std::function<bool()>& cancelled,
    const std::function<void(bool paused)>& onChange,
    const std::function<int64_t()>& availableFn = {});

/// A render that started and never finished leaves a marker file (process id + output file) in
/// the application data folder; the next start reports it ("the render was killed, probably
/// out of memory"). markRenderStart/End bracket a render; takeInterruptedRender() returns the
/// output path of an interrupted one (and removes the marker) or an empty string. A marker
/// whose process still runs (another window rendering) is left alone.
void markRenderStart(const QString& outputPath);
void markRenderEnd();
[[nodiscard]] QString takeInterruptedRender(const QString& markerPath = {});
[[nodiscard]] QString renderMarkerPath();

} // namespace kk::memguard
