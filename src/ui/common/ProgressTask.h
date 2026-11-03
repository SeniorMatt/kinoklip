// SPDX-License-Identifier: GPL-3.0-or-later
// ProgressTask — runs a long file operation (copying media into a single-file project, Pack,
// Unpack, Compact, Verify) on a worker thread behind a progress dialog with Cancel, so the
// window keeps repainting and large files never freeze the UI.
#pragma once

#include <QString>

#include <functional>

class QProgressDialog;
class QWidget;

namespace kk {

/// Progress of a background task: amount done of total (bytes, or steps). Returns false once
/// the user pressed Cancel; the task then stops at its next check. Thread-safe.
using TaskProgress = std::function<bool(qint64 done, qint64 total)>;

/// Runs @p work on a worker thread and waits for it while a window-modal progress dialog
/// (parented to @p parent, shown after 300 ms) shows @p label and the progress @p work
/// reports. A local event loop runs meanwhile (repaints, timers). Returns false if the user
/// pressed Cancel (what that means is up to @p work, which sees the callback return false).
/// GUI thread only.
bool runWithProgress(QWidget* parent, const QString& title, const QString& label,
    const std::function<void(const TaskProgress&)>& work);

/// Tests: called with every progress dialog right after it is created, before the work
/// starts (e.g. to press Cancel). An empty function removes the hook.
void setProgressDialogHookForTests(std::function<void(QProgressDialog*)> hook);

} // namespace kk
