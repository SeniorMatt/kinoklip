// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectSettingsDialog implementation.
#include "ui/dialogs/ProjectSettingsDialog.h"

#include "core/Preferences.h"
#include "core/Project.h"
#include "export/ProxyMedia.h"
#include "ui/common/Metrics.h"
#include "ui/common/SchemeColors.h"
#include "ui/common/TimecodeFormat.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace kk {

namespace {

struct ResolutionPreset
{
    const char* label;
    int width;
    int height;
};

constexpr ResolutionPreset kPresets[] = {
    {QT_TRANSLATE_NOOP("kk::ProjectSettingsDialog", "UHD 3840 x 2160"), 3840, 2160},
    {QT_TRANSLATE_NOOP("kk::ProjectSettingsDialog", "QHD 2560 x 1440"), 2560, 1440},
    {QT_TRANSLATE_NOOP("kk::ProjectSettingsDialog", "HD 1920 x 1080"), 1920, 1080},
    {QT_TRANSLATE_NOOP("kk::ProjectSettingsDialog", "HD 1280 x 720"), 1280, 720},
    {QT_TRANSLATE_NOOP("kk::ProjectSettingsDialog", "Vertical 1080 x 1920"), 1080, 1920},
    {QT_TRANSLATE_NOOP("kk::ProjectSettingsDialog", "Square 1080 x 1080"), 1080, 1080},
};
constexpr int kCustomPreset = int(std::size(kPresets));
constexpr int kMinDimension = 16;
constexpr int kMaxDimension = 8192;
constexpr int kSampleRate = 48000; ///< the mixer's fixed rate (PLAN 5.1)
const QSize kDialogSize(820, 560);
constexpr int kSectionListWidth = 200;
constexpr int kTitlePointDelta = 2; ///< section title: base font + 2 pt
constexpr int kMaxAutosaveMinutes = 120;

QFormLayout* makeForm()
{
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(metrics::kSpaceL);
    form->setVerticalSpacing(metrics::kSpaceM);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    return form;
}

/// A widget holding @p form, with every label in one column of the shared minimum width.
QWidget* formWidget(QFormLayout* form)
{
    auto* widget = new QWidget;
    widget->setLayout(form);
    for (int row = 0; row < form->rowCount(); ++row) {
        if (QLayoutItem* item = form->itemAt(row, QFormLayout::LabelRole)) {
            if (QWidget* label = item->widget())
                label->setMinimumWidth(metrics::kFormLabelMinWidth + metrics::kSpaceXl * 3);
        }
        if (QLayoutItem* item = form->itemAt(row, QFormLayout::FieldRole)) {
            if (auto* combo = qobject_cast<QComboBox*>(item->widget()))
                combo->setMinimumWidth(metrics::kFormComboMinWidth);
        }
    }
    return widget;
}

QLabel* subHeading(const QString& text)
{
    auto* label = new QLabel(text);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    return label;
}

/// A combo that shows a value the engine does not let the user change yet.
QComboBox* fixedCombo(const QString& value, const QString& why)
{
    auto* combo = new QComboBox;
    combo->addItem(value);
    combo->setEnabled(false);
    combo->setToolTip(why);
    return combo;
}

QString fpsLabel(Rational fps)
{
    return QString::number(fps.toDouble(), 'g', 5);
}

} // namespace

QVector<Rational> ProjectSettingsDialog::frameRates()
{
    return {
        {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1}};
}

