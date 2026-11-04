// SPDX-License-Identifier: GPL-3.0-or-later
// RenderSettingsPanel — widgets <-> ExportSettings, laid out like Resolve's Render Settings.
#include "ui/deliver/RenderSettingsPanel.h"

#include "core/Project.h"
#include "export/ExportPresets.h"
#include "platform/FileDialogs.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"
#include "ui/common/Metrics.h"
#include "ui/deliver/RenderPresetRow.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {

/// Resolution choices of the Video tab: {width, height, label}; 0x0 = timeline, -1 = custom.
struct ResolutionChoice
{
    int width;
    int height;
    const char* label;
};
constexpr ResolutionChoice kResolutions[] = {
    {0, 0, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "Match timeline")},
    {3840, 2160, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "3840 x 2160 Ultra HD")},
    {2560, 1440, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "2560 x 1440 QHD")},
    {1920, 1080, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "1920 x 1080 HD")},
    {1280, 720, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "1280 x 720 HD")},
    {1080, 1920, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "1080 x 1920 Vertical HD")},
    {1080, 1080, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "1080 x 1080 Square")},
    {960, 540, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "960 x 540")},
    {640, 360, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "640 x 360")},
    {-1, -1, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "Custom")},
};
constexpr int kCustomResolution = int(std::size(kResolutions)) - 1;

constexpr int kAudioBitrates[] = {96, 128, 160, 192, 256, 320};
constexpr int kMaxBitrateKbps = 800000; ///< spin box limit (800 Mb/s)
constexpr int kMaxKeyframeInterval = 1000; ///< frames
constexpr int kDefaultKeyframeInterval = 50; ///< shown when switching to "Every"
/// Constant-quality values of the levels Best / High / Medium / Low per codec (lower =
/// better for every codec here). Chosen so the built-in presets land on named levels
/// (YouTube H.264 18 = High, H.265 24 = High, AV1 / VP9 32 = High, JPEG 2 = Best).
struct QualityLevels
{
    const char* codec;
    int values[4];
};
constexpr QualityLevels kQualityLevels[] = {
    {"h264", {16, 18, 23, 28}},
    {"hevc", {20, 24, 28, 32}},
    {"av1", {24, 32, 38, 45}},
    {"vp9", {24, 32, 38, 45}},
    {"mjpeg", {2, 4, 8, 15}},
};
/// Codecs not in the table: offsets from the codec's default value.
constexpr int kQualityOffsets[] = {-4, 0, 4, 9};

// QSettings keys of the file naming options (UI state of this panel, not export settings).
constexpr char kNameUsesTimelineKey[] = "deliver/fileNameUsesTimeline";
constexpr char kUseSubfolderKey[] = "deliver/useSubfolder";
constexpr char kSubfolderKey[] = "deliver/subfolder";
constexpr char kUniqueNamesKey[] = "deliver/uniqueFileNames";

bool hasHardwareEncoders(const QString& codec)
{
    return codec == QLatin1String("h264") || codec == QLatin1String("hevc")
        || codec == QLatin1String("av1") || codec == QLatin1String("vp9");
}

QString categoryIcon(const QString& category)
{
    if (category == QLatin1String("Audio"))
        return QStringLiteral("audio");
    if (category == QLatin1String("Images"))
        return QStringLiteral("image");
    if (category == QLatin1String("User"))
        return QStringLiteral("marker");
    return QStringLiteral("film");
}

void selectData(QComboBox* combo, const QVariant& data)
{
    const int index = combo->findData(data);
    if (index >= 0)
        combo->setCurrentIndex(index);
}

/// A form with the spacing scale and labels in one aligned column.
QFormLayout* makeForm(QWidget* page)
{
    auto* form = new QFormLayout(page);
    form->setContentsMargins(
        metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM);
    form->setHorizontalSpacing(metrics::kSpaceM);
    form->setVerticalSpacing(metrics::kSpaceS + metrics::kSpaceXs);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    return form;
}

/// Tab pages scroll vertically on small screens (1366x768) instead of squeezing rows.
QWidget* scrollable(QWidget* page)
{
    auto* scroll = new QScrollArea;
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return scroll;
}

/// A row of widgets for a form field (no extra margins).
QWidget* row(std::initializer_list<QWidget*> widgets, bool stretch = true)
{
    auto* w = new QWidget;
    auto* layout = new QHBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(metrics::kSpaceS);
    for (QWidget* child : widgets)
        layout->addWidget(child);
    if (stretch)
        layout->addStretch(1);
    return w;
}

/// A combo that shows a fixed value the engine does not let the user change yet.
QComboBox* fixedCombo(const QString& value, const QString& why)
{
    auto* combo = new QComboBox;
    combo->addItem(value);
    combo->setEnabled(false);
    combo->setToolTip(why);
    return combo;
}

} // namespace

int RenderSettingsPanel::qualityValue(const QString& codecId, QualityLevel level)
{
    const VideoCodecInfo* codec = findVideoCodec(codecId);
    if (!codec || !codec->hasQuality || level == QualityLevel::Custom)
        return -1;
    int value = codec->qualityDefault + kQualityOffsets[int(level)];
    for (const QualityLevels& levels : kQualityLevels) {
        if (codecId == QLatin1String(levels.codec))
            value = levels.values[int(level)];
    }
    return std::clamp(value, codec->qualityMin, codec->qualityMax);
}

