// SPDX-License-Identifier: GPL-3.0-or-later
// PreferencesDialog implementation.
#include "ui/dialogs/PreferencesDialog.h"

#include "media/HwAccel.h"
#include "ui/common/SettingsPages.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

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

void applyHwAccelPreference()
{
    media::setPreferredHwAccel(toMediaMode(prefs::hwAccel()));
}

// ---- Standard pages ----

/// Hardware acceleration choice with the backends that work on this machine listed next to it.
class PlaybackSettingsPage final : public SettingsPage
{
public:
    explicit PlaybackSettingsPage(QWidget* parent)
        : SettingsPage(parent)
    {
        auto* form = new QFormLayout(this);
        m_hwAccel = new QComboBox;
        for (const QString& id : prefs::hwAccelIds())
            m_hwAccel->addItem(hwAccelLabel(prefs::hwAccelFromString(id)), id);
        m_hwAccel->setCurrentIndex(m_hwAccel->findData(prefs::hwAccelToString(prefs::hwAccel())));
        form->addRow(PreferencesDialog::tr("Hardware acceleration"), m_hwAccel);
        m_hwInfo = new QLabel(PreferencesDialog::tr("Checking available hardware…"));
        m_hwInfo->setObjectName(QStringLiteral("HwInfo"));
        m_hwInfo->setWordWrap(true);
        m_hwInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
        // Secondary text in the palette's disabled text colour (KDE "inactive text"
        // convention), so it stays readable in light and dark colour schemes.
        m_hwInfo->setForegroundRole(QPalette::PlaceholderText);
        form->addRow(QString(), m_hwInfo);

        // Probing opens GPU devices (up to ~1 s the first time): never on the GUI thread.
        auto* watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
            m_hwInfo->setText(watcher->result());
            // The list arrives after the dialog was laid out: let the window grow so the
            // wrapped lines are not clipped.
            if (QWidget* w = window(); w && w->isWindow() && w != this)
                w->adjustSize();
        });
        watcher->setFuture(QtConcurrent::run(describeCapabilities));
    }

    void apply() override
    {
        prefs::setHwAccel(prefs::hwAccelFromString(m_hwAccel->currentData().toString()));
        applyHwAccelPreference();
    }

private:
    QComboBox* m_hwAccel = nullptr;
    QLabel* m_hwInfo = nullptr;
};

/// Autosave interval.
class ProjectSettingsPage final : public SettingsPage
{
public:
    explicit ProjectSettingsPage(QWidget* parent)
        : SettingsPage(parent)
    {
        auto* form = new QFormLayout(this);
        m_autosave = new QSpinBox;
        m_autosave->setRange(0, kMaxAutosaveMinutes);
        m_autosave->setSuffix(PreferencesDialog::tr(" min"));
        m_autosave->setSpecialValueText(PreferencesDialog::tr("Off"));
        m_autosave->setValue(prefs::autosaveMinutes());
        form->addRow(PreferencesDialog::tr("Autosave every"), m_autosave);
    }

    void apply() override { prefs::setAutosaveMinutes(m_autosave->value()); }

private:
    QSpinBox* m_autosave = nullptr;
};

/// Interface scale (applies after a restart). Colours and widget style are on the
/// "appearance" page registered by the application's theme (src/app/Theme).
class InterfaceSettingsPage final : public SettingsPage
{
public:
    explicit InterfaceSettingsPage(QWidget* parent)
        : SettingsPage(parent)
    {
        auto* form = new QFormLayout(this);
        m_scale = new QComboBox;
        m_scale->addItem(PreferencesDialog::tr("Automatic (system)"), QStringLiteral("auto"));
        for (const char* factor : {"1", "1.25", "1.5", "1.75", "2"}) {
            const QString value = QLatin1String(factor);
            m_scale->addItem(QStringLiteral("%1%").arg(qRound(value.toDouble() * 100)), value);
        }
        const QString scale
            = QSettings().value(QLatin1String(prefs::kUiScale), QStringLiteral("auto")).toString();
        m_scale->setCurrentIndex(std::max(0, m_scale->findData(scale)));
        m_scale->setToolTip(
            PreferencesDialog::tr("Takes effect after restarting the application."));
        form->addRow(PreferencesDialog::tr("Interface scale"), m_scale);
    }

    void apply() override
    {
        QSettings().setValue(QLatin1String(prefs::kUiScale), m_scale->currentData().toString());
    }

private:
    QComboBox* m_scale = nullptr;
};

} // namespace

void registerStandardSettingsPages(SettingsPageRegistry& registry)
{
    registry.add({QStringLiteral("playback"), PreferencesDialog::tr("Playback and Decoding"),
        QStringLiteral("media-playback-start"), 10,
        [](QWidget* parent) { return new PlaybackSettingsPage(parent); }});
    registry.add({QStringLiteral("project"), PreferencesDialog::tr("Project"),
        QStringLiteral("document-save"), 20,
        [](QWidget* parent) { return new ProjectSettingsPage(parent); }});
    registry.add({QStringLiteral("interface"), PreferencesDialog::tr("Interface"),
        QStringLiteral("preferences-desktop-display"), 40,
        [](QWidget* parent) { return new InterfaceSettingsPage(parent); }});
}

void applyHwAccelPreference()
{
    media::setPreferredHwAccel(toMediaMode(prefs::hwAccel()));
}

PreferencesDialog::PreferencesDialog(const SettingsPageRegistry* registry, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));
    setMinimumWidth(520);

    // Without a registry (tests, tools) the dialog shows the standard pages.
    SettingsPageRegistry standard;
    if (!registry) {
        registerStandardSettingsPages(standard);
        registry = &standard;
    }

    // Resolve-style layout: every page as a titled group, stacked vertically.
    auto* layout = new QVBoxLayout(this);
    for (const SettingsPageInfo& info : registry->pages()) {
        auto* group = new QGroupBox(info.title);
        group->setObjectName(QStringLiteral("SettingsGroup_") + info.id);
        auto* groupLayout = new QVBoxLayout(group);
        groupLayout->setContentsMargins(0, 0, 0, 0);
        SettingsPage* page = info.create(group);
        page->setObjectName(QStringLiteral("SettingsPage_") + info.id);
        groupLayout->addWidget(page);
        m_pages.append(page);
        layout->addWidget(group);
    }
    layout->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void PreferencesDialog::apply()
{
    for (SettingsPage* page : std::as_const(m_pages))
        page->apply();
    emit preferencesChanged();
}

void PreferencesDialog::accept()
{
    apply();
    QDialog::accept();
}

} // namespace kk
