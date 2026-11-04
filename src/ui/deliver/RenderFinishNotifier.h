// SPDX-License-Identifier: GPL-3.0-or-later
// RenderFinishNotifier — a system notification when the render queue finishes, like
// HandBrake's (Resolve has none): "Render complete" with Open Folder / Play buttons.
#pragma once

#include "export/RenderQueue.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

namespace kk {

namespace platform {
class Notifier;
}

/// Watches a RenderQueue and sends one desktop notification per run (Render All, or one
/// job): from the first job that starts until the queue stops. Nothing is sent when the run
/// ended because the user cancelled (Stop, or cancelling the job that was rendering last), or
/// when Preferences > UI Settings > "Show a system notification when renders finish" is off.
/// The main window's taskbar entry is flashed (QApplication::alert) when it is not active.
class RenderFinishNotifier : public QObject
{
    Q_OBJECT
public:
    /// What a finished run reports.
    struct Summary
    {
        QString summary; ///< "Render complete" / "Render finished with errors"
        QString body; ///< "Timeline 1 rendered in 0:42", "1 of 3 failed"
        QString revealPath; ///< file shown by Open Folder (empty: no button)
        QString playPath; ///< the one rendered file for Play (empty: no button)
    };

    /// Action keys of the notification's buttons.
    static constexpr char kOpenFolderAction[] = "open-folder";
    static constexpr char kPlayAction[] = "play";

    /// @p window is flashed in the taskbar; the desktop notifier is created on first use.
    RenderFinishNotifier(RenderQueue* queue, QWidget* window, QObject* parent = nullptr);
    ~RenderFinishNotifier() override;

    /// Uses @p notifier (not owned; nullptr = no notifications) instead of the desktop's.
    void setNotifier(platform::Notifier* notifier);
    /// Replaces platform::showInFileManager (Open Folder) and platform::openUrl (Play).
    void setHandlers(std::function<void(const QString&)> revealInFileManager,
        std::function<void(const QString&)> openFile);

    /// The report of a run whose jobs finished in @p jobs (queue order); nullopt when the
    /// user cancelled it (the last job was cancelled) or it is empty.
    [[nodiscard]] static std::optional<Summary> summarize(const QList<RenderQueue::Job>& jobs);
    /// "0:42", "12:05", "1:02:03".
    [[nodiscard]] static QString formatDuration(double seconds);

private:
    void onJobFinished(quint64 id);
    void onRunningChanged(bool running);
    void runFinished();
    platform::Notifier* notifier();

    RenderQueue* m_queue;
    QPointer<QWidget> m_window;
    QList<RenderQueue::Job> m_run; ///< jobs finished in the current run
    std::unique_ptr<platform::Notifier> m_own;
    platform::Notifier* m_notifier = nullptr;
    bool m_notifierSet = false;
    std::function<void(const QString&)> m_reveal;
    std::function<void(const QString&)> m_open;
};

} // namespace kk