RenderSettingsPanel::RenderSettingsPanel(AppContext& ctx, ExportPresets* presets, QWidget* parent)
    : PanelFrame(tr("Render Settings"), parent)
    , m_ctx(ctx)
    , m_presets(presets)
{
    setPanelKey(QStringLiteral("render_settings")); // in-app help: panel:render_settings
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, metrics::kSpaceM);
    layout->setSpacing(0);

    // ---- Preset row (Resolve: tiles across the top, the preset name under them) ----
    m_presetRow = new RenderPresetRow;
    connect(m_presetRow, &RenderPresetRow::presetClicked, this, &RenderSettingsPanel::applyPreset);
    auto* presetBox = new QWidget;
    presetBox->setObjectName(QStringLiteral("RenderPresetBox"));
    auto* presetLayout = new QVBoxLayout(presetBox);
    presetLayout->setContentsMargins(
        metrics::kSpaceS, metrics::kSpaceS, metrics::kSpaceS, metrics::kSpaceS);
    presetLayout->setSpacing(metrics::kSpaceS);
    presetLayout->addWidget(m_presetRow);
    layout->addWidget(presetBox);

    auto* presetMenuButton = new QToolButton;
    presetMenuButton->setIcon(QIcon::fromTheme(QStringLiteral("overflow-menu")));
    presetMenuButton->setToolTip(tr("Preset options: save or delete user presets"));
    presetMenuButton->setPopupMode(QToolButton::InstantPopup);
    presetMenuButton->setAutoRaise(true);
    presetMenuButton->setFocusPolicy(Qt::NoFocus);
    auto* menu = new QMenu(presetMenuButton);
    menu->addAction(tr("Save as New Preset…"), this, &RenderSettingsPanel::savePreset);
    menu->addAction(tr("Delete Selected User Preset"), this, &RenderSettingsPanel::deletePreset);
    presetMenuButton->setMenu(menu);
    addTitleBarWidget(presetMenuButton);

    layout->addWidget(buildHeader());

    // ---- Tabs ----
    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(scrollable(buildVideoTab()), tr("Video"));
    m_tabs->addTab(scrollable(buildAudioTab()), tr("Audio"));
    m_tabs->addTab(scrollable(buildFileTab()), tr("File"));
    layout->addWidget(m_tabs, 1);

    // ---- Range + queue ----
    auto* bottom = new QHBoxLayout;
    bottom->setContentsMargins(metrics::kSpaceM, metrics::kSpaceM, metrics::kSpaceM, 0);
    bottom->setSpacing(metrics::kSpaceM);
    m_range = new QComboBox;
    m_range->setObjectName(QStringLiteral("RenderRange"));
    m_range->addItem(tr("Entire Timeline"), int(RangeMode::Entire));
    m_range->addItem(tr("In/Out Range"), int(RangeMode::InOut));
    m_range->setToolTip(tr("Part of the timeline to render"));
    auto* rangeLabel = new QLabel(tr("Render"));
    rangeLabel->setBuddy(m_range);
    bottom->addWidget(rangeLabel);
    bottom->addWidget(m_range);
    bottom->addStretch(1);
    m_cancelEdit = new QPushButton(tr("Cancel"));
    m_cancelEdit->setObjectName(QStringLiteral("RenderCancelEdit"));
    m_cancelEdit->setToolTip(tr("Stop editing the job and keep it as it was"));
    m_cancelEdit->hide();
    bottom->addWidget(m_cancelEdit);
    m_addButton = new QPushButton(icon(QStringLiteral("add")), tr("Add to Render Queue"));
    m_addButton->setObjectName(QStringLiteral("RenderAddToQueue"));
    m_addButton->setDefault(true);
    bottom->addWidget(m_addButton);
    layout->addLayout(bottom);
    connect(m_range, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(m_addButton, &QPushButton::clicked, this, [this] {
        if (m_editingJob)
            emit updateJobRequested();
        else
            emit addToQueueRequested();
    });
    connect(m_cancelEdit, &QPushButton::clicked, this, &RenderSettingsPanel::cancelEditRequested);

    setContentWidget(body);
    rebuildPresetRow();

    setDefaultOutput(QString(), QString());
    applyPreset(QStringLiteral("youtube_1080p"));
    if (m_presetId.isEmpty())
        setSettings(settings());
}

QWidget* RenderSettingsPanel::buildHeader()
{
    auto* header = new QWidget;
    auto* form = makeForm(header);
    m_presetName = new QLabel;
    m_presetName->setObjectName(QStringLiteral("RenderPresetName"));
    QFont bold = m_presetName->font();
    bold.setWeight(QFont::DemiBold);
    m_presetName->setFont(bold);
    form->addRow(m_presetName);

    m_fileName = new QLineEdit;
    m_fileName->setObjectName(QStringLiteral("RenderFileName"));
    form->addRow(tr("Filename"), m_fileName);
    m_location = new QLineEdit;
    m_location->setObjectName(QStringLiteral("RenderLocation"));
    auto* browse = new QPushButton(tr("Browse"));
    browse->setToolTip(tr("Choose the render location"));
    auto* locationRow = row({m_location, browse}, false);
    static_cast<QHBoxLayout*>(locationRow->layout())->setStretch(0, 1);
    form->addRow(tr("Location"), locationRow);
    connect(browse, &QPushButton::clicked, this, &RenderSettingsPanel::browseLocation);
    connect(m_fileName, &QLineEdit::textChanged, this, &RenderSettingsPanel::updateResultPath);
    connect(m_location, &QLineEdit::textChanged, this, &RenderSettingsPanel::updateResultPath);

    m_singleClip = new QRadioButton(tr("Single clip"));
    m_individualClips = new QRadioButton(tr("Individual clips"));
    m_individualClips->setObjectName(QStringLiteral("RenderIndividualClips"));
    m_singleClip->setChecked(true);
    m_individualClips->setEnabled(false);
    m_individualClips->setToolTip(
        tr("Rendering every timeline clip to its own file is coming in a later version."));
    form->addRow(tr("Render"), row({m_singleClip, m_individualClips}));
    return header;
}

