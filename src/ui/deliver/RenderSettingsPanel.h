// SPDX-License-Identifier: GPL-3.0-or-later
// RenderSettingsPanel — the Deliver page's left panel: preset strip, Video / Audio / File tabs,
// render range and "Add to Render Queue". Edits an ExportSettings value.
#pragma once

#include "export/EncoderCatalog.h"
#include "export/ExportSettings.h"
#include "ui/common/PanelFrame.h"

#include <QList>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTabWidget;

namespace kk {

struct AppContext;
class ExportPresets;

/// Render settings form. The widgets are a view of one ExportSettings value: every change
/// rebuilds the value, normalises it (codec allowed by the container, quality in range) and
/// writes it back, so impossible combinations cannot be selected.
///
/// The preset strip lists ExportPresets (built-in + user); clicking one applies it on top of
/// the current file name, location and range. The title-bar menu saves/deletes user presets.
class RenderSettingsPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// @p presets must outlive the panel.
    RenderSettingsPanel(AppContext& ctx, ExportPresets* presets, QWidget* parent = nullptr);

    /// Current settings (output path = location / file name + container extension).
    [[nodiscard]] ExportSettings settings() const;
    /// Shows @p settings in the widgets.
    void setSettings(const ExportSettings& settings);
    /// Applies a preset by id (no-op for unknown ids).
    void applyPreset(const QString& id);
    /// Id of the preset last applied (empty after manual changes are not tracked).
    [[nodiscard]] QString currentPresetId() const { return m_presetId; }

    /// Detected hardware encoders: annotates the Encoder combo ("unavailable" entries).
    void setHardwareStatus(const QList<HwEncoderStatus>& status);
    /// Default output folder and base name (e.g. after a project was opened).
    void setDefaultOutput(const QString& folder, const QString& baseName);

Q_SIGNALS:
    /// The user pressed "Add to Render Queue".
    void addToQueueRequested();
    /// Any setting changed.
    void settingsChanged();

private:
    QWidget* buildVideoTab();
    QWidget* buildAudioTab();
    QWidget* buildFileTab();
    void rebuildPresetStrip();
    void refresh(); ///< settings() → normalise → setSettings()
    void updateVisibility();
    void updateResultPath();
    void browseLocation();
    void savePreset();
    void deletePreset();

    AppContext& m_ctx;
    ExportPresets* m_presets;
    ExportSettings m_settings; ///< last value written to the widgets
    QString m_presetId;
    bool m_updating = false;
    QList<HwEncoderStatus> m_hwStatus;

    QListWidget* m_presetList = nullptr;
    QTabWidget* m_tabs = nullptr;
    // Video
    QFormLayout* m_videoForm = nullptr;
    QCheckBox* m_videoEnabled = nullptr;
    QComboBox* m_format = nullptr;
    QComboBox* m_codec = nullptr;
    QComboBox* m_encoder = nullptr;
    QComboBox* m_profile = nullptr;
    QComboBox* m_resolution = nullptr;
    QSpinBox* m_width = nullptr;
    QSpinBox* m_height = nullptr;
    QWidget* m_sizeRow = nullptr;
    QLabel* m_frameRate = nullptr;
    QComboBox* m_rateControl = nullptr;
    QSpinBox* m_quality = nullptr;
    QSpinBox* m_bitrate = nullptr;
    QComboBox* m_speed = nullptr;
    QCheckBox* m_alpha = nullptr;
    // Audio
    QFormLayout* m_audioForm = nullptr;
    QCheckBox* m_audioEnabled = nullptr;
    QComboBox* m_audioCodec = nullptr;
    QComboBox* m_audioBitrate = nullptr;
    QComboBox* m_sampleRate = nullptr;
    QComboBox* m_bitDepth = nullptr;
    // File
    QLineEdit* m_fileName = nullptr;
    QLineEdit* m_location = nullptr;
    QLabel* m_resultPath = nullptr;
    // Range
    QRadioButton* m_rangeEntire = nullptr;
    QRadioButton* m_rangeInOut = nullptr;
    QPushButton* m_addButton = nullptr;
};

} // namespace kk
