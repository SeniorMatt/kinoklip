// SPDX-License-Identifier: GPL-3.0-or-later
// ProjectSettingsDialog implementation.
#include "ui/dialogs/ProjectSettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

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
constexpr int kDialogMinWidth = 440;

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
    setWindowTitle(tr("Project Settings"));
    setMinimumWidth(kDialogMinWidth);
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    m_name = new QLineEdit;
    form->addRow(tr("Project name"), m_name);

    m_preset = new QComboBox;
    for (const ResolutionPreset& preset : kPresets)
        m_preset->addItem(tr(preset.label));
    m_preset->addItem(tr("Custom"));
    form->addRow(tr("Timeline resolution"), m_preset);

    m_width = new QSpinBox;
    m_height = new QSpinBox;
    for (QSpinBox* spin : {m_width, m_height}) {
        spin->setRange(kMinDimension, kMaxDimension);
        spin->setSingleStep(2); // encoders want even sizes
        spin->setSuffix(tr(" px"));
    }
    auto* size = new QHBoxLayout;
    size->addWidget(m_width);
    size->addWidget(new QLabel(QStringLiteral("x")));
    size->addWidget(m_height);
    size->addStretch(1);
    form->addRow(QString(), size);

    m_fps = new QComboBox;
    for (const Rational& fps : frameRates())
        m_fps->addItem(
            tr("%1 fps").arg(fpsLabel(fps)), QVariant::fromValue(QPoint(fps.num, fps.den)));
    form->addRow(tr("Frame rate"), m_fps);

    m_sampleRate = new QComboBox;
    m_sampleRate->addItem(tr("48000 Hz"), kSampleRate);
    m_sampleRate->setEnabled(false);
    m_sampleRate->setToolTip(tr("Audio is mixed at 48 kHz in this version."));
    form->addRow(tr("Audio sample rate"), m_sampleRate);

    m_warning = new QLabel;
    m_warning->setWordWrap(true);
    QPalette warn = m_warning->palette();
    // KDE "neutral" (warning) text colours of Breeze Dark / Breeze Light, picked by the
    // lightness of the active scheme so the warning stays readable in both.
    const bool darkScheme = warn.color(QPalette::Window).lightness() < 128;
    warn.setColor(
        QPalette::WindowText, darkScheme ? QColor(0xf6, 0x74, 0x00) : QColor(0xc6, 0x5c, 0x00));
    m_warning->setPalette(warn);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_warning);
    layout->addStretch(1);
    layout->addWidget(buttons);

    connect(m_preset, &QComboBox::currentIndexChanged, this, &ProjectSettingsDialog::presetChanged);
    connect(m_fps, &QComboBox::currentIndexChanged, this, &ProjectSettingsDialog::updateWarning);
    setSettings(project->settings());
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
    const bool fpsChanged = settings().fps != m_project->settings().fps;
    const bool hasClips = m_project->timeline()->data().duration() > 0;
    m_warning->setVisible(fpsChanged && hasClips);
    m_warning->setText(tr("Changing the frame rate keeps clip positions in frames, so the "
                          "timeline will play faster or slower."));
}

void ProjectSettingsDialog::apply()
{
    m_project->setSettings(settings());
}

void ProjectSettingsDialog::accept()
{
    apply();
    QDialog::accept();
}

} // namespace kk