QWidget* RenderSettingsPanel::buildVideoTab()
{
    auto* page = new QWidget;
    m_videoForm = makeForm(page);

    m_videoEnabled = new QCheckBox(tr("Export Video"));
    m_videoForm->addRow(m_videoEnabled);
    m_format = new QComboBox;
    m_format->setObjectName(QStringLiteral("RenderFormat"));
    for (const ContainerInfo& c : exportContainers())
        m_format->addItem(c.name, c.id);
    m_videoForm->addRow(tr("Format"), m_format);
    m_codec = new QComboBox;
    m_codec->setObjectName(QStringLiteral("RenderCodec"));
    m_videoForm->addRow(tr("Codec"), m_codec);
    m_encoder = new QComboBox;
    m_encoder->setObjectName(QStringLiteral("RenderEncoder"));
    for (EncoderBackend b : {EncoderBackend::Auto, EncoderBackend::Software, EncoderBackend::Nvenc,
             EncoderBackend::Vaapi, EncoderBackend::Qsv}) {
        m_encoder->addItem(encoderBackendName(b), int(b));
    }
    m_encoder->setToolTip(
        tr("Hardware encoders are detected when the Deliver page opens. If the chosen one "
           "fails, the render falls back to the software encoder."));
    m_videoForm->addRow(tr("Encoder"), m_encoder);
    m_profile = new QComboBox;
    m_profile->setObjectName(QStringLiteral("RenderProfile"));
    m_videoForm->addRow(tr("Encoding profile"), m_profile);

    m_resolution = new QComboBox;
    m_resolution->setObjectName(QStringLiteral("RenderResolution"));
    for (const ResolutionChoice& r : kResolutions)
        m_resolution->addItem(tr(r.label));
    m_videoForm->addRow(tr("Resolution"), m_resolution);
    m_width = new QSpinBox;
    m_height = new QSpinBox;
    for (QSpinBox* spin : {m_width, m_height}) {
        spin->setRange(16, 16384);
        spin->setSingleStep(2);
    }
    m_sizeRow = row({m_width, new QLabel(QStringLiteral("x")), m_height});
    m_videoForm->addRow(QString(), m_sizeRow);
    m_frameRate = fixedCombo(QString(),
        tr("Renders use the timeline frame rate (no retiming); change it in Project Settings."));
    m_videoForm->addRow(tr("Frame rate"), m_frameRate);

    // Quality: Resolve's "Automatic [level]" / "Restrict to [n] Kb/s".
    m_qualityAuto = new QRadioButton(tr("Automatic"));
    m_qualityRestrict = new QRadioButton(tr("Restrict to"));
    m_qualityRestrict->setObjectName(QStringLiteral("RenderQualityRestrict"));
    auto* qualityGroup = new QButtonGroup(this);
    qualityGroup->addButton(m_qualityAuto);
    qualityGroup->addButton(m_qualityRestrict);
    m_qualityLevel = new QComboBox;
    m_qualityLevel->setObjectName(QStringLiteral("RenderQualityLevel"));
    m_qualityLevel->addItem(tr("Best"), int(QualityLevel::Best));
    m_qualityLevel->addItem(tr("High"), int(QualityLevel::High));
    m_qualityLevel->addItem(tr("Medium"), int(QualityLevel::Medium));
    m_qualityLevel->addItem(tr("Low"), int(QualityLevel::Low));
    m_qualityLevel->addItem(tr("Custom"), int(QualityLevel::Custom));
    m_quality = new QSpinBox;
    m_quality->setObjectName(QStringLiteral("RenderQualityValue"));
    m_quality->setToolTip(tr("CRF / CQ / QP: lower values give better quality and bigger files."));
    m_bitrate = new QSpinBox;
    m_bitrate->setObjectName(QStringLiteral("RenderBitrate"));
    m_bitrate->setRange(100, kMaxBitrateKbps);
    m_bitrate->setSingleStep(1000);
    m_bitrate->setSuffix(tr(" Kb/s"));
    m_qualityBox = new QWidget;
    auto* qualityLayout = new QVBoxLayout(m_qualityBox);
    qualityLayout->setContentsMargins(0, 0, 0, 0);
    qualityLayout->setSpacing(metrics::kSpaceS);
    qualityLayout->addWidget(row({m_qualityAuto, m_qualityLevel, m_quality}));
    qualityLayout->addWidget(row({m_qualityRestrict, m_bitrate}));
    m_videoForm->addRow(tr("Quality"), m_qualityBox);
    m_speed = new QComboBox;
    m_speed->addItem(tr("Faster"), QStringLiteral("fast"));
    m_speed->addItem(tr("Balanced"), QStringLiteral("medium"));
    m_speed->addItem(tr("Smaller file"), QStringLiteral("slow"));
    m_speed->setToolTip(tr("Encoder speed against compression efficiency"));
    m_videoForm->addRow(tr("Encoder speed"), m_speed);

    m_keyframesAuto = new QRadioButton(tr("Automatic"));
    m_keyframesEvery = new QRadioButton(tr("Every"));
    m_keyframesEvery->setObjectName(QStringLiteral("RenderKeyframesEvery"));
    auto* keyframeGroup = new QButtonGroup(this);
    keyframeGroup->addButton(m_keyframesAuto);
    keyframeGroup->addButton(m_keyframesEvery);
    m_keyframeInterval = new QSpinBox;
    m_keyframeInterval->setObjectName(QStringLiteral("RenderKeyframeInterval"));
    m_keyframeInterval->setRange(1, kMaxKeyframeInterval);
    m_keyframeInterval->setValue(kDefaultKeyframeInterval);
    m_keyframeInterval->setSuffix(tr(" frames"));
    m_keyframesAuto->setToolTip(tr("One key frame every two seconds"));
    m_keyframeBox = row({m_keyframesAuto, m_keyframesEvery, m_keyframeInterval});
    m_videoForm->addRow(tr("Key frames"), m_keyframeBox);
    m_frameReordering = new QCheckBox(tr("Frame reordering"));
    m_frameReordering->setObjectName(QStringLiteral("RenderFrameReordering"));
    m_frameReordering->setToolTip(
        tr("Allow B-frames. Turn off for files that some editing or streaming tools need to "
           "decode without reordering."));
    m_videoForm->addRow(QString(), m_frameReordering);
    // Resolve's Advanced Settings: render from proxy / optimized media instead of the camera
    // originals (off by default: export always uses the originals unless asked).
    m_useProxies = new QCheckBox(tr("Use proxy media"));
    m_useProxies->setObjectName(QStringLiteral("RenderUseProxies"));
    m_useProxies->setToolTip(tr("Render from proxies where they exist (fast drafts). Off: "
                                "always the camera originals."));
    m_videoForm->addRow(QString(), m_useProxies);
    m_useOptimized = new QCheckBox(tr("Use optimized media"));
    m_useOptimized->setObjectName(QStringLiteral("RenderUseOptimized"));
    m_videoForm->addRow(QString(), m_useOptimized);

    const QString levelsWhy
        = tr("YUV codecs are written with video levels (limited range), RGB and JPEG with full "
             "levels. Choosing the levels is coming in a later version.");
    m_dataLevels = fixedCombo(tr("Auto"), levelsWhy);
    m_videoForm->addRow(tr("Data levels"), m_dataLevels);
    const QString tagWhy = tr("Renders are tagged Rec.709 (sRGB for RGB image formats). Other "
                              "colour spaces are coming in a later version.");
    m_colorSpace = fixedCombo(tr("Rec.709"), tagWhy);
    m_videoForm->addRow(tr("Color space tag"), m_colorSpace);
    m_gamma = fixedCombo(tr("Rec.709"), tagWhy);
    m_videoForm->addRow(tr("Gamma tag"), m_gamma);
    m_alpha = new QCheckBox(tr("Export alpha"));
    m_alpha->setToolTip(tr("Keep transparency (ProRes 4444, PNG, TIFF, OpenEXR)"));
    m_videoForm->addRow(QString(), m_alpha);

    auto changed = [this] { refresh(); };
    for (QAbstractButton* button : std::initializer_list<QAbstractButton*> {
             m_videoEnabled, m_alpha, m_qualityAuto, m_keyframesAuto, m_frameReordering}) {
        connect(button, &QAbstractButton::toggled, this, changed);
    }
    for (QComboBox* combo : {m_format, m_codec, m_encoder, m_profile, m_resolution, m_speed})
        connect(combo, &QComboBox::currentIndexChanged, this, changed);
    connect(m_qualityLevel, &QComboBox::currentIndexChanged, this, [this] {
        // A named level writes its value into the spin box (the value is what is stored).
        const auto level = QualityLevel(m_qualityLevel->currentData().toInt());
        const int value = qualityValue(m_settings.video.codec, level);
        if (!m_updating && value >= 0)
            m_quality->setValue(value);
        refresh();
    });
    for (QSpinBox* spin : {m_width, m_height, m_quality, m_bitrate, m_keyframeInterval})
        connect(spin, &QSpinBox::valueChanged, this, changed);
    return page;
}