ProjectSettingsDialog::ProjectSettingsDialog(Project* project, QWidget* parent)
    : QDialog(parent)
    , m_project(project)
{
    setWindowTitle(tr("Project Settings: %1").arg(project->name()));
    resize(kDialogSize);

    m_sections = new QListWidget;
    m_sections->setObjectName(QStringLiteral("ProjectSettingsSections"));
    m_sections->setFixedWidth(kSectionListWidth);
    m_sections->setFrameShape(QFrame::NoFrame);
    m_sections->setSpacing(metrics::kSpaceXs);
    m_stack = new QStackedWidget;
    connect(m_sections, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);

    addSection(QStringLiteral("master"), tr("Master Settings"), buildMaster());
    addSection(QStringLiteral("scaling"), tr("Image Scaling"), buildScaling());
    addSection(QStringLiteral("audio"), tr("Audio"), buildAudio());
    addSection(QStringLiteral("general"), tr("General Options"), buildGeneral());
    addSection(QStringLiteral("cache"), tr("Optimized Media and Render Cache"), buildCache());
    m_sections->setCurrentRow(0);

    auto* separator = new QFrame;
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    auto* body = new QHBoxLayout;
    body->setSpacing(0);
    body->addWidget(m_sections);
    body->addWidget(separator);
    body->addWidget(m_stack, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        metrics::kSpaceL, metrics::kSpaceL, metrics::kSpaceL, metrics::kSpaceL);
    layout->setSpacing(metrics::kSpaceM);
    layout->addLayout(body, 1);
    layout->addWidget(buttons);

    connect(m_preset, &QComboBox::currentIndexChanged, this, &ProjectSettingsDialog::presetChanged);
    connect(m_fps, &QComboBox::currentIndexChanged, this, &ProjectSettingsDialog::updateWarning);
    connect(m_fps, &QComboBox::currentIndexChanged, this,
        [this] { m_playbackFps->setItemText(0, m_fps->currentText()); });
    setSettings(project->settings());
}

void ProjectSettingsDialog::addSection(const QString& id, const QString& title, QWidget* page)
{
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("ProjectSettingsSection_") + id);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(
        metrics::kSpaceXl, metrics::kSpaceL, metrics::kSpaceXl, metrics::kSpaceL);
    layout->setSpacing(metrics::kSpaceL);
    auto* heading = new QLabel(title);
    QFont font = heading->font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() + kTitlePointDelta);
    heading->setFont(font);
    layout->addWidget(heading);
    layout->addWidget(page);
    layout->addStretch(1);
    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_stack->addWidget(scroll);
    auto* item = new QListWidgetItem(title, m_sections);
    item->setData(Qt::UserRole, id);
}

bool ProjectSettingsDialog::showSection(const QString& sectionId)
{
    for (int row = 0; row < m_sections->count(); ++row) {
        if (m_sections->item(row)->data(Qt::UserRole).toString() == sectionId) {
            m_sections->setCurrentRow(row);
            return true;
        }
    }
    return false;
}

QWidget* ProjectSettingsDialog::buildMaster()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(metrics::kSpaceL);

    auto* form = makeForm();
    m_preset = new QComboBox;
    m_preset->setObjectName(QStringLiteral("TimelineResolution"));
    for (const ResolutionPreset& preset : kPresets)
        m_preset->addItem(tr(preset.label));
    m_preset->addItem(tr("Custom"));
    form->addRow(tr("Timeline resolution"), m_preset);

    m_width = new QSpinBox;
    m_width->setObjectName(QStringLiteral("TimelineWidth"));
    m_height = new QSpinBox;
    m_height->setObjectName(QStringLiteral("TimelineHeight"));
    for (QSpinBox* spin : {m_width, m_height}) {
        spin->setRange(kMinDimension, kMaxDimension);
        spin->setSingleStep(2); // encoders want even sizes
        spin->setSuffix(tr(" px"));
    }
    auto* size = new QHBoxLayout;
    size->setSpacing(metrics::kSpaceS);
    size->addWidget(m_width);
    size->addWidget(new QLabel(QStringLiteral("x")));
    size->addWidget(m_height);
    size->addStretch(1);
    form->addRow(QString(), size);
    form->addRow(tr("Pixel aspect ratio"),
        fixedCombo(tr("Square"),
            tr("Kinoklip works with square pixels; anamorphic pixel "
               "aspect ratios are coming in a later version.")));

    m_fps = new QComboBox;
    m_fps->setObjectName(QStringLiteral("TimelineFrameRate"));
    for (const Rational& fps : frameRates())
        m_fps->addItem(
            tr("%1 fps").arg(fpsLabel(fps)), QVariant::fromValue(QPoint(fps.num, fps.den)));
    form->addRow(tr("Timeline frame rate"), m_fps);
    m_playbackFps = fixedCombo(
        QString(), tr("Playback always runs at the timeline frame rate in this version."));
    form->addRow(tr("Playback frame rate"), m_playbackFps);
    layout->addWidget(formWidget(form));

    m_warning = new QLabel;
    m_warning->setWordWrap(true);
    QPalette warn = m_warning->palette();
    // The scheme's "neutral" (warning) text colour.
    warn.setColor(QPalette::WindowText, schemeColors().neutral);
    m_warning->setPalette(warn);
    layout->addWidget(m_warning);

    layout->addWidget(subHeading(tr("Frame Interpolation")));
    auto* interpolation = makeForm();
    m_retimeProcess = new QComboBox;
    m_retimeProcess->setObjectName(QStringLiteral("RetimeProcess"));
    m_retimeProcess->addItem(tr("Nearest"), int(RetimeProcess::Nearest));
    m_retimeProcess->addItem(tr("Frame Blend"), int(RetimeProcess::FrameBlend));
    m_retimeProcess->addItem(tr("Optical Flow"), int(RetimeProcess::OpticalFlow));
    m_retimeProcess->setToolTip(tr("How clips playing at another speed make the frames between "
                                   "two source frames, unless a clip chooses its own."));
    interpolation->addRow(tr("Retime process"), m_retimeProcess);
    layout->addWidget(formWidget(interpolation));

    layout->addWidget(subHeading(tr("Video Monitoring")));
    auto* monitoring = makeForm();
    const QString noDevice = tr("Output to video capture and playback devices is coming in a "
                                "later version.");
    monitoring->addRow(tr("Video format"), fixedCombo(tr("None"), noDevice));
    monitoring->addRow(tr("Data levels"), fixedCombo(tr("Video"), noDevice));
    layout->addWidget(formWidget(monitoring));
    return page;
}

