// SPDX-License-Identifier: GPL-3.0-or-later
// RenderSettingsPanel — widgets ↔ ExportSettings.
#include "ui/deliver/RenderSettingsPanel.h"

#include "core/Project.h"
#include "export/ExportPresets.h"
#include "ui/common/AppContext.h"
#include "ui/common/Icons.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

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
    {0, 0, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "Timeline")},
    {3840, 2160, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "3840 x 2160 Ultra HD")},
    {2560, 1440, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "2560 x 1440 QHD")},
    {1920, 1080, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "1920 x 1080 HD")},
    {1280, 720, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "1280 x 720 HD")},
    {960, 540, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "960 x 540")},
    {640, 360, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "640 x 360")},
    {-1, -1, QT_TRANSLATE_NOOP("kk::RenderSettingsPanel", "Custom")},
};
constexpr int kCustomResolution = int(std::size(kResolutions)) - 1;

constexpr int kAudioBitrates[] = {96, 128, 160, 192, 256, 320};
constexpr int kMaxBitrateKbps = 800000; ///< spin box limit (800 Mb/s)
constexpr int kPresetStripHeight = 86;

bool hasHardwareEncoders(const QString& codec)
{
    return codec == QLatin1String("h264") || codec == QLatin1String("hevc")
        || codec == QLatin1String("av1") || codec == QLatin1String("vp9");
}

QString categoryIcon(const QString& category)
{
    if (category == QLatin1String("Audio")) {
        return QStringLiteral("audio");
    }
    if (category == QLatin1String("Images")) {
        return QStringLiteral("image");
    }
    if (category == QLatin1String("User")) {
        return QStringLiteral("marker");
    }
    return QStringLiteral("film");
}

void selectData(QComboBox* combo, const QVariant& data)
{
    const int index = combo->findData(data);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    }
}

} // namespace

RenderSettingsPanel::RenderSettingsPanel(AppContext& ctx, ExportPresets* presets, QWidget* parent)
    : PanelFrame(tr("Render Settings"), parent)
    , m_ctx(ctx)
    , m_presets(presets)
{
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    // ---- Preset strip ----
    m_presetList = new QListWidget;
    m_presetList->setObjectName(QStringLiteral("RenderPresetStrip"));
    m_presetList->setViewMode(QListView::IconMode);
    m_presetList->setFlow(QListView::LeftToRight);
    m_presetList->setWrapping(false);
    m_presetList->setMovement(QListView::Static);
    m_presetList->setIconSize(QSize(28, 28));
    m_presetList->setGridSize(QSize(96, 64));
    m_presetList->setWordWrap(true);
    m_presetList->setFixedHeight(kPresetStripHeight);
    m_presetList->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_presetList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_presetList);
    connect(m_presetList, &QListWidget::itemClicked, this,
        [this](QListWidgetItem* item) { applyPreset(item->data(Qt::UserRole).toString()); });

    auto* presetMenuButton = new QToolButton;
    presetMenuButton->setText(tr("Presets"));
    presetMenuButton->setToolTip(tr("Save or delete user presets"));
    presetMenuButton->setPopupMode(QToolButton::InstantPopup);
    presetMenuButton->setAutoRaise(true);
    auto* menu = new QMenu(presetMenuButton);
    menu->addAction(tr("Save as New Preset..."), this, &RenderSettingsPanel::savePreset);
    menu->addAction(tr("Delete Selected User Preset"), this, &RenderSettingsPanel::deletePreset);
    presetMenuButton->setMenu(menu);
    addTitleBarWidget(presetMenuButton);

    // ---- Tabs ----
    m_tabs = new QTabWidget;
    m_tabs->addTab(buildVideoTab(), tr("Video"));
    m_tabs->addTab(buildAudioTab(), tr("Audio"));
    m_tabs->addTab(buildFileTab(), tr("File"));
    layout->addWidget(m_tabs, 1);

    // ---- Range + queue ----
    auto* rangeRow = new QHBoxLayout;
    rangeRow->addWidget(new QLabel(tr("Render")));
    m_rangeEntire = new QRadioButton(tr("Entire Timeline"));
    m_rangeInOut = new QRadioButton(tr("In/Out Range"));
    auto* rangeGroup = new QButtonGroup(this);
    rangeGroup->addButton(m_rangeEntire);
    rangeGroup->addButton(m_rangeInOut);
    m_rangeEntire->setChecked(true);
    rangeRow->addWidget(m_rangeEntire);
    rangeRow->addWidget(m_rangeInOut);
    rangeRow->addStretch(1);
    layout->addLayout(rangeRow);
    connect(rangeGroup, &QButtonGroup::buttonToggled, this, [this] { refresh(); });

    m_addButton = new QPushButton(icon(QStringLiteral("add")), tr("Add to Render Queue"));
    m_addButton->setDefault(true);
    layout->addWidget(m_addButton);
    connect(m_addButton, &QPushButton::clicked, this, &RenderSettingsPanel::addToQueueRequested);

    setContentWidget(body);
    rebuildPresetStrip();

    setDefaultOutput(QString(), QString());
    applyPreset(QStringLiteral("youtube_1080p"));
    if (m_presetId.isEmpty()) {
        setSettings(settings());
    }
}