QWidget* RenderSettingsPanel::buildAudioTab()
{
    auto* page = new QWidget;
    m_audioForm = makeForm(page);
    m_audioEnabled = new QCheckBox(tr("Export Audio"));
    m_audioForm->addRow(m_audioEnabled);
    m_audioCodec = new QComboBox;
    m_audioCodec->setObjectName(QStringLiteral("RenderAudioCodec"));
    m_audioForm->addRow(tr("Codec"), m_audioCodec);
    m_sampleRate = new QComboBox;
    m_sampleRate->setObjectName(QStringLiteral("RenderSampleRate"));
    m_audioForm->addRow(tr("Sample rate"), m_sampleRate);
    m_bitDepth = new QComboBox;
    m_bitDepth->setObjectName(QStringLiteral("RenderBitDepth"));
    m_bitDepth->addItem(tr("16"), 16);
    m_bitDepth->addItem(tr("24"), 24);
    m_audioForm->addRow(tr("Bit depth"), m_bitDepth);
    m_channels = fixedCombo(tr("Stereo"),
        tr("The mixer renders stereo; mono and multichannel output are coming in a later "
           "version."));
    m_audioForm->addRow(tr("Output channels"), m_channels);
    m_audioBitrate = new QComboBox;
    m_audioBitrate->setObjectName(QStringLiteral("RenderAudioBitrate"));
    for (int kbps : kAudioBitrates)
        m_audioBitrate->addItem(tr("%1 Kb/s").arg(kbps), kbps);
    m_audioForm->addRow(tr("Bitrate"), m_audioBitrate);

    auto changed = [this] { refresh(); };
    connect(m_audioEnabled, &QCheckBox::toggled, this, changed);
    for (QComboBox* combo : {m_audioCodec, m_audioBitrate, m_sampleRate, m_bitDepth})
        connect(combo, &QComboBox::currentIndexChanged, this, changed);
    return page;
}

