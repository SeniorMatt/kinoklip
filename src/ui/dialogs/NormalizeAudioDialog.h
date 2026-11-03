// SPDX-License-Identifier: GPL-3.0-or-later
// NormalizeAudioDialog — Clip > Normalize Audio Levels: mode, target level, relative or
// independent levelling; measures the clips off the GUI thread and sets their volume as one
// undo step (spec 37.40).
#pragma once

#include "render/AudioNormalize.h"

#include <QDialog>
#include <QFuture>
#include <QVector>

#include <atomic>
#include <memory>

class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QRadioButton;
class QTimer;

namespace kk {

class Project;
class SelectionModel;

/// The audio clips Clip > Normalize Audio Levels acts on: the selected audio clips of media
/// and the audio clips linked to selected video clips, on unlocked tracks, in timeline order.
[[nodiscard]] QVector<quint64> normalizeTargets(
    const Project& project, const SelectionModel& selection);

/// Normalize Audio Levels dialog. Normalize starts the analysis: a worker thread measures
/// every clip's source range (audionorm::measure()) while a progress bar follows it; Cancel
/// stops the worker (the timeline is left untouched). When the measurements are in, the clip
/// volumes are set (levelops::setClipGain()) in one undo step "Normalize Audio Levels" and the
/// dialog closes with Accepted.
///
/// Thread-safety: GUI thread; the worker only reads its own copies of the clips' sources.
class NormalizeAudioDialog : public QDialog
{
    Q_OBJECT
public:
    /// Dialog for clips @p clipIds of @p project's active timeline.
    NormalizeAudioDialog(Project* project, QVector<quint64> clipIds, QWidget* parent = nullptr);
    /// Waits for a running analysis (after cancelling it).
    ~NormalizeAudioDialog() override;

    /// Selects the normalization mode (the target resets to the mode's default).
    void setMode(audionorm::Mode mode);
    [[nodiscard]] audionorm::Mode mode() const;
    /// Target level in the mode's unit (dBFS, dBTP, LUFS).
    void setTarget(double level);
    [[nodiscard]] double target() const;
    /// Relative or Independent (only meaningful with several clips).
    void setScope(audionorm::Scope scope);
    [[nodiscard]] audionorm::Scope scope() const;

    /// The Normalize button: starts measuring (no-op while running).
    void start();
    /// True while the worker measures.
    [[nodiscard]] bool isRunning() const;
    /// Measurements of the last finished analysis, one per clip (in constructor order).
    [[nodiscard]] const std::vector<audionorm::Measurement>& measurements() const
    {
        return m_results;
    }

public slots:
    /// Cancel / Esc / close: stops a running analysis, then closes with Rejected.
    void reject() override;

private:
    void updateUnit();
    void pollProgress();
    void finishAnalysis();

    Project* m_project;
    QVector<quint64> m_clips;
    std::vector<audionorm::Source> m_sources; ///< parallel to m_clips (empty path = no media)
    QComboBox* m_mode = nullptr;
    QDoubleSpinBox* m_target = nullptr;
    QRadioButton* m_relative = nullptr;
    QRadioButton* m_independent = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_status = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QTimer* m_poll = nullptr;
    QFuture<std::vector<audionorm::Measurement>> m_future;
    /// Shared with the worker: it may outlive a cancelled dialog by one block.
    struct Shared
    {
        std::atomic<bool> cancel {false};
        std::atomic<int> permille {0};
    };
    std::shared_ptr<Shared> m_shared;
    std::vector<audionorm::Measurement> m_results;
};

} // namespace kk
