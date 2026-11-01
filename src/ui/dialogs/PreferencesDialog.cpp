// SPDX-License-Identifier: GPL-3.0-or-later
// PreferencesDialog implementation.
#include "ui/dialogs/PreferencesDialog.h"

#include "media/HwAccel.h"
#include "ui/common/ActionRegistry.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QLabel>
#include <QPointer>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace kk {

namespace {

constexpr int kMaxAutosaveMinutes = 120;

QString hwAccelLabel(prefs::HwAccel mode)
{
    switch (mode) {
    case prefs::HwAccel::Auto:
        return PreferencesDialog::tr("Automatic");
    case prefs::HwAccel::Off:
        return PreferencesDialog::tr("Off (software only)");
    case prefs::HwAccel::Vaapi:
        return PreferencesDialog::tr("VA-API (Intel, AMD)");
    case prefs::HwAccel::Cuda:
        return PreferencesDialog::tr("CUDA / NVDEC (NVIDIA)");
    case prefs::HwAccel::Qsv:
        return PreferencesDialog::tr("Quick Sync Video (Intel)");
    case prefs::HwAccel::Vulkan:
        return PreferencesDialog::tr("Vulkan Video (experimental)");
    }
    return {};
}

media::HwAccelMode toMediaMode(prefs::HwAccel mode)
{
    switch (mode) {
    case prefs::HwAccel::Off:
        return media::HwAccelMode::Off;
    case prefs::HwAccel::Vaapi:
    case prefs::HwAccel::Qsv: // Intel decode on Linux goes through VA-API
        return media::HwAccelMode::Vaapi;
    case prefs::HwAccel::Cuda:
        return media::HwAccelMode::Cuda;
    case prefs::HwAccel::Vulkan:
        return media::HwAccelMode::Vulkan;
    case prefs::HwAccel::Auto:
        break;
    }
    return media::HwAccelMode::Auto;
}

/// One line per backend: "VAAPI: available (/dev/dri/renderD128) - h264, hevc" or the error.
QString describeCapabilities()
{
    QStringList lines;
    for (const media::HwDeviceInfo& info : media::hwCapabilities()) {
        if (info.available) {
            QString line = PreferencesDialog::tr("%1: available").arg(info.name);
            if (!info.device.isEmpty())
                line += QStringLiteral(" (%1)").arg(info.device);
            if (!info.codecs.isEmpty())
                line += QStringLiteral(" - ") + info.codecs.join(QStringLiteral(", "));
            lines.append(line);
        } else {
            lines.append(PreferencesDialog::tr("%1: not available%2")
                             .arg(info.name,
                                 info.error.isEmpty() ? QString()
                                                      : QStringLiteral(" (%1)").arg(info.error)));
        }
    }
    return lines.isEmpty() ? PreferencesDialog::tr("No hardware decoders found.")
                           : lines.join(QLatin1Char('\n'));
}

} // namespace

void applyHwAccelPreference()
{
    media::setPreferredHwAccel(toMediaMode(prefs::hwAccel()));
}

PreferencesDialog::PreferencesDialog(ActionRegistry* actions, QWidget* parent)
    : QDialog(parent)
    , m_actions(actions)
{
    setWindowTitle(tr("Preferences"));
    setMinimumWidth(520);

    // ---- Playback ----
    auto* playback = new QGroupBox(tr("Playback and Decoding"));
    auto* playbackForm = new QFormLayout(playback);
    m_hwAccel = new QComboBox;
    for (const QString& id : prefs::hwAccelIds())
        m_hwAccel->addItem(hwAccelLabel(prefs::hwAccelFromString(id)), id);
    m_hwAccel->setCurrentIndex(m_hwAccel->findData(prefs::hwAccelToString(prefs::hwAccel())));
    playbackForm->addRow(tr("Hardware acceleration"), m_hwAccel);
    m_hwInfo = new QLabel(tr("Checking available hardware…"));
    m_hwInfo->setObjectName(QStringLiteral("HwInfo"));
    m_hwInfo->setWordWrap(true);
    m_hwInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPalette dim = m_hwInfo->palette();
    QColor dimText = dim.color(QPalette::WindowText);
    dimText.setAlphaF(0.65f);
    dim.setColor(QPalette::WindowText, dimText);
    m_hwInfo->setPalette(dim);
    playbackForm->addRow(QString(), m_hwInfo);

    // ---- Project ----
    auto* project = new QGroupBox(tr("Project"));
    auto* projectForm = new QFormLayout(project);
    m_autosave = new QSpinBox;
    m_autosave->setRange(0, kMaxAutosaveMinutes);
    m_autosave->setSuffix(tr(" min"));
    m_autosave->setSpecialValueText(tr("Off"));
    m_autosave->setValue(prefs::autosaveMinutes());
    projectForm->addRow(tr("Autosave every"), m_autosave);

    // ---- Appearance ----
    auto* appearance = new QGroupBox(tr("Appearance"));
    auto* appearanceForm = new QFormLayout(appearance);
    m_systemColors = new QCheckBox(tr("Use system colors instead of the dark theme"));
    m_systemColors->setChecked(
        QSettings().value(QLatin1String(prefs::kUseSystemColors), false).toBool());
    appearanceForm->addRow(tr("Colors"), m_systemColors);
    m_scale = new QComboBox;
    m_scale->addItem(tr("Automatic (system)"), QStringLiteral("auto"));
    for (const char* factor : {"1", "1.25", "1.5", "1.75", "2"}) {
        const QString value = QLatin1String(factor);
        m_scale->addItem(QStringLiteral("%1%").arg(qRound(value.toDouble() * 100)), value);
    }
    const QString scale
        = QSettings().value(QLatin1String(prefs::kUiScale), QStringLiteral("auto")).toString();
    m_scale->setCurrentIndex(std::max(0, m_scale->findData(scale)));
    m_scale->setToolTip(tr("Takes effect after restarting the application."));
    appearanceForm->addRow(tr("Interface scale"), m_scale);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(playback);
    layout->addWidget(project);
    layout->addWidget(appearance);
    layout->addStretch(1);
    layout->addWidget(buttons);

    // Probing opens GPU devices (up to ~1 s the first time): never on the GUI thread.
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
        [this, watcher] { showCapabilities(watcher->result()); });
    watcher->setFuture(QtConcurrent::run(describeCapabilities));
}

void PreferencesDialog::showCapabilities(const QString& text)
{
    m_hwInfo->setText(text);
}

void PreferencesDialog::apply()
{
    prefs::setHwAccel(prefs::hwAccelFromString(m_hwAccel->currentData().toString()));
    applyHwAccelPreference();
    prefs::setAutosaveMinutes(m_autosave->value());
    QSettings().setValue(QLatin1String(prefs::kUiScale), m_scale->currentData().toString());

    // The theme switch belongs to the app (src/app/Theme): go through its action so menus and
    // the stored value stay in sync.
    const bool systemColors = m_systemColors->isChecked();
    QAction* action
        = m_actions ? m_actions->action(QStringLiteral("view.use_system_colors")) : nullptr;
    if (action && action->isChecked() != systemColors)
        action->trigger(); // toggles the checked state and emits triggered(checked)
    else if (!action)
        QSettings().setValue(QLatin1String(prefs::kUseSystemColors), systemColors);
    emit preferencesChanged();
}

void PreferencesDialog::accept()
{
    apply();
    QDialog::accept();
}

} // namespace kk
