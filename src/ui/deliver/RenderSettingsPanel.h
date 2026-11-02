// SPDX-License-Identifier: GPL-3.0-or-later
// RenderSettingsPanel — the Deliver page's left panel, structured like Resolve's Render
// Settings: preset row, file name + location, single / individual clips, Video / Audio / File
// tabs, render range and "Add to Render Queue". Edits an ExportSettings value.
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
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTabWidget;

namespace kk {

struct AppContext;
class ExportPresets;
class RenderPresetRow;

/// Render settings form. The widgets are a view of one ExportSettings value: every change
/// rebuilds the value, normalises it (codec allowed by the container, quality in range) and
/// writes it back, so impossible combinations cannot be selected. Options the export engine
/// cannot honour yet (individual clips, data levels, colour tags, channel layouts) are shown
/// disabled with a tooltip saying why, never faked.
///
/// The preset row lists ExportPresets (built-in + user); clicking one applies it on top of the
/// current file name, location and range. The title-bar menu saves/deletes user presets.
/// File naming options (timeline name, subfolder, unique names) are remembered in QSettings
/// (group "deliver/").
class RenderSettingsPanel : public PanelFrame
{
    Q_OBJECT
public:
    /// @p presets must outlive the panel.
    RenderSettingsPanel(AppContext& ctx, ExportPresets* presets, QWidget* parent = nullptr);

    /// Current settings (output path = location [/ subfolder] / file name + extension).
    [[nodiscard]] ExportSettings settings() const;
    /// Shows @p settings in the widgets.
    void setSettings(const ExportSettings& settings);
    /// Applies a preset by id (no-op for unknown ids).
    void applyPreset(const QString& id);
    /// Id of the preset last applied (empty after a manual change).
    [[nodiscard]] QString currentPresetId() const { return m_presetId; }

    /// Detected hardware encoders: annotates the Encoder combo ("not available" entries).
    void setHardwareStatus(const QList<HwEncoderStatus>& status);
    /// Default output folder and base name (e.g. after a project was opened).
    void setDefaultOutput(const QString& folder, const QString& baseName);

    /// Quality presets of the "Automatic" quality mode, Resolve style.
    enum class QualityLevel {
        Best,
        High,
        Medium,
        Low,
        Custom
    };
    /// Constant-quality value (CRF / CQ / qscale) of @p level for @p codecId (Custom: -1).
    [[nodiscard]] static int qualityValue(const QString& codecId, QualityLevel level);

Q_SIGNALS:
    /// The user pressed "Add to Render Queue".
    void addToQueueRequested();
    /// Any setting changed.
    void settingsChanged();

private:
    QWidget* buildHeader();
    QWidget* buildVideoTab();
    QWidget* buildAudioTab();
    QWidget* buildFileTab();
    void rebuildPresetRow();
    void refresh(); ///< settings() → normalise → setSettings()
    void updateVisibility();
    void updateResultPath();
    void updateFileName();
    void browseLocation();
    void savePreset();
    void deletePreset();
    void storeFileOptions() const;
    [[nodiscard]] QString timelineName() const;

    AppContext& m_ctx;
    ExportPresets* m_presets;
    ExportSettings m_settings; ///< last value written to the widgets
    QString m_presetId;
    bool m_updating = false;
    QList<HwEncoderStatus> m_hwStatus;

    RenderPresetRow* m_presetRow = nullptr;
    QLabel* m_presetName = nullptr;
    QLineEdit* m_fileName = nullptr;
    QLineEdit* m_location = nullptr;
    QRadioButton* m_singleClip = nullptr;
    QRadioButton* m_individualClips = nullptr;
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
    QComboBox* m_frameRate = nullptr;
    QWidget* m_qualityBox = nullptr;
    QRadioButton* m_qualityAuto = nullptr;
    QRadioButton* m_qualityRestrict = nullptr;
    QComboBox* m_qualityLevel = nullptr;
    QSpinBox* m_quality = nullptr;
    QSpinBox* m_bitrate = nullptr;
    QComboBox* m_speed = nullptr;
    QWidget* m_keyframeBox = nullptr;
    QRadioButton* m_keyframesAuto = nullptr;
    QRadioButton* m_keyframesEvery = nullptr;
    QSpinBox* m_keyframeInterval = nullptr;
    QCheckBox* m_frameReordering = nullptr;
    QComboBox* m_dataLevels = nullptr;
    QComboBox* m_colorSpace = nullptr;
    QComboBox* m_gamma = nullptr;
    QCheckBox* m_alpha = nullptr;
    // Audio
    QFormLayout* m_audioForm = nullptr;
    QCheckBox* m_audioEnabled = nullptr;
    QComboBox* m_audioCodec = nullptr;
    QComboBox* m_sampleRate = nullptr;
    QComboBox* m_bitDepth = nullptr;
    QComboBox* m_channels = nullptr;
    QComboBox* m_audioBitrate = nullptr;
    // File
    QRadioButton* m_nameCustom = nullptr;
    QRadioButton* m_nameTimeline = nullptr;
    QLabel* m_extension = nullptr;
    QCheckBox* m_useSubfolder = nullptr;
    QLineEdit* m_subfolder = nullptr;
    QCheckBox* m_uniqueNames = nullptr;
    QLabel* m_resultPath = nullptr;
    // File > Subtitle settings
    QFormLayout* m_fileForm = nullptr;
    QCheckBox* m_subtitleExport = nullptr; ///< "Export subtitle"
    QComboBox* m_subtitleMode = nullptr; ///< burn in / separate file / embedded
    QComboBox* m_subtitleFormat = nullptr; ///< separate file: SRT / WebVTT
    QComboBox* m_subtitleCodec = nullptr; ///< embedded: the container's subtitle codecs
    // Range
    QComboBox* m_range = nullptr;
    QPushButton* m_addButton = nullptr;
};

} // namespace kk
