// SPDX-License-Identifier: GPL-3.0-or-later
// ProgressTask implementation.
#include "ui/common/ProgressTask.h"

#include <QEventLoop>
#include <QFutureWatcher>
#include <QProgressDialog>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <memory>

namespace kk {

namespace {
std::function<void(QProgressDialog*)>& dialogHook()
{
    static std::function<void(QProgressDialog*)> hook;
    return hook;
}

/// The dialog's range: done / total scaled to 0..kSteps (byte counts exceed int).
constexpr int kSteps = 1000;
} // namespace

void setProgressDialogHookForTests(std::function<void(QProgressDialog*)> hook)
{
    dialogHook() = std::move(hook);
}

bool runWithProgress(QWidget* parent, const QString& title, const QString& label,
    const std::function<void(const TaskProgress&)>& work)
{
    struct State
    {
        std::atomic<qint64> done {0};
        std::atomic<qint64> total {0};
        std::atomic<bool> cancel {false};
    };
    auto state = std::make_shared<State>();

    QProgressDialog dialog(label, QObject::tr("Cancel"), 0, kSteps, parent);
    dialog.setWindowTitle(title);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(300);
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    dialog.setValue(0);
    QObject::connect(&dialog, &QProgressDialog::canceled, &dialog, [state, &dialog] {
        state->cancel = true;
        dialog.setLabelText(QObject::tr("Cancelling…"));
    });
    if (dialogHook())
        dialogHook()(&dialog);
    if (dialog.wasCanceled()) // cancel() sets the flag without emitting canceled()
        state->cancel = true;

    const TaskProgress progress = [state](qint64 done, qint64 total) {
        state->done = done;
        state->total = total;
        return !state->cancel.load();
    };
    QFutureWatcher<void> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
    QTimer tick;
    tick.setInterval(50);
    QObject::connect(&tick, &QTimer::timeout, &dialog, [state, &dialog] {
        if (dialog.wasCanceled())
            state->cancel = true;
        const qint64 total = state->total.load();
        const qint64 done = state->done.load();
        if (total > 0 && !state->cancel.load())
            dialog.setValue(int(std::min<qint64>(kSteps, done * kSteps / total)));
    });
    watcher.setFuture(QtConcurrent::run([work, progress] { work(progress); }));
    tick.start();
    if (!watcher.isFinished())
        loop.exec(); // input reaches the dialog (Cancel); the window below is blocked by it
    watcher.waitForFinished();
    tick.stop();
    dialog.reset();
    return !state->cancel.load();
}

} // namespace kk
