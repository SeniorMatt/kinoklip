// SPDX-License-Identifier: GPL-3.0-or-later
// PreferencesDialog — user preferences: hardware acceleration, autosave, appearance.
#pragma once

#include "core/Preferences.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;

namespace kk {

class ActionRegistry;

namespace prefs {
/// UI scale factor applied at start-up ("auto" or a factor such as "1.25"). Stored here until
/// core/Preferences.h gains it; main() must read it before creating QApplication
/// (QT_SCALE_FACTOR) — see src/ui/dialogs/readme.md.
inline constexpr char kUiScale[] = "ui/scaleFactor";
} // namespace prefs

/// Edits the preferences stored in QSettings (keys in core/Preferences.h):
/// - Playback: hardware acceleration (Auto / Off / VA-API / CUDA / QSV / Vulkan, key
///   `playback/hwaccel`), with the backends that work on this machine listed next to it
///   (media::hwCapabilities(), probed on a worker thread because opening devices is slow).
/// - Project: autosave interval in minutes (0 = off).
/// - Appearance: use system colours (applied immediately through the
///   `view.use_system_colors` action), UI scale (applies after a restart).
///
/// OK stores everything, pushes the hardware choice to the media module
/// (applyHwAccelPreference()) and emits preferencesChanged().
class PreferencesDialog : public QDialog
{
    Q_OBJECT
public:
    /// Creates the dialog filled from QSettings. @p actions (may be null) provides the
    /// "use system colours" action so the theme switch goes through the app's handler.
    explicit PreferencesDialog(ActionRegistry* actions, QWidget* parent = nullptr);

    /// Stores the entered values.
    void apply();
    void accept() override;

signals:
    /// The preferences were stored (listeners re-read them, e.g. the autosave timer).
    void preferencesChanged();

private:
    void showCapabilities(const QString& text);

    ActionRegistry* m_actions = nullptr;
    QComboBox* m_hwAccel = nullptr;
    QLabel* m_hwInfo = nullptr;
    QSpinBox* m_autosave = nullptr;
    QCheckBox* m_systemColors = nullptr;
    QComboBox* m_scale = nullptr;
};

/// Makes the media module use the stored hardware acceleration preference for new decoders
/// (QSV maps to VA-API: Intel's Linux decode path). Call at start-up and after changes.
void applyHwAccelPreference();

} // namespace kk
