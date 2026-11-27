// SPDX-License-Identifier: GPL-3.0-or-later
// CloneToolDialog — Media page > Clone Tool (Resolve: Clone Tool): copies camera cards and
// folders to one or more destinations and verifies every copy with a checksum (spec 196).
// The work is media/CloneTool.h, run on a worker thread so the window stays alive.
#pragma once

#include "media/CloneTool.h"

#include <QDialog>

#include <atomic>
#include <future>

class QComboBox;
class QCheckBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTimer;

namespace kk {

/// Not modal: the Media page stays usable while a clone runs. Thread-safety: GUI thread only
/// (the job runs on one worker thread and talks to the dialog through atomics).
class CloneToolDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CloneToolDialog(QWidget* parent = nullptr);
    ~CloneToolDialog() override;

    void addSource(const QString& path);
    void addDestination(const QString& path);
    [[nodiscard]] clone::Job job() const;

    /// Starts the job (the Clone button). Returns false when sources or destinations are missing.
    bool start();
    /// Cancels a running job.
    void cancel();
    [[nodiscard]] bool running() const { return m_future.valid(); }
    /// The result of the last job; the dialog emits finished() when it is in.
    [[nodiscard]] const clone::Result& result() const { return m_result; }

signals:
    void cloneFinished(bool ok);

private:
    void pollJob();
    void updateButtons();

    QListWidget* m_sources = nullptr;
    QListWidget* m_destinations = nullptr;
    QComboBox* m_checksum = nullptr;
    QCheckBox* m_verify = nullptr;
    QPushButton* m_clone = nullptr;
    QPushButton* m_cancel = nullptr;
    QProgressBar* m_progress = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QTimer* m_poll = nullptr;

    std::future<clone::Result> m_future;
    std::atomic<qint64> m_done {0};
    std::atomic<qint64> m_total {0};
    std::atomic<bool> m_cancelled {false};
    clone::Result m_result;
};

} // namespace kk
