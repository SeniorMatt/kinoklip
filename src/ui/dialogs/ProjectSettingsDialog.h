// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectSettingsDialog — Resolve-style project settings: a section list (Master Settings,
// Image Scaling, Audio, General Options, Optimized Media and Render Cache) and the section's
// settings.
#pragma once

#include "core/Project.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QStackedWidget;

namespace kk {

/// Edits ProjectSettings. Save applies them with Project::setSettings(), which makes
/// resolution and frame-rate changes one undoable step (the name change marks the project
/// modified).
///
/// Sections (left list, like Resolve):
/// - Master Settings: timeline resolution presets (UHD, QHD, HD, 720p, vertical, square) or
///   Custom, pixel aspect ratio (square only), timeline frame rate (23.976 ... 60), playback
///   frame rate (= timeline), frame interpolation (the default retime process of retimed
///   clips: Nearest, Frame Blend, Optical Flow), video monitoring (no output devices yet).
/// - Image Scaling: mismatched resolution handling ("scale entire image to fit", the
///   compositor's only mode for now), output scaling (= timeline).
/// - Audio: sample rate 48 kHz and 32-bit float mixing (the mixer's fixed format, PLAN 5.1).
/// - General Options: project name, autosave interval (the user preference
///   `project/autosaveMinutes`, shared with Preferences > Project Save and Load).
/// - Render Cache: render cache / optimized media placeholders.
/// Settings the engine cannot change yet are shown disabled with a tooltip saying why.
class ProjectSettingsDialog : public QDialog
{
    Q_OBJECT
public:
    /// Creates the dialog filled from @p project's current settings.
    explicit ProjectSettingsDialog(Project* project, QWidget* parent = nullptr);

    /// Settings as currently entered.
    [[nodiscard]] ProjectSettings settings() const;
    /// Optimized media / proxy / render cache settings currently shown.
    [[nodiscard]] MediaCacheSettings cacheSettings() const;
    /// Fills the controls from @p settings (selects a matching preset or Custom).
    void setSettings(const ProjectSettings& settings);
    /// Applies the entered settings to the project (one undo step if the timeline changes).
    void apply();

    /// Frame rates offered, in UI order.
    [[nodiscard]] static QVector<Rational> frameRates();

    void accept() override;
    /// Shows the section @p sectionId ("master", "scaling", "audio", "general", "cache").
    bool showSection(const QString& sectionId);

signals:
    /// The autosave interval (a user preference) was stored.
    void preferencesChanged();

private:
    QWidget* buildMaster();
    QWidget* buildScaling();
    QWidget* buildAudio();
    QWidget* buildGeneral();
    QWidget* buildCache();
    void addSection(const QString& id, const QString& title, QWidget* page);

    void presetChanged(int index);
    void updateWarning();

    Project* m_project = nullptr;
    QLineEdit* m_name = nullptr;
    QComboBox* m_preset = nullptr;
    QSpinBox* m_width = nullptr;
    QSpinBox* m_height = nullptr;
    QComboBox* m_fps = nullptr;
    QComboBox* m_sampleRate = nullptr;
    QComboBox* m_playbackFps = nullptr;
    QComboBox* m_retimeProcess = nullptr; ///< Frame Interpolation > Retime process
    QSpinBox* m_autosave = nullptr;
    QLabel* m_warning = nullptr;
    QComboBox* m_proxyResolution = nullptr;
    QComboBox* m_proxyFormat = nullptr;
    QComboBox* m_optimizedResolution = nullptr;
    QComboBox* m_optimizedFormat = nullptr;
    QComboBox* m_cacheFormat = nullptr;
    QCheckBox* m_backgroundCache = nullptr;
    QSpinBox* m_backgroundDelay = nullptr;
    QCheckBox* m_userTransitions = nullptr;
    QCheckBox* m_userComposites = nullptr;
    QListWidget* m_sections = nullptr;
    QStackedWidget* m_stack = nullptr;
};

} // namespace kk