QWidget* RenderSettingsPanel::buildFileTab()
{
    auto* page = new QWidget;
    auto* form = makeForm(page);
    const QSettings settings;

    m_nameCustom = new QRadioButton(tr("Custom name"));
    m_nameTimeline = new QRadioButton(tr("Timeline name"));
    m_nameTimeline->setObjectName(QStringLiteral("RenderNameTimeline"));
    auto* nameGroup = new QButtonGroup(this);
    nameGroup->addButton(m_nameCustom);
    nameGroup->addButton(m_nameTimeline);
    (settings.value(QLatin1String(kNameUsesTimelineKey), false).toBool() ? m_nameTimeline
                                                                         : m_nameCustom)
        ->setChecked(true);
    auto* nameBox = new QWidget;
    auto* nameLayout = new QVBoxLayout(nameBox);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->setSpacing(metrics::kSpaceS);
    nameLayout->addWidget(m_nameCustom);
    nameLayout->addWidget(m_nameTimeline);
    form->addRow(tr("Filename uses"), nameBox);

    m_extension = new QLabel;
    m_extension->setObjectName(QStringLiteral("RenderExtension"));
    m_extension->setToolTip(tr("Set by the format on the Video tab"));
    form->addRow(tr("File extension"), m_extension);

    m_useSubfolder = new QCheckBox(tr("Use subfolder"));
    m_useSubfolder->setObjectName(QStringLiteral("RenderUseSubfolder"));
    m_useSubfolder->setChecked(settings.value(QLatin1String(kUseSubfolderKey), false).toBool());
    m_subfolder = new QLineEdit(settings.value(QLatin1String(kSubfolderKey)).toString());
    m_subfolder->setObjectName(QStringLiteral("RenderSubfolder"));
    m_subfolder->setPlaceholderText(tr("Subfolder name"));
    m_subfolder->setEnabled(m_useSubfolder->isChecked());
    auto* subfolderRow = row({m_useSubfolder, m_subfolder}, false);
    static_cast<QHBoxLayout*>(subfolderRow->layout())->setStretch(1, 1);
    form->addRow(tr("Render location"), subfolderRow);
    m_uniqueNames = new QCheckBox(tr("Use unique filenames"));
    m_uniqueNames->setObjectName(QStringLiteral("RenderUniqueNames"));
    m_uniqueNames->setToolTip(
        tr("Adds _1, _2, ... to the name instead of overwriting an existing file."));
    m_uniqueNames->setChecked(settings.value(QLatin1String(kUniqueNamesKey), false).toBool());
    form->addRow(QString(), m_uniqueNames);

    // Subtitle settings (Resolve's File tab): off by default; burn in, a separate file next
    // to the render, or a subtitle stream in the movie.
    m_fileForm = form;
    auto* subtitleHeading = new QLabel(tr("Subtitle Settings"));
    QFont headingFont = subtitleHeading->font();
    headingFont.setWeight(QFont::DemiBold);
    subtitleHeading->setFont(headingFont);
    form->addRow(subtitleHeading);
    m_subtitleExport = new QCheckBox(tr("Export subtitle"));
    m_subtitleExport->setObjectName(QStringLiteral("RenderSubtitleExport"));
    m_subtitleExport->setToolTip(tr("Include the enabled subtitle tracks in the render"));
    form->addRow(QString(), m_subtitleExport);
    m_subtitleMode = new QComboBox;
    m_subtitleMode->setObjectName(QStringLiteral("RenderSubtitleMode"));
    m_subtitleMode->addItem(tr("Burn into video"), int(SubtitleExportMode::BurnIn));
    m_subtitleMode->addItem(tr("As a separate file"), int(SubtitleExportMode::Sidecar));
    m_subtitleMode->addItem(tr("As embedded captions"), int(SubtitleExportMode::Embedded));
    form->addRow(tr("Format"), m_subtitleMode);
    m_subtitleFormat = new QComboBox;
    m_subtitleFormat->setObjectName(QStringLiteral("RenderSubtitleFormat"));
    m_subtitleFormat->addItem(tr("SRT"), int(SubtitleFormat::Srt));
    m_subtitleFormat->addItem(tr("WebVTT"), int(SubtitleFormat::WebVtt));
    form->addRow(tr("Export as"), m_subtitleFormat);
    m_subtitleCodec = new QComboBox;
    m_subtitleCodec->setObjectName(QStringLiteral("RenderSubtitleCodec"));
    form->addRow(tr("Codec"), m_subtitleCodec);
    connect(m_subtitleExport, &QCheckBox::toggled, this, [this] { refresh(); });
    for (QComboBox* box : {m_subtitleMode, m_subtitleFormat, m_subtitleCodec})
        connect(box, &QComboBox::currentIndexChanged, this, [this] { refresh(); });

    m_resultPath = new QLabel;
    m_resultPath->setObjectName(QStringLiteral("RenderResultPath"));
    m_resultPath->setWordWrap(true);
    m_resultPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_resultPath->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(tr("Output file"), m_resultPath);

    connect(m_nameTimeline, &QRadioButton::toggled, this, [this] {
        storeFileOptions();
        updateFileName();
    });
    connect(m_useSubfolder, &QCheckBox::toggled, this, [this](bool on) {
        m_subfolder->setEnabled(on);
        storeFileOptions();
        updateResultPath();
    });
    for (QLineEdit* edit : {m_subfolder}) {
        connect(edit, &QLineEdit::textChanged, this, [this] {
            storeFileOptions();
            updateResultPath();
        });
    }
    connect(m_uniqueNames, &QCheckBox::toggled, this, [this] {
        storeFileOptions();
        updateResultPath();
    });
    return page;
}

void RenderSettingsPanel::storeFileOptions() const
{
    QSettings settings;
    settings.setValue(QLatin1String(kNameUsesTimelineKey), m_nameTimeline->isChecked());
    settings.setValue(QLatin1String(kUseSubfolderKey), m_useSubfolder->isChecked());
    settings.setValue(QLatin1String(kSubfolderKey), m_subfolder->text().trimmed());
    settings.setValue(QLatin1String(kUniqueNamesKey), m_uniqueNames->isChecked());
}

QString RenderSettingsPanel::timelineName() const
{
    QString name = m_ctx.project ? m_ctx.project->timeline()->data().name : QString();
    if (name.isEmpty() && m_ctx.project)
        name = m_ctx.project->name();
    return name.isEmpty() ? tr("Untitled") : name;
}

void RenderSettingsPanel::updateFileName()
{
    const bool fromTimeline = m_nameTimeline->isChecked();
    m_fileName->setReadOnly(fromTimeline);
    if (fromTimeline)
        m_fileName->setText(timelineName());
    updateResultPath();
}

void RenderSettingsPanel::rebuildPresetRow()
{
    m_presetRow->clear();
    for (const ExportPreset& p : m_presets->all())
        m_presetRow->addPreset(p.id, p.name, icon(categoryIcon(p.category)), p.description);
    m_presetRow->setCurrent(m_presetId);
}

bool RenderSettingsPanel::useProxyMedia() const
{
    return m_useProxies->isChecked();
}

bool RenderSettingsPanel::useOptimizedMedia() const
{
    return m_useOptimized->isChecked();
}

void RenderSettingsPanel::setUseProxyMedia(bool use)
{
    m_useProxies->setChecked(use);
}

void RenderSettingsPanel::setUseOptimizedMedia(bool use)
{
    m_useOptimized->setChecked(use);
}

void RenderSettingsPanel::setEditingJob(bool editing)
{
    m_editingJob = editing;
    m_addButton->setText(editing ? tr("Update Job") : tr("Add to Render Queue"));
    m_addButton->setIcon(editing ? QIcon() : icon(QStringLiteral("add")));
    m_cancelEdit->setVisible(editing);
}

