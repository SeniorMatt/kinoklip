// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectSettingsDialog — project name, timeline resolution, frame rate and sample rate.
#pragma once

#include "core/Project.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace kk {

/// Edits ProjectSettings. OK applies them with Project::setSettings(), which makes resolution
/// and frame-rate changes one undoable step (the name change marks the project modified).
///
/// Resolution presets (UHD, QHD, HD, 720p, vertical, square) fill width/height; "Custom"
/// unlocks the spin boxes. Frame rates: 23.976, 24, 25, 29.97, 30, 50, 59.94, 60.
/// The sample rate is fixed at 48 kHz in v0.1 (the audio mixer's rate, PLAN 5.1); the combo
/// shows it so the setting has a home once other rates are supported.
class ProjectSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    /// Creates the dialog filled from @p project's current settings.
    explicit ProjectSettingsDialog(Project* project, QWidget* parent = nullptr);

    /// Settings as currently entered.
    [[nodiscard]] ProjectSettings settings() const;
    /// Fills the controls from @p settings (selects a matching preset or Custom).
    void setSettings(const ProjectSettings& settings);
    /// Applies the entered settings to the project (one undo step if the timeline changes).
    void apply();

    /// Frame rates offered, in UI order.
    [[nodiscard]] static QVector<Rational> frameRates();

    void accept() override;

private:
    void presetChanged(int index);
    void updateWarning();

    Project* m_project = nullptr;
    QLineEdit* m_name = nullptr;
    QComboBox* m_preset = nullptr;
    QSpinBox* m_width = nullptr;
    QSpinBox* m_height = nullptr;
    QComboBox* m_fps = nullptr;
    QComboBox* m_sampleRate = nullptr;
    QLabel* m_warning = nullptr;
};

} // namespace kk