QWidget* RenderSettingsPanel::buildVideoTab()
{
    auto* page = new QWidget;
    m_videoForm = new QFormLayout(page);
    m_videoForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_videoEnabled = new QCheckBox(tr("Export Video"));
    m_videoForm->addRow(m_videoEnabled);
    m_format = new QComboBox;
    for (const ContainerInfo& c : exportContainers()) {
        m_format->addItem(c.name, c.id);
    }
    m_videoForm->addRow(tr("Format"), m_format);
    m_codec = new QComboBox;
    m_videoForm->addRow(tr("Codec"), m_codec);
    m_encoder = new QComboBox;
    for (EncoderBackend b : {EncoderBackend::Auto, EncoderBackend::Software, EncoderBackend::Nvenc,
             EncoderBackend::Vaapi, EncoderBackend::Qsv}) {
        m_encoder->addItem(encoderBackendName(b), int(b));
    }
    m_encoder->setToolTip(
        tr("Hardware encoders are detected when the Deliver page opens. If the chosen one "
           "fails, the render falls back to the software encoder."));
    m_videoForm->addRow(tr("Encoder"), m_encoder);
    m_profile = new QComboBox;
    m_videoForm->addRow(tr("Profile"), m_profile);

    m_resolution = new QComboBox;
    for (const ResolutionChoice& r : kResolutions) {
        m_resolution->addItem(tr(r.label));
    }
    m_videoForm->addRow(tr("Resolution"), m_resolution);
    m_sizeRow = new QWidget;
    auto* sizeLayout = new QHBoxLayout(m_sizeRow);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    m_width = new QSpinBox;
    m_height = new QSpinBox;
    for (QSpinBox* spin : {m_width, m_height}) {
        spin->setRange(16, 16384);
        spin->setSingleStep(2);
    }
    sizeLayout->addWidget(m_width);
    sizeLayout->addWidget(new QLabel(QStringLiteral("x")));
    sizeLayout->addWidget(m_height);
    sizeLayout->addStretch(1);
    m_videoForm->addRow(QString(), m_sizeRow);
    m_frameRate = new QLabel;
    m_videoForm->addRow(tr("Frame rate"), m_frameRate);

    m_rateControl = new QComboBox;
    m_rateControl->addItem(tr("Constant quality"), int(RateControl::Quality));
    m_rateControl->addItem(tr("Target bitrate"), int(RateControl::Bitrate));
    m_videoForm->addRow(tr("Quality"), m_rateControl);
    m_quality = new QSpinBox;
    m_quality->setToolTip(tr("CRF / CQ / QP: lower values give better quality and bigger files."));
    m_videoForm->addRow(tr("Quality value"), m_quality);
    m_bitrate = new QSpinBox;
    m_bitrate->setRange(100, kMaxBitrateKbps);
    m_bitrate->setSingleStep(1000);
    m_bitrate->setSuffix(tr(" kb/s"));
    m_videoForm->addRow(tr("Bitrate"), m_bitrate);
    m_speed = new QComboBox;
    m_speed->addItem(tr("Faster"), QStringLiteral("fast"));
    m_speed->addItem(tr("Balanced"), QStringLiteral("medium"));
    m_speed->addItem(tr("Smaller file"), QStringLiteral("slow"));
    m_videoForm->addRow(tr("Encoding speed"), m_speed);
    m_alpha = new QCheckBox(tr("Export alpha (transparency)"));
    m_videoForm->addRow(m_alpha);

    auto changed = [this] { refresh(); };
    connect(m_videoEnabled, &QCheckBox::toggled, this, changed);
    connect(m_alpha, &QCheckBox::toggled, this, changed);
    for (QComboBox* combo :
        {m_format, m_codec, m_encoder, m_profile, m_resolution, m_rateControl, m_speed}) {
        connect(combo, &QComboBox::currentIndexChanged, this, changed);
    }
    for (QSpinBox* spin : {m_width, m_height, m_quality, m_bitrate}) {
        connect(spin, &QSpinBox::valueChanged, this, changed);
    }
    return page;
}