ExportSettings RenderSettingsPanel::settings() const
{
    ExportSettings s = m_settings;
    if (!m_format)
        return s;
    s.container = m_format->currentData().toString();
    s.videoEnabled = m_videoEnabled->isChecked();
    s.video.codec = m_codec->currentData().toString();
    s.video.encoder = EncoderBackend(m_encoder->currentData().toInt());
    s.video.profile = m_profile->currentData().toString();
    const int res = m_resolution->currentIndex();
    if (res == kCustomResolution) {
        s.video.width = m_width->value();
        s.video.height = m_height->value();
    } else if (res >= 0) {
        s.video.width = kResolutions[res].width;
        s.video.height = kResolutions[res].height;
    }
    s.video.rateControl
        = m_qualityRestrict->isChecked() ? RateControl::Bitrate : RateControl::Quality;
    s.video.quality = m_quality->value();
    s.video.bitrateKbps = m_bitrate->value();
    s.video.speed = m_speed->currentData().toString();
    s.video.keyframeInterval = m_keyframesEvery->isChecked() ? m_keyframeInterval->value() : 0;
    s.video.frameReordering = m_frameReordering->isChecked();
    s.video.alpha = m_alpha->isChecked();
    s.audio.enabled = m_audioEnabled->isChecked();
    s.audio.codec = m_audioCodec->currentData().toString();
    s.audio.bitrateKbps = m_audioBitrate->currentData().toInt();
    s.audio.sampleRate = m_sampleRate->currentData().toInt();
    s.audio.bitDepth = m_bitDepth->currentData().toInt();
    s.range = RangeMode(m_range->currentData().toInt());
    s.subtitles.enabled = m_subtitleExport->isChecked() && s.kind() != OutputKind::Audio
        && s.kind() != OutputKind::Still;
    s.subtitles.mode = SubtitleExportMode(m_subtitleMode->currentData().toInt());
    s.subtitles.sidecarFormat = SubtitleFormat(m_subtitleFormat->currentData().toInt());
    if (m_subtitleCodec->currentIndex() >= 0)
        s.subtitles.embeddedCodec = m_subtitleCodec->currentData().toString();

    QString name = m_fileName->text().trimmed();
    if (name.isEmpty())
        name = tr("Untitled");
    QDir dir(m_location->text().trimmed());
    const QString subfolder = m_subfolder->text().trimmed();
    if (m_useSubfolder->isChecked() && !subfolder.isEmpty())
        dir.setPath(dir.filePath(subfolder));
    s.outputPath = withContainerExtension(dir.filePath(name), s.container);
    if (m_uniqueNames->isChecked() && s.kind() != OutputKind::ImageSequence) {
        // Resolve's "unique filenames": name_1, name_2, ... instead of overwriting.
        const QFileInfo base(s.outputPath);
        for (int n = 1; QFileInfo::exists(s.outputPath); ++n) {
            s.outputPath = base.dir().filePath(
                QStringLiteral("%1_%2.%3").arg(base.completeBaseName()).arg(n).arg(base.suffix()));
        }
    }
    return s;
}

void RenderSettingsPanel::setSettings(const ExportSettings& input)
{
    ExportSettings s = input;
    s.normalize();
    m_settings = s;
    m_updating = true;
    const ContainerInfo* container = s.containerInfo();
    selectData(m_format, s.container);

    m_codec->clear();
    if (container) {
        for (const QString& id : container->videoCodecs) {
            if (const VideoCodecInfo* c = findVideoCodec(id))
                m_codec->addItem(c->name, c->id);
        }
    }
    selectData(m_codec, s.video.codec);
    m_videoEnabled->setChecked(s.videoEnabled);
    selectData(m_encoder, int(s.video.encoder));

    const VideoCodecInfo* codec = findVideoCodec(s.video.codec);
    m_profile->clear();
    if (codec) {
        for (int i = 0; i < codec->profiles.size(); ++i) {
            m_profile->addItem(
                codec->profileNames.value(i, codec->profiles[i]), codec->profiles[i]);
        }
        m_quality->setRange(codec->qualityMin, std::max(codec->qualityMin, codec->qualityMax));
    }
    if (m_profile->count() == 0) {
        // H.264 / H.265 / AV1: the encoder picks the profile from the pixel format.
        m_profile->addItem(tr("Auto"), QString());
    }
    selectData(m_profile, s.video.profile);

    int res = kCustomResolution;
    for (int i = 0; i < kCustomResolution; ++i) {
        if (kResolutions[i].width == s.video.width && kResolutions[i].height == s.video.height)
            res = i;
    }
    m_resolution->setCurrentIndex(res);
    const TimelineData& tl = m_ctx.project->timeline()->data();
    const QSize size = s.outputSize(tl);
    m_width->setValue(size.width());
    m_height->setValue(size.height());
    m_resolution->setItemText(0, tr("Match timeline (%1 x %2)").arg(tl.width).arg(tl.height));
    m_frameRate->setItemText(0, tr("%1 fps").arg(tl.fps.toDouble(), 0, 'g', 5));

    (s.video.rateControl == RateControl::Bitrate ? m_qualityRestrict : m_qualityAuto)
        ->setChecked(true);
    m_quality->setValue(s.video.quality);
    QualityLevel level = QualityLevel::Custom;
    for (QualityLevel l :
        {QualityLevel::Best, QualityLevel::High, QualityLevel::Medium, QualityLevel::Low}) {
        if (qualityValue(s.video.codec, l) == s.video.quality) {
            level = l;
            break;
        }
    }
    selectData(m_qualityLevel, int(level));
    m_bitrate->setValue(s.video.bitrateKbps);
    selectData(m_speed, s.video.speed);
    (s.video.keyframeInterval > 0 ? m_keyframesEvery : m_keyframesAuto)->setChecked(true);
    if (s.video.keyframeInterval > 0)
        m_keyframeInterval->setValue(s.video.keyframeInterval);
    m_frameReordering->setChecked(s.video.frameReordering);
    m_alpha->setChecked(s.video.alpha);

    m_audioCodec->clear();
    if (container) {
        for (const QString& id : container->audioCodecs) {
            if (const AudioCodecInfo* c = findAudioCodec(id))
                m_audioCodec->addItem(c->name, c->id);
        }
    }
    selectData(m_audioCodec, s.audio.codec);
    m_audioEnabled->setChecked(s.audio.enabled);
    selectData(m_audioBitrate, s.audio.bitrateKbps);
    if (m_audioBitrate->currentData().toInt() != s.audio.bitrateKbps) {
        m_audioBitrate->addItem(tr("%1 Kb/s").arg(s.audio.bitrateKbps), s.audio.bitrateKbps);
        selectData(m_audioBitrate, s.audio.bitrateKbps);
    }
    m_sampleRate->clear();
    if (const AudioCodecInfo* a = findAudioCodec(s.audio.codec)) {
        for (int rate : a->sampleRates)
            m_sampleRate->addItem(tr("%1 Hz").arg(rate), rate);
    }
    selectData(m_sampleRate, s.audio.sampleRate);
    selectData(m_bitDepth, s.audio.bitDepth);
    selectData(m_range, int(s.range));
    m_subtitleExport->setChecked(s.subtitles.enabled);
    selectData(m_subtitleMode, int(s.subtitles.mode));
    selectData(m_subtitleFormat, int(s.subtitles.sidecarFormat));
    m_subtitleCodec->clear();
    for (const QString& codec : subtitleCodecs(s.container))
        m_subtitleCodec->addItem(subtitleCodecName(codec), codec);
    const QString subtitleCodec = s.embeddedSubtitleCodec();
    if (const int i = m_subtitleCodec->findData(
            subtitleCodec.isEmpty() ? s.subtitles.embeddedCodec : subtitleCodec);
        i >= 0)
        m_subtitleCodec->setCurrentIndex(i);
    m_extension->setText(container ? QStringLiteral(".") + container->extension : QString());

    if (!s.outputPath.isEmpty()) {
        const QFileInfo fi(s.outputPath);
        QString folder = fi.absolutePath();
        // A path built with the subfolder option keeps the parent as the location.
        const QString subfolder = m_subfolder->text().trimmed();
        if (m_useSubfolder->isChecked() && !subfolder.isEmpty()
            && QFileInfo(folder).fileName() == subfolder) {
            folder = QFileInfo(folder).absolutePath();
        }
        if (!m_nameTimeline->isChecked())
            m_fileName->setText(fi.completeBaseName());
        m_location->setText(QDir::toNativeSeparators(folder));
    }
    m_updating = false;
    updateVisibility();
    updateFileName();
}