QWidget* ProjectSettingsDialog::buildScaling()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(metrics::kSpaceL);
    const QString why = tr("Clips that do not match the timeline resolution are scaled to fit "
                           "it (letterboxed). Crop and stretch are coming in a later version.");
    layout->addWidget(subHeading(tr("Input Scaling")));
    auto* input = makeForm();
    auto* mismatch = fixedCombo(tr("Scale entire image to fit"), why);
    mismatch->setObjectName(QStringLiteral("MismatchedResolution"));
    mismatch->addItems({tr("Scale full frame with crop"), tr("Stretch frame to all corners"),
        tr("Center crop with no resizing")});
    input->addRow(tr("Mismatched resolution files"), mismatch);
    layout->addWidget(formWidget(input));
    layout->addWidget(subHeading(tr("Output Scaling")));
    auto* output = makeForm();
    output->addRow(tr("Output resolution"),
        fixedCombo(tr("Match timeline"),
            tr("The Deliver page chooses the render resolution of each job.")));
    layout->addWidget(formWidget(output));
    return page;
}

QWidget* ProjectSettingsDialog::buildAudio()
{
    auto* form = makeForm();
    m_sampleRate = fixedCombo(tr("48000 Hz"), tr("Audio is mixed at 48 kHz in this version."));
    m_sampleRate->setItemData(0, kSampleRate);
    m_sampleRate->setObjectName(QStringLiteral("SampleRate"));
    form->addRow(tr("Audio sample rate"), m_sampleRate);
    form->addRow(tr("Audio bit depth"),
        fixedCombo(tr("32-bit float"),
            tr("The mixer works in 32-bit float; render bit depth is set on the Deliver page.")));
    return formWidget(form);
}

QWidget* ProjectSettingsDialog::buildGeneral()
{
    auto* form = makeForm();
    m_name = new QLineEdit;
    m_name->setObjectName(QStringLiteral("ProjectName"));
    form->addRow(tr("Project name"), m_name);
    m_timelineStart = new QLineEdit;
    m_timelineStart->setObjectName(QStringLiteral("TimelineStartTimecode"));
    m_timelineStart->setInputMask(QStringLiteral("99:99:99:99"));
    m_timelineStart->setToolTip(tr("Timecode of the first frame of new timelines. Each "
                                   "timeline keeps its own (Timeline Settings)."));
    form->addRow(tr("Timeline starting timecode"), m_timelineStart);
    m_autosave = new QSpinBox;
    m_autosave->setObjectName(QStringLiteral("AutosaveMinutes"));
    m_autosave->setRange(0, kMaxAutosaveMinutes);
    m_autosave->setSuffix(tr(" min"));
    m_autosave->setSpecialValueText(tr("Off"));
    m_autosave->setValue(prefs::autosaveMinutes());
    m_autosave->setToolTip(tr("Applies to every project (also in Preferences > User > Project "
                              "Save and Load)."));
    form->addRow(tr("Autosave every"), m_autosave);
    return formWidget(form);
}