QWidget* RenderSettingsPanel::buildAudioTab()
{
    auto* page = new QWidget;
    m_audioForm = new QFormLayout(page);
    m_audioForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_audioEnabled = new QCheckBox(tr("Export Audio"));
    m_audioForm->addRow(m_audioEnabled);
    m_audioCodec = new QComboBox;
    m_audioForm->addRow(tr("Codec"), m_audioCodec);
    m_audioBitrate = new QComboBox;
    for (int kbps : kAudioBitrates) {
        m_audioBitrate->addItem(tr("%1 kb/s").arg(kbps), kbps);
    }
    m_audioForm->addRow(tr("Bitrate"), m_audioBitrate);
    m_sampleRate = new QComboBox;
    m_audioForm->addRow(tr("Sample rate"), m_sampleRate);
    m_bitDepth = new QComboBox;
    m_bitDepth->addItem(tr("16-bit"), 16);
    m_bitDepth->addItem(tr("24-bit"), 24);
    m_audioForm->addRow(tr("Bit depth"), m_bitDepth);
    auto* note = new QLabel(tr("Audio is mixed at 48 kHz stereo and resampled for other rates."));
    note->setWordWrap(true);
    note->setEnabled(false);
    m_audioForm->addRow(note);

    auto changed = [this] { refresh(); };
    connect(m_audioEnabled, &QCheckBox::toggled, this, changed);
    for (QComboBox* combo : {m_audioCodec, m_audioBitrate, m_sampleRate, m_bitDepth}) {
        connect(combo, &QComboBox::currentIndexChanged, this, changed);
    }
    return page;
}

QWidget* RenderSettingsPanel::buildFileTab()
{
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_fileName = new QLineEdit;
    form->addRow(tr("File name"), m_fileName);
    auto* locationRow = new QWidget;
    auto* locationLayout = new QHBoxLayout(locationRow);
    locationLayout->setContentsMargins(0, 0, 0, 0);
    m_location = new QLineEdit;
    auto* browse = new QPushButton(tr("Browse..."));
    locationLayout->addWidget(m_location, 1);
    locationLayout->addWidget(browse);
    form->addRow(tr("Location"), locationRow);
    m_resultPath = new QLabel;
    m_resultPath->setWordWrap(true);
    m_resultPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Output"), m_resultPath);
    connect(browse, &QPushButton::clicked, this, &RenderSettingsPanel::browseLocation);
    connect(m_fileName, &QLineEdit::textChanged, this, &RenderSettingsPanel::updateResultPath);
    connect(m_location, &QLineEdit::textChanged, this, &RenderSettingsPanel::updateResultPath);
    return page;
}

void RenderSettingsPanel::rebuildPresetStrip()
{
    m_presetList->clear();
    for (const ExportPreset& p : m_presets->all()) {
        auto* item = new QListWidgetItem(icon(categoryIcon(p.category)), p.name);
        item->setData(Qt::UserRole, p.id);
        item->setToolTip(
            p.description.isEmpty() ? p.name : p.name + QLatin1Char('\n') + p.description);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        m_presetList->addItem(item);
        if (p.id == m_presetId) {
            item->setSelected(true);
        }
    }
}