void RenderSettingsPanel::refresh()
{
    if (m_updating)
        return;
    m_presetId.clear(); // a manual change: no preset matches any more
    m_presetRow->setCurrent(QString());
    m_presetName->setText(tr("Custom Export"));
    ExportSettings next = settings();
    // A named quality level ("High") follows a codec change with that codec's value.
    const auto level = QualityLevel(m_qualityLevel->currentData().toInt());
    if (level != QualityLevel::Custom && next.video.codec != m_settings.video.codec) {
        if (const int value = qualityValue(next.video.codec, level); value >= 0)
            next.video.quality = value;
    }
    setSettings(next);
    Q_EMIT settingsChanged();
}

void RenderSettingsPanel::updateVisibility()
{
    const ExportSettings& s = m_settings;
    const OutputKind kind = s.kind();
    const VideoCodecInfo* codec = findVideoCodec(s.video.codec);
    const bool video = kind != OutputKind::Audio;
    const bool movie = kind == OutputKind::Movie;
    const bool videoOn = video && (!movie || s.videoEnabled);
    const bool interFrame = videoOn && codec && codec->hasBitrate; // h264/hevc/av1/vp9
    m_videoForm->setRowVisible(m_videoEnabled, movie);
    m_videoForm->setRowVisible(m_codec, video);
    m_videoForm->setRowVisible(m_encoder, videoOn && hasHardwareEncoders(s.video.codec));
    m_videoForm->setRowVisible(m_profile, videoOn && codec && codec->id != QLatin1String("gif"));
    m_profile->setEnabled(codec && !codec->profiles.isEmpty());
    m_profile->setToolTip(m_profile->isEnabled()
            ? QString()
            : tr("Chosen automatically by the encoder for this codec"));
    m_videoForm->setRowVisible(m_resolution, videoOn);
    m_videoForm->setRowVisible(m_sizeRow, videoOn);
    m_sizeRow->setEnabled(m_resolution->currentIndex() == kCustomResolution);
    m_videoForm->setRowVisible(m_frameRate, videoOn && kind != OutputKind::Still);

    const bool hasQuality = videoOn && codec && codec->hasQuality;
    m_videoForm->setRowVisible(m_qualityBox, hasQuality || (videoOn && codec && codec->hasBitrate));
    const bool restrict = codec && codec->hasBitrate && s.video.rateControl == RateControl::Bitrate;
    m_qualityAuto->setEnabled(codec && codec->hasQuality);
    m_qualityRestrict->setEnabled(codec && codec->hasBitrate);
    m_qualityRestrict->setToolTip(m_qualityRestrict->isEnabled()
            ? tr("Target average bitrate")
            : tr("This codec has no bitrate control"));
    m_qualityLevel->setEnabled(!restrict && hasQuality);
    m_quality->setVisible(
        QualityLevel(m_qualityLevel->currentData().toInt()) == QualityLevel::Custom);
    m_quality->setEnabled(!restrict && hasQuality);
    m_bitrate->setEnabled(restrict);
    m_videoForm->setRowVisible(m_speed, interFrame);
    m_videoForm->setRowVisible(m_keyframeBox, interFrame);
    m_keyframeInterval->setEnabled(m_keyframesEvery->isChecked());
    m_videoForm->setRowVisible(m_frameReordering, interFrame);
    m_videoForm->setRowVisible(m_dataLevels, videoOn);
    m_videoForm->setRowVisible(m_colorSpace, videoOn);
    m_videoForm->setRowVisible(m_gamma, videoOn);
    bool alphaPossible = codec && codec->alphaCapable;
    if (codec && codec->id == QLatin1String("prores"))
        alphaPossible = s.video.profile.startsWith(QLatin1String("4444"));
    m_videoForm->setRowVisible(m_alpha, videoOn && alphaPossible);

    const bool audioPossible = kind == OutputKind::Movie || kind == OutputKind::Audio;
    const bool audioOn = s.hasAudio();
    const AudioCodecInfo* a = findAudioCodec(s.audio.codec);
    m_audioForm->setRowVisible(m_audioEnabled, kind == OutputKind::Movie);
    m_audioForm->setRowVisible(m_audioCodec, audioPossible);
    m_audioForm->setRowVisible(m_sampleRate, audioOn);
    m_audioForm->setRowVisible(m_bitDepth, audioOn && s.audio.codec == QLatin1String("flac"));
    m_audioForm->setRowVisible(m_channels, audioOn);
    m_audioForm->setRowVisible(m_audioBitrate, audioOn && a && !a->lossless);
    m_tabs->setTabEnabled(1, audioPossible);
    m_tabs->setTabToolTip(1, audioPossible ? QString() : tr("This format has no audio"));
    m_range->setEnabled(kind != OutputKind::Still);
    m_addButton->setEnabled(s.hasVideo() || s.hasAudio());

    // Subtitles: embedding needs a container with subtitle streams, burning in a picture.
    // An audio-only file or a still has no picture to carry them: the box is greyed out.
    const bool subtitlesPossible = kind != OutputKind::Still && kind != OutputKind::Audio;
    const bool subtitlesOn = s.subtitles.enabled && subtitlesPossible;
    m_subtitleExport->setEnabled(subtitlesPossible);
    m_subtitleExport->setToolTip(subtitlesPossible
            ? tr("Include the enabled subtitle tracks in the render")
            : tr("This format has no picture to carry subtitles"));
    m_fileForm->setRowVisible(m_subtitleMode, subtitlesOn);
    m_fileForm->setRowVisible(
        m_subtitleFormat, subtitlesOn && s.subtitles.mode == SubtitleExportMode::Sidecar);
    m_fileForm->setRowVisible(
        m_subtitleCodec, subtitlesOn && s.subtitles.mode == SubtitleExportMode::Embedded);
    const bool canEmbed = !subtitleCodecs(s.container).isEmpty();
    m_subtitleCodec->setEnabled(canEmbed);
    m_subtitleCodec->setToolTip(canEmbed
            ? QString()
            : tr("This format cannot carry subtitles; choose MP4, QuickTime, "
                 "MKV or WebM, or write a separate file"));

    // Encoder availability annotations.
    for (int i = 0; i < m_encoder->count(); ++i) {
        const auto backend = EncoderBackend(m_encoder->itemData(i).toInt());
        QString text = encoderBackendName(backend);
        if (backend != EncoderBackend::Auto && backend != EncoderBackend::Software
            && !m_hwStatus.isEmpty()) {
            bool available = false;
            for (const HwEncoderStatus& hw : m_hwStatus)
                available |= hw.backend == backend && hw.codecId == s.video.codec && hw.available;
            if (!available)
                text += tr(" (not available)");
        }
        m_encoder->setItemText(i, text);
    }
}