QWidget* ProjectSettingsDialog::buildCache()
{
    // Resolve: Master Settings > Optimized Media and Render Cache.
    auto* form = makeForm();
    auto resolutionCombo = [](const char* name) {
        auto* c = new QComboBox;
        c->setObjectName(QLatin1String(name));
        c->addItem(tr("Choose automatically"), int(ProxyResolution::Auto));
        c->addItem(tr("Original"), int(ProxyResolution::Original));
        c->addItem(tr("Half"), int(ProxyResolution::Half));
        c->addItem(tr("Quarter"), int(ProxyResolution::Quarter));
        return c;
    };
    auto formatCombo = [](const char* name, const QVector<IntermediateFormat>& formats) {
        auto* c = new QComboBox;
        c->setObjectName(QLatin1String(name));
        for (const IntermediateFormat& f : formats)
            c->addItem(f.name, f.id);
        return c;
    };
    m_proxyResolution = resolutionCombo("ProxyResolution");
    m_proxyResolution->setToolTip(tr("Choose automatically: the largest of Original, Half and "
                                     "Quarter that is at most 540 lines high."));
    form->addRow(tr("Proxy media resolution"), m_proxyResolution);
    m_proxyFormat = formatCombo("ProxyFormat", proxyFormats());
    m_proxyFormat->setToolTip(tr("H.264 (fast decode) gives the smallest files and the fastest "
                                 "playback on laptops; hardware encoded when available."));
    form->addRow(tr("Proxy media format"), m_proxyFormat);
    m_optimizedResolution = resolutionCombo("OptimizedResolution");
    form->addRow(tr("Optimized media resolution"), m_optimizedResolution);
    m_optimizedFormat = formatCombo("OptimizedFormat", proxyFormats());
    form->addRow(tr("Optimized media format"), m_optimizedFormat);
    m_cacheFormat = formatCombo("RenderCacheFormat", renderCacheFormats());
    m_cacheFormat->setToolTip(tr("Intra-frame codec of cached frames (timeline resolution). "
                                 "DNxHR LB decodes fastest; SQ / ProRes 422 look closer to "
                                 "the uncached picture."));
    form->addRow(tr("Render cache format"), m_cacheFormat);

    auto* background = new QWidget;
    auto* row = new QHBoxLayout(background);
    row->setContentsMargins(0, 0, 0, 0);
    m_backgroundCache = new QCheckBox(tr("Enable background caching after"));
    m_backgroundCache->setObjectName(QStringLiteral("BackgroundCache"));
    m_backgroundDelay = new QSpinBox;
    m_backgroundDelay->setObjectName(QStringLiteral("BackgroundCacheDelay"));
    m_backgroundDelay->setRange(0, 600);
    m_backgroundDelay->setSuffix(tr(" seconds"));
    row->addWidget(m_backgroundCache);
    row->addWidget(m_backgroundDelay);
    row->addStretch(1);
    form->addRow(QString(), background);
    m_userTransitions = new QCheckBox(tr("Automatically cache transitions in User Mode"));
    m_userTransitions->setObjectName(QStringLiteral("UserCacheTransitions"));
    form->addRow(QString(), m_userTransitions);
    m_userComposites = new QCheckBox(tr("Automatically cache composites in User Mode"));
    m_userComposites->setObjectName(QStringLiteral("UserCacheComposites"));
    form->addRow(QString(), m_userComposites);

    auto select = [](QComboBox* c, const QVariant& v) {
        const int i = c->findData(v);
        if (i >= 0)
            c->setCurrentIndex(i);
    };
    const MediaCacheSettings& cs = m_project->cacheSettings();
    select(m_proxyResolution, int(cs.proxyResolution));
    select(m_proxyFormat, cs.proxyFormat);
    select(m_optimizedResolution, int(cs.optimizedResolution));
    select(m_optimizedFormat, cs.optimizedFormat);
    select(m_cacheFormat, cs.renderCacheFormat);
    m_backgroundCache->setChecked(cs.backgroundCache);
    m_backgroundDelay->setValue(cs.backgroundCacheDelaySec);
    m_userTransitions->setChecked(cs.userCacheTransitions);
    m_userComposites->setChecked(cs.userCacheComposites);
    return formWidget(form);
}