ExportSettings RenderSettingsPanel::settings() const
{
    ExportSettings s = m_settings;
    if (!m_format) {
        return s;
    }
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
    s.video.rateControl = RateControl(m_rateControl->currentData().toInt());
    s.video.quality = m_quality->value();
    s.video.bitrateKbps = m_bitrate->value();
    s.video.speed = m_speed->currentData().toString();
    s.video.alpha = m_alpha->isChecked();
    s.audio.enabled = m_audioEnabled->isChecked();
    s.audio.codec = m_audioCodec->currentData().toString();
    s.audio.bitrateKbps = m_audioBitrate->currentData().toInt();
    s.audio.sampleRate = m_sampleRate->currentData().toInt();
    s.audio.bitDepth = m_bitDepth->currentData().toInt();
    s.range = m_rangeInOut->isChecked() ? RangeMode::InOut : RangeMode::Entire;
    QString name = m_fileName->text().trimmed();
    if (name.isEmpty()) {
        name = tr("Untitled");
    }
    s.outputPath
        = withContainerExtension(QDir(m_location->text().trimmed()).filePath(name), s.container);
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
            if (const VideoCodecInfo* c = findVideoCodec(id)) {
                m_codec->addItem(c->name, c->id);
            }
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
    selectData(m_profile, s.video.profile);

    int res = kCustomResolution;
    for (int i = 0; i < kCustomResolution; ++i) {
        if (kResolutions[i].width == s.video.width && kResolutions[i].height == s.video.height) {
            res = i;
        }
    }
    m_resolution->setCurrentIndex(res);
    const TimelineData& tl = m_ctx.project->timeline()->data();
    const QSize size = s.outputSize(tl);
    m_width->setValue(size.width());
    m_height->setValue(size.height());
    m_resolution->setItemText(0, tr("Timeline (%1 x %2)").arg(tl.width).arg(tl.height));
    m_frameRate->setText(tr("%1 fps (timeline)").arg(tl.fps.toDouble(), 0, 'g', 5));
    selectData(m_rateControl, int(s.video.rateControl));
    m_quality->setValue(s.video.quality);
    m_bitrate->setValue(s.video.bitrateKbps);
    selectData(m_speed, s.video.speed);
    m_alpha->setChecked(s.video.alpha);

    m_audioCodec->clear();
    if (container) {
        for (const QString& id : container->audioCodecs) {
            if (const AudioCodecInfo* c = findAudioCodec(id)) {
                m_audioCodec->addItem(c->name, c->id);
            }
        }
    }
    selectData(m_audioCodec, s.audio.codec);
    m_audioEnabled->setChecked(s.audio.enabled);
    selectData(m_audioBitrate, s.audio.bitrateKbps);
    if (m_audioBitrate->currentData().toInt() != s.audio.bitrateKbps) {
        m_audioBitrate->addItem(tr("%1 kb/s").arg(s.audio.bitrateKbps), s.audio.bitrateKbps);
        selectData(m_audioBitrate, s.audio.bitrateKbps);
    }
    m_sampleRate->clear();
    if (const AudioCodecInfo* a = findAudioCodec(s.audio.codec)) {
        for (int rate : a->sampleRates) {
            m_sampleRate->addItem(tr("%1 Hz").arg(rate), rate);
        }
    }
    selectData(m_sampleRate, s.audio.sampleRate);
    selectData(m_bitDepth, s.audio.bitDepth);
    (s.range == RangeMode::InOut ? m_rangeInOut : m_rangeEntire)->setChecked(true);

    if (!s.outputPath.isEmpty()) {
        const QFileInfo fi(s.outputPath);
        m_fileName->setText(fi.completeBaseName());
        m_location->setText(QDir::toNativeSeparators(fi.absolutePath()));
    }
    m_updating = false;
    updateVisibility();
    updateResultPath();
}