void RenderSettingsPanel::updateResultPath()
{
    const ExportSettings s = settings();
    const QString path
        = s.kind() == OutputKind::ImageSequence ? sequencePattern(s.outputPath) : s.outputPath;
    m_resultPath->setText(QDir::toNativeSeparators(path));
}

void RenderSettingsPanel::applyPreset(const QString& id)
{
    ExportSettings s = settings();
    if (!m_presets->apply(id, s))
        return;
    setSettings(s);
    m_presetId = id;
    m_presetRow->setCurrent(id);
    const ExportPreset* preset = m_presets->find(id);
    m_presetName->setText(preset ? preset->name : QString());
    Q_EMIT settingsChanged();
}

void RenderSettingsPanel::setHardwareStatus(const QList<HwEncoderStatus>& status)
{
    m_hwStatus = status;
    QStringList available;
    for (const HwEncoderStatus& hw : status) {
        if (hw.available)
            available << hw.encoderName;
    }
    m_encoder->setToolTip(available.isEmpty()
            ? tr("No hardware encoder was found; software encoders are used.")
            : tr("Hardware encoders found: %1").arg(available.join(QStringLiteral(", "))));
    updateVisibility();
}

void RenderSettingsPanel::setDefaultOutput(const QString& folder, const QString& baseName)
{
    QString dir = folder;
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        if (dir.isEmpty())
            dir = QDir::homePath();
    }
    QString name = baseName;
    if (name.isEmpty())
        name = m_ctx.project ? m_ctx.project->name() : QString();
    m_location->setText(QDir::toNativeSeparators(dir));
    if (!m_nameTimeline->isChecked())
        m_fileName->setText(name.isEmpty() ? tr("Untitled") : name);
    updateFileName();
}

void RenderSettingsPanel::browseLocation()
{
    const QString dir = platform::getExistingDirectory(
        this, tr("Render Location"), QString(), m_location->text());
    if (!dir.isEmpty())
        m_location->setText(QDir::toNativeSeparators(dir));
}

void RenderSettingsPanel::savePreset()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Save Preset"), tr("Preset name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    QString error;
    const QString id = m_presets->saveUserPreset(name, settings(), &error);
    if (id.isEmpty()) {
        QMessageBox::warning(this, tr("Save Preset"), error);
        return;
    }
    m_presetId = id;
    m_presetName->setText(name.trimmed());
    rebuildPresetRow();
}

void RenderSettingsPanel::deletePreset()
{
    const QString selected = m_presetRow->current();
    const QString id = selected.isEmpty() ? m_presetId : selected;
    const ExportPreset* preset = m_presets->find(id);
    if (!preset || preset->builtin) {
        QMessageBox::information(this, tr("Delete Preset"),
            tr("Select a user preset first. Built-in presets cannot be deleted."));
        return;
    }
    if (QMessageBox::question(
            this, tr("Delete Preset"), tr("Delete the preset \"%1\"?").arg(preset->name))
        != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!m_presets->removeUserPreset(id, &error)) {
        QMessageBox::warning(this, tr("Delete Preset"), error);
        return;
    }
    m_presetId.clear();
    rebuildPresetRow();
}

} // namespace kk