MediaCacheSettings ProjectSettingsDialog::cacheSettings() const
{
    MediaCacheSettings s = m_project->cacheSettings(); // keeps the Playback menu modes
    s.proxyResolution = ProxyResolution(m_proxyResolution->currentData().toInt());
    s.proxyFormat = m_proxyFormat->currentData().toString();
    s.optimizedResolution = ProxyResolution(m_optimizedResolution->currentData().toInt());
    s.optimizedFormat = m_optimizedFormat->currentData().toString();
    s.renderCacheFormat = m_cacheFormat->currentData().toString();
    s.backgroundCache = m_backgroundCache->isChecked();
    s.backgroundCacheDelaySec = m_backgroundDelay->value();
    s.userCacheTransitions = m_userTransitions->isChecked();
    s.userCacheComposites = m_userComposites->isChecked();
    return s;
}

ProjectSettings ProjectSettingsDialog::settings() const
{
    ProjectSettings s;
    s.name = m_name->text().trimmed();
    if (s.name.isEmpty())
        s.name = m_project->name();
    s.width = m_width->value();
    s.height = m_height->value();
    const QPoint fps = m_fps->currentData().toPoint();
    s.fps = Rational {fps.x(), fps.y()};
    s.retimeProcess = RetimeProcess(m_retimeProcess->currentData().toInt());
    // The typed starting timecode, in frames of the chosen rate; unchanged text keeps the
    // current value (Project::setSettings() carries it over to a new rate).
    const ProjectSettings current = m_project->settings();
    s.timelineStart = current.timelineStart;
    if (m_timelineStart->text() != timecode::format(current.timelineStart, current.fps)) {
        if (const auto frames = timecode::parse(m_timelineStart->text(), s.fps))
            s.timelineStart = std::max<int64_t>(0, *frames);
    }
    return s;
}

void ProjectSettingsDialog::setSettings(const ProjectSettings& settings)
{
    m_name->setText(settings.name);
    int preset = kCustomPreset;
    for (int i = 0; i < kCustomPreset; ++i) {
        if (kPresets[i].width == settings.width && kPresets[i].height == settings.height)
            preset = i;
    }
    m_width->setValue(settings.width);
    m_height->setValue(settings.height);
    m_preset->setCurrentIndex(preset);
    presetChanged(preset);
    int fpsIndex = m_fps->findData(QPoint(settings.fps.num, settings.fps.den));
    if (fpsIndex < 0) {
        // A rate from an imported project that is not in the list: show it anyway.
        m_fps->addItem(tr("%1 fps").arg(fpsLabel(settings.fps)),
            QVariant::fromValue(QPoint(settings.fps.num, settings.fps.den)));
        fpsIndex = m_fps->count() - 1;
    }
    m_fps->setCurrentIndex(fpsIndex);
    m_playbackFps->setItemText(0, m_fps->currentText());
    const int process = m_retimeProcess->findData(int(settings.retimeProcess));
    m_retimeProcess->setCurrentIndex(std::max(0, process));
    m_timelineStart->setText(timecode::format(settings.timelineStart, settings.fps));
    updateWarning();
}

void ProjectSettingsDialog::presetChanged(int index)
{
    const bool custom = index == kCustomPreset;
    m_width->setEnabled(custom);
    m_height->setEnabled(custom);
    if (!custom && index >= 0) {
        m_width->setValue(kPresets[index].width);
        m_height->setValue(kPresets[index].height);
    }
}

void ProjectSettingsDialog::updateWarning()
{
    // As in Resolve the timeline frame rate is locked once a timeline that follows the
    // project settings has clips: their frame positions would no longer match their media
    // (spec 25.13). A new timeline with its own frame rate is the way to change it.
    bool hasClips = false;
    for (const quint64 uid : m_project->timelineUids()) {
        const auto data = m_project->timelineData(uid);
        if (data && data->useProjectSettings && data->duration() > 0)
            hasClips = true;
    }
    m_fps->setEnabled(!hasClips);
    m_fps->setToolTip(hasClips ? tr("The frame rate cannot be changed once a timeline has "
                                    "clips. Create a new timeline and turn off \"Use project "
                                    "settings\" in its Timeline Settings to use another rate.")
                               : QString());
    m_warning->setVisible(false);
}

void ProjectSettingsDialog::apply()
{
    m_project->setSettings(settings());
    m_project->setCacheSettings(cacheSettings());
    if (m_autosave->value() != prefs::autosaveMinutes()) {
        prefs::setAutosaveMinutes(m_autosave->value());
        emit preferencesChanged();
    }
}

void ProjectSettingsDialog::accept()
{
    apply();
    QDialog::accept();
}

} // namespace kk