void RenderSettingsPanel::refresh()
{
    if (m_updating) {
        return;
    }
    m_presetId.clear(); // a manual change: no preset matches any more
    m_presetList->clearSelection();
    setSettings(settings());
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
    m_videoForm->setRowVisible(m_videoEnabled, movie);
    m_videoForm->setRowVisible(m_codec, video);
    m_videoForm->setRowVisible(m_encoder, videoOn && hasHardwareEncoders(s.video.codec));
    m_videoForm->setRowVisible(m_profile, videoOn && codec && !codec->profiles.isEmpty());
    m_videoForm->setRowVisible(m_resolution, videoOn);
    m_videoForm->setRowVisible(m_sizeRow, videoOn);
    m_sizeRow->setEnabled(m_resolution->currentIndex() == kCustomResolution);
    m_videoForm->setRowVisible(m_frameRate, videoOn && kind != OutputKind::Still);
    const bool rc = videoOn && codec && codec->hasQuality && codec->hasBitrate;
    m_videoForm->setRowVisible(m_rateControl, rc);
    const bool quality = videoOn && codec && codec->hasQuality
        && (!codec->hasBitrate || s.video.rateControl == RateControl::Quality);
    m_videoForm->setRowVisible(m_quality, quality);
    m_videoForm->setRowVisible(m_bitrate, rc && s.video.rateControl == RateControl::Bitrate);
    m_videoForm->setRowVisible(m_speed, videoOn && codec && codec->hasBitrate);
    bool alphaPossible = codec && codec->alphaCapable;
    if (codec && codec->id == QLatin1String("prores")) {
        alphaPossible = s.video.profile.startsWith(QLatin1String("4444"));
    }
    m_videoForm->setRowVisible(m_alpha, videoOn && alphaPossible);

    const bool audioPossible = kind == OutputKind::Movie || kind == OutputKind::Audio;
    const bool audioOn = s.hasAudio();
    const AudioCodecInfo* a = findAudioCodec(s.audio.codec);
    m_audioForm->setRowVisible(m_audioEnabled, kind == OutputKind::Movie);
    m_audioForm->setRowVisible(m_audioCodec, audioPossible);
    m_audioForm->setRowVisible(m_audioBitrate, audioOn && a && !a->lossless);
    m_audioForm->setRowVisible(m_sampleRate, audioOn);
    m_audioForm->setRowVisible(m_bitDepth, audioOn && s.audio.codec == QLatin1String("flac"));
    m_tabs->setTabEnabled(1, audioPossible);
    m_rangeEntire->setEnabled(kind != OutputKind::Still);
    m_addButton->setEnabled(s.hasVideo() || s.hasAudio());

    // Encoder availability annotations.
    for (int i = 0; i < m_encoder->count(); ++i) {
        const auto backend = EncoderBackend(m_encoder->itemData(i).toInt());
        QString text = encoderBackendName(backend);
        if (backend != EncoderBackend::Auto && backend != EncoderBackend::Software
            && !m_hwStatus.isEmpty()) {
            bool available = false;
            for (const HwEncoderStatus& hw : m_hwStatus) {
                available |= hw.backend == backend && hw.codecId == s.video.codec && hw.available;
            }
            if (!available) {
                text += tr(" (not available)");
            }
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
    if (!m_presets->apply(id, s)) {
        return;
    }
    setSettings(s);
    m_presetId = id;
    for (int i = 0; i < m_presetList->count(); ++i) {
        QListWidgetItem* item = m_presetList->item(i);
        item->setSelected(item->data(Qt::UserRole).toString() == id);
    }
    Q_EMIT settingsChanged();
}

void RenderSettingsPanel::setHardwareStatus(const QList<HwEncoderStatus>& status)
{
    m_hwStatus = status;
    QStringList available;
    for (const HwEncoderStatus& hw : status) {
        if (hw.available) {
            available << hw.encoderName;
        }
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
        if (dir.isEmpty()) {
            dir = QDir::homePath();
        }
    }
    QString name = baseName;
    if (name.isEmpty()) {
        name = m_ctx.project ? m_ctx.project->name() : QString();
    }
    m_location->setText(QDir::toNativeSeparators(dir));
    m_fileName->setText(name.isEmpty() ? tr("Untitled") : name);
}

void RenderSettingsPanel::browseLocation()
{
    const QString dir
        = QFileDialog::getExistingDirectory(this, tr("Render Location"), m_location->text());
    if (!dir.isEmpty()) {
        m_location->setText(QDir::toNativeSeparators(dir));
    }
}

void RenderSettingsPanel::savePreset()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Save Preset"), tr("Preset name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    QString error;
    const QString id = m_presets->saveUserPreset(name, settings(), &error);
    if (id.isEmpty()) {
        QMessageBox::warning(this, tr("Save Preset"), error);
        return;
    }
    m_presetId = id;
    rebuildPresetStrip();
}

void RenderSettingsPanel::deletePreset()
{
    const QList<QListWidgetItem*> selected = m_presetList->selectedItems();
    const QString id
        = selected.isEmpty() ? m_presetId : selected.first()->data(Qt::UserRole).toString();
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
    rebuildPresetStrip();
}

} // namespace kk
