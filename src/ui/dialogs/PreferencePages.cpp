// SPDX-License-Identifier: GPL-3.0-or-later
// PreferencePages — the standard System and User preference pages (Resolve's sections) and
// the code that applies stored preferences to the running application.
#include "core/EditBehaviour.h"
#include "media/HwAccel.h"
#include "media/ImageLoader.h"
#include "media/MediaCache.h"
#include "platform/FileDialogs.h"
#include "playback/AudioDeviceMonitor.h"
#include "playback/PlaybackEngine.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/Metrics.h"
#include "ui/common/SettingsPages.h"
#include "ui/common/UiAnimation.h"
#include "ui/dialogs/PreferencesDialog.h"
#include "ui/viewer/PlaybackResolution.h"
#include "ui/viewer/ViewerPanel.h"

#include <QAction>
#include <QAudioDevice>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace kk {

namespace {

constexpr int kMaxAutosaveMinutes = 120;
constexpr int kMinStillCacheMiB = 64;
constexpr int kMaxStillCacheMiB = 16384;
constexpr int kStillCacheStepMiB = 64;
constexpr double kMaxDefaultSeconds = 600.0; ///< longest default still / title / transition
constexpr int kMaxNudgeFrames = 100;
constexpr int kMaxRollMs = 60000;
constexpr int kMsPerSecond = 1000;
constexpr int kBytesPerMiB = 1024 * 1024;

QString tr(const char* text)
{
    return PreferencesDialog::tr(text);
}

/// A form with the shared spacing; the dialog aligns the label columns of all pages.
QFormLayout* makeForm(QWidget* page)
{
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(metrics::kSpaceL);
    form->setVerticalSpacing(metrics::kSpaceM);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    return form;
}

/// Dimmed explanatory text under a field (palette's placeholder colour: readable in light and
/// dark schemes).
QLabel* note(const QString& text)
{
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

QDoubleSpinBox* secondsSpin(double value)
{
    auto* spin = new QDoubleSpinBox;
    spin->setRange(0.1, kMaxDefaultSeconds);
    spin->setDecimals(1);
    spin->setSingleStep(0.5);
    spin->setSuffix(PreferencesDialog::tr(" s"));
    spin->setValue(value);
    return spin;
}

QString hwAccelLabel(prefs::HwAccel mode)
{
    switch (mode) {
    case prefs::HwAccel::Auto:
        return tr("Automatic");
    case prefs::HwAccel::Off:
        return tr("Off (software only)");
    case prefs::HwAccel::Vaapi:
        return tr("VA-API (Intel, AMD)");
    case prefs::HwAccel::Cuda:
        return tr("CUDA / NVDEC (NVIDIA)");
    case prefs::HwAccel::Qsv:
        return tr("Quick Sync Video (Intel)");
    case prefs::HwAccel::Vulkan:
        return tr("Vulkan Video (experimental)");
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

QString defaultCacheDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/media");
}

// ---- System: Memory and GPU ----------------------------------------------------------

class MemoryPage final : public SettingsPage
{
public:
    explicit MemoryPage(QWidget* parent)
        : SettingsPage(parent)
    {
        auto* form = makeForm(this);
        m_stillCache = new QSpinBox;
        m_stillCache->setObjectName(QStringLiteral("StillCacheMiB"));
        m_stillCache->setRange(kMinStillCacheMiB, kMaxStillCacheMiB);
        m_stillCache->setSingleStep(kStillCacheStepMiB);
        m_stillCache->setSuffix(tr(" MB"));
        m_stillCache->setValue(prefs::stillCacheMiB());
        m_stillCache->setToolTip(tr("Memory kept for decoded still images (photos, titles). "
                                    "Least recently used images are dropped first."));
        form->addRow(tr("Still image cache"), m_stillCache);

        m_gpu = new QComboBox;
        m_gpu->setObjectName(QStringLiteral("GpuProcessing"));
        m_gpu->addItem(tr("Auto (OpenGL)"), QStringLiteral("auto"));
        m_gpu->addItem(tr("CPU only"), QStringLiteral("cpu"));
        m_gpu->setCurrentIndex(prefs::forceCpuRendering() ? 1 : 0);
        form->addRow(tr("GPU processing mode"), m_gpu);
        form->addRow(QString(),
            note(tr("The GPU processing mode applies after restarting the application.")));
    }

    void apply() override
    {
        QSettings settings;
        settings.setValue(QLatin1String(prefs::kStillCacheMiB), m_stillCache->value());
        settings.setValue(QLatin1String(prefs::kRenderBackend), m_gpu->currentData());
    }

private:
    QSpinBox* m_stillCache = nullptr;
    QComboBox* m_gpu = nullptr;
};

// ---- System: Local Control -----------------------------------------------------------

/// The switch for live control of the open window by local tools (spec 55.8).
class ControlPage final : public SettingsPage
{
public:
    explicit ControlPage(QWidget* parent)
        : SettingsPage(parent)
    {
        auto* form = makeForm(this);
        m_enabled = new QCheckBox(tr("Let local tools control Kinoklip (MCP)"));
        m_enabled->setObjectName(QStringLiteral("LocalControl"));
        m_enabled->setToolTip(tr("Programs running as you on this computer, such as an AI "
                                 "assistant started with \"kinoklip mcp\", can edit the open "
                                 "project. Every call is one undo step."));
        m_enabled->setChecked(prefs::localControlEnabled());
        form->addRow(tr("Local tools"), m_enabled);
        form->addRow(QString(),
            note(tr("Lets programs you run on this computer, such as an AI assistant started "
                    "with \"kinoklip mcp\", edit the project open in this window; each of their "
                    "changes is one step you can undo. Only your own user account can connect, "
                    "and nothing is opened on the network. Off by default.")));
    }

    void apply() override { prefs::setLocalControlEnabled(m_enabled->isChecked()); }

private:
    QCheckBox* m_enabled = nullptr;
};

// ---- System: Media Storage -----------------------------------------------------------

class StoragePage final : public SettingsPage
{
public:
    explicit StoragePage(QWidget* parent)
        : SettingsPage(parent)
    {
        auto* form = makeForm(this);
        m_folder = new QLineEdit(prefs::cacheDirectory());
        m_folder->setObjectName(QStringLiteral("CacheDirectory"));
        m_folder->setPlaceholderText(QDir::toNativeSeparators(defaultCacheDirectory()));
        auto* browse = new QPushButton(tr("Browse"));
        browse->setToolTip(tr("Choose the cache folder"));
        auto* reset = new QPushButton(tr("Default"));
        reset->setToolTip(tr("Use the default cache folder"));
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(metrics::kSpaceS);
        layout->addWidget(m_folder, 1);
        layout->addWidget(browse);
        layout->addWidget(reset);
        form->addRow(tr("Cache files location"), row);
        form->addRow(QString(),
            note(tr("Thumbnails and audio waveforms of imported media are kept here so "
                    "projects open quickly.")));
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString dir = platform::getExistingDirectory(this, tr("Cache Files Location"),
                QString(), m_folder->text().isEmpty() ? defaultCacheDirectory() : m_folder->text());
            if (!dir.isEmpty())
                m_folder->setText(QDir::toNativeSeparators(dir));
        });
        connect(reset, &QPushButton::clicked, m_folder, &QLineEdit::clear);

        // Proxy media folder (Resolve: "Proxy generation location"): portable proxies that
        // other projects find automatically; default inside the cache folder.
        m_proxyFolder = new QLineEdit(prefs::proxyDirectoryOverride());
        m_proxyFolder->setObjectName(QStringLiteral("ProxyDirectory"));
        m_proxyFolder->setPlaceholderText(tr("<cache files location>/ProxyMedia"));
        auto* proxyBrowse = new QPushButton(tr("Browse"));
        proxyBrowse->setToolTip(tr("Choose the proxy media folder"));
        auto* proxyReset = new QPushButton(tr("Default"));
        proxyReset->setToolTip(tr("Keep proxies inside the cache folder"));
        auto* proxyRow = new QWidget;
        auto* proxyLayout = new QHBoxLayout(proxyRow);
        proxyLayout->setContentsMargins(0, 0, 0, 0);
        proxyLayout->setSpacing(metrics::kSpaceS);
        proxyLayout->addWidget(m_proxyFolder, 1);
        proxyLayout->addWidget(proxyBrowse);
        proxyLayout->addWidget(proxyReset);
        form->addRow(tr("Proxy generation location"), proxyRow);
        connect(proxyBrowse, &QPushButton::clicked, this, [this] {
            const QString dir = platform::getExistingDirectory(this, tr("Proxy Media Location"),
                QString(),
                m_proxyFolder->text().isEmpty() ? prefs::proxyDirectory() : m_proxyFolder->text());
            if (!dir.isEmpty())
                m_proxyFolder->setText(QDir::toNativeSeparators(dir));
        });
        connect(proxyReset, &QPushButton::clicked, m_proxyFolder, &QLineEdit::clear);

        m_cacheLimit = new QSpinBox;
        m_cacheLimit->setObjectName(QStringLiteral("RenderCacheLimit"));
        m_cacheLimit->setRange(1, 4096);
        m_cacheLimit->setSuffix(tr(" GB"));
        m_cacheLimit->setValue(prefs::renderCacheLimitGiB());
        form->addRow(tr("Render cache size limit"), m_cacheLimit);
        form->addRow(QString(),
            note(tr("Optimized media and the render cache live in the cache folder "
                    "(OptimizedMedia, RenderCache); the oldest cached frames are deleted "
                    "first when the limit is reached.")));
    }

    void apply() override
    {
        QSettings settings;
        settings.setValue(
            QLatin1String(prefs::kCacheDirectory), QDir::fromNativeSeparators(m_folder->text()));
        settings.setValue(QLatin1String(prefs::kProxyDirectory),
            QDir::fromNativeSeparators(m_proxyFolder->text()));
        settings.setValue(QLatin1String(prefs::kRenderCacheLimitGiB), m_cacheLimit->value());
    }

private:
    QLineEdit* m_folder = nullptr;
    QLineEdit* m_proxyFolder = nullptr;
    QSpinBox* m_cacheLimit = nullptr;
};

// ---- System: Decode Options ----------------------------------------------------------

/// Hardware acceleration choice with the backends that work on this machine in a table
/// (one row per API; never a wrapped paragraph).
class DecodePage final : public SettingsPage
{
public:
    explicit DecodePage(QWidget* parent)
        : SettingsPage(parent)
    {
        auto* form = makeForm(this);
        m_hwAccel = new QComboBox;
        m_hwAccel->setObjectName(QStringLiteral("HwAccel"));
        for (const QString& id : prefs::hwAccelIds())
            m_hwAccel->addItem(hwAccelLabel(prefs::hwAccelFromString(id)), id);
        m_hwAccel->setCurrentIndex(m_hwAccel->findData(prefs::hwAccelToString(prefs::hwAccel())));
        form->addRow(tr("Hardware decoding"), m_hwAccel);

        m_table = new QTreeWidget;
        m_table->setObjectName(QStringLiteral("HwInfo"));
        m_table->setRootIsDecorated(false);
        m_table->setSelectionMode(QAbstractItemView::NoSelection);
        m_table->setFocusPolicy(Qt::NoFocus);
        m_table->setHeaderLabels({tr("API"), tr("Status"), tr("Codecs")});
        m_table->header()->setStretchLastSection(true);
        m_table->setWordWrap(false);
        m_table->setTextElideMode(Qt::ElideRight);
        auto* checking = new QTreeWidgetItem(m_table, {QString(), tr("Checking…")});
        checking->setFirstColumnSpanned(true);
        form->addRow(tr("Available hardware"), m_table);

        // Probing opens GPU devices (up to ~1 s the first time): never on the GUI thread.
        auto* watcher = new QFutureWatcher<QList<media::HwDeviceInfo>>(this);
        connect(watcher, &QFutureWatcher<QList<media::HwDeviceInfo>>::finished, this,
            [this, watcher] { fill(watcher->result()); });
        watcher->setFuture(QtConcurrent::run([] { return media::hwCapabilities(); }));
        fitHeight();
    }

    void apply() override
    {
        prefs::setHwAccel(prefs::hwAccelFromString(m_hwAccel->currentData().toString()));
        applyHwAccelPreference();
    }

private:
    void fill(const QList<media::HwDeviceInfo>& devices)
    {
        m_table->clear();
        for (const media::HwDeviceInfo& info : devices) {
            QString status = info.available ? tr("Available") : tr("Not available");
            if (info.available && !info.device.isEmpty())
                status += QStringLiteral(" (%1)").arg(info.device);
            auto* item = new QTreeWidgetItem(
                m_table, {info.name, status, info.codecs.join(QStringLiteral(", "))});
            QString tip = info.available ? info.codecs.join(QStringLiteral(", ")) : info.error;
            for (int column = 0; column < m_table->columnCount(); ++column)
                item->setToolTip(column, tip);
            if (!info.available)
                item->setForeground(1, palette().brush(QPalette::PlaceholderText));
        }
        if (devices.isEmpty())
            new QTreeWidgetItem(m_table, {tr("None"), tr("No hardware decoders found")});
        m_table->resizeColumnToContents(0);
        m_table->resizeColumnToContents(1);
        fitHeight();
    }

    /// The table is as tall as its rows (3 APIs), so the section needs no inner scrolling.
    void fitHeight()
    {
        const int rows = std::max(1, m_table->topLevelItemCount());
        const int rowHeight = std::max(m_table->sizeHintForRow(0), fontMetrics().height() + 6);
        m_table->setFixedHeight(m_table->header()->sizeHint().height() + rows * rowHeight
            + 2 * m_table->frameWidth() + metrics::kSpaceXs);
    }

    QComboBox* m_hwAccel = nullptr;
    QTreeWidget* m_table = nullptr;
};

// ---- System: Audio I/O ---------------------------------------------------------------

class AudioIoPage final : public SettingsPage
{
public:
    explicit AudioIoPage(QWidget* parent)
        : SettingsPage(parent)
    {
        auto* form = makeForm(this);
        m_device = new QComboBox;
        m_device->setObjectName(QStringLiteral("AudioOutputDevice"));
        form->addRow(tr("Output device"), m_device);
        form->addRow(QString(),
            note(tr("Playback uses 48 kHz stereo. Automatic follows the system's output device "
                    "(for example headphones connected while playing). A chosen device is "
                    "used as soon as it is saved; while it is not connected, the system's "
                    "device plays instead.")));
        fillDevices(prefs::audioOutputDevice());
        // Devices connected or removed (and a new system default) while the dialog is open.
        connect(AudioDeviceMonitor::instance(), &AudioDeviceMonitor::outputsChanged, this,
            [this] { fillDevices(m_device->currentData().toString()); });
    }

    void apply() override
    {
        QSettings().setValue(QLatin1String(prefs::kAudioOutputDevice), m_device->currentData());
        // Every playback output switches now, also in the middle of a play.
        AudioDeviceMonitor::instance()->reloadPreference();
    }

private:
    /// Rebuilds the list from the connected outputs and selects @p selectedId ("" = Automatic).
    void fillDevices(const QString& selectedId)
    {
        const AudioDeviceMonitor* monitor = AudioDeviceMonitor::instance();
        const QSignalBlocker blocker(m_device);
        m_device->clear();
        const QAudioDevice systemDefault = monitor->systemDefault();
        m_device->addItem(systemDefault.isNull()
                ? tr("Automatic (no output device)")
                : tr("Automatic (%1)").arg(systemDefault.description()),
            QString());
        for (const QAudioDevice& device : monitor->outputs())
            m_device->addItem(device.description(), QString::fromUtf8(device.id()));
        int index = m_device->findData(selectedId);
        if (index < 0 && !selectedId.isEmpty()) {
            m_device->addItem(tr("%1 (not connected)").arg(selectedId), selectedId);
            index = m_device->count() - 1;
        }
        m_device->setCurrentIndex(std::max(0, index));
    }

    QComboBox* m_device = nullptr;
};

// ---- User: UI Settings ---------------------------------------------------------------

/// Interface scale. Colour theme and widget style are on the "appearance" page registered by
/// the application's theme (src/app/Theme) into the same section.
class InterfacePage final : public SettingsPage
{
public:
    explicit InterfacePage(QWidget* parent)
        : SettingsPage(parent)
    {
        auto* form = makeForm(this);
        m_scale = new QComboBox;
        m_scale->setObjectName(QStringLiteral("UiScale"));
        m_scale->addItem(tr("Automatic (system)"), QStringLiteral("auto"));
        for (const char* factor : {"1", "1.25", "1.5", "1.75", "2"}) {
            const QString value = QLatin1String(factor);
            m_scale->addItem(QStringLiteral("%1%").arg(qRound(value.toDouble() * 100)), value);
        }
        const QString scale
            = QSettings().value(QLatin1String(prefs::kUiScale), QStringLiteral("auto")).toString();
        m_scale->setCurrentIndex(std::max(0, m_scale->findData(scale)));
        m_scale->setToolTip(tr("Takes effect after restarting the application."));
        form->addRow(tr("Interface scale"), m_scale);

        // v2.2: small tweens of timeline edits (src/ui/common/UiAnimation.h).
        m_animate = new QCheckBox(tr("Animate timeline edits"));
        m_animate->setObjectName(QStringLiteral("AnimateTimelineEdits"));
        m_animate->setChecked(prefs::animationsEnabled());
        form->addRow(tr("Animations"), m_animate);
        m_speed = new QSlider(Qt::Horizontal);
        m_speed->setObjectName(QStringLiteral("AnimationSpeed"));
        // Steps of 0.25 from 0.5x (slower) to 2x (faster).
        m_speed->setRange(kMinSpeedStep, kMaxSpeedStep);
        m_speed->setPageStep(1);
        m_speed->setTickPosition(QSlider::TicksBelow);
        m_speed->setValue(int(std::lround(prefs::animationSpeed() / kSpeedStep)));
        m_speed->setEnabled(m_animate->isChecked());
        auto* speedLabel = new QLabel;
        auto showSpeed = [this, speedLabel] {
            speedLabel->setText(tr("%1x").arg(m_speed->value() * kSpeedStep));
        };
        showSpeed();
        connect(m_speed, &QSlider::valueChanged, this, showSpeed);
        connect(m_animate, &QCheckBox::toggled, m_speed, &QWidget::setEnabled);
        auto* speedRow = new QHBoxLayout;
        speedRow->addWidget(m_speed, 1);
        speedRow->addWidget(speedLabel);
        form->addRow(tr("Animation speed"), speedRow);
        form->addRow(QString(),
            note(tr("Clips slide, appear and fade after edits, cuts flash and the playhead "
                    "glides on jumps. The timeline itself changes at once. The desktop's "
                    "animation speed applies as well.")));

        // Our own (Resolve has none): HandBrake-style "Render complete" (spec 45.5).
        m_renderNotify = new QCheckBox(tr("Show a system notification when renders finish"));
        m_renderNotify->setObjectName(QStringLiteral("RenderNotifications"));
        m_renderNotify->setChecked(prefs::renderNotificationsEnabled());
        form->addRow(tr("Notifications"), m_renderNotify);
    }

    void apply() override
    {
        QSettings().setValue(QLatin1String(prefs::kUiScale), m_scale->currentData().toString());
        prefs::setAnimationsEnabled(m_animate->isChecked());
        prefs::setAnimationSpeed(m_speed->value() * kSpeedStep);
        prefs::setRenderNotificationsEnabled(m_renderNotify->isChecked());
        uianim::reload(); // live: every timeline view follows at once
    }

private:
    static constexpr double kSpeedStep = 0.25;
    static constexpr int kMinSpeedStep = 2; ///< 0.5x
    static constexpr int kMaxSpeedStep = 8; ///< 2x
    QComboBox* m_scale = nullptr;
    QCheckBox* m_animate = nullptr;
    QSlider* m_speed = nullptr;
    QCheckBox* m_renderNotify = nullptr;
};

// ---- User: Project Save and Load -----------------------------------------------------

class ProjectSavePage final : public SettingsPage
{
public:
    explicit ProjectSavePage(QWidget* parent)
        : SettingsPage(parent)
    {
        auto* form = makeForm(this);
        m_autosave = new QSpinBox;
        m_autosave->setObjectName(QStringLiteral("AutosaveMinutes"));
        m_autosave->setRange(0, kMaxAutosaveMinutes);
        m_autosave->setSuffix(tr(" min"));
        m_autosave->setSpecialValueText(tr("Off"));
        m_autosave->setValue(prefs::autosaveMinutes());
        form->addRow(tr("Autosave every"), m_autosave);
        const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/autosave");
        auto* location = note(QDir::toNativeSeparators(folder));
        location->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(tr("Autosave location"), location);

        // Project library (the Project Manager's folder) and what start-up shows.
        m_library = new QLineEdit(prefs::projectLibraryDirectoryOverride());
        m_library->setObjectName(QStringLiteral("ProjectLibraryDirectory"));
        m_library->setPlaceholderText(
            QDir::toNativeSeparators(prefs::projectLibraryDefaultDirectory()));
        auto* browse = new QPushButton(tr("Browse"));
        browse->setToolTip(tr("Choose the project library folder"));
        auto* reset = new QPushButton(tr("Default"));
        reset->setToolTip(tr("Use the default project library folder"));
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(metrics::kSpaceS);
        layout->addWidget(m_library, 1);
        layout->addWidget(browse);
        layout->addWidget(reset);
        form->addRow(tr("Project library location"), row);
        form->addRow(QString(),
            note(tr("The Project Manager lists the projects in this folder and its sub-folders. "
                    "Projects saved elsewhere appear under Recent Projects.")));
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString dir
                = platform::getExistingDirectory(this, tr("Project Library Location"), QString(),
                    m_library->text().isEmpty() ? prefs::projectLibraryDefaultDirectory()
                                                : m_library->text());
            if (!dir.isEmpty())
                m_library->setText(QDir::toNativeSeparators(dir));
        });
        connect(reset, &QPushButton::clicked, m_library, &QLineEdit::clear);
        m_startup = new QComboBox;
        m_startup->setObjectName(QStringLiteral("StartupAction"));
        m_startup->addItem(tr("Show the Project Manager"), true);
        m_startup->addItem(tr("Reopen the last project"), false);
        m_startup->setCurrentIndex(prefs::startupShowsProjectManager() ? 0 : 1);
        form->addRow(tr("At start-up"), m_startup);
    }

    void apply() override
    {
        prefs::setAutosaveMinutes(m_autosave->value());
        prefs::setProjectLibraryDirectory(QDir::fromNativeSeparators(m_library->text().trimmed()));
        prefs::setStartupShowsProjectManager(m_startup->currentData().toBool());
    }

private:
    QSpinBox* m_autosave = nullptr;
    QLineEdit* m_library = nullptr;
    QComboBox* m_startup = nullptr;
};

// ---- User: Editing -------------------------------------------------------------------

/// The keys of core/EditBehaviour (group "edit/") plus the timeline zoom anchor.
class EditingPage final : public SettingsPage
{
public:
    explicit EditingPage(QWidget* parent)
        : SettingsPage(parent)
    {
        const EditBehaviour b = EditBehaviour::fromSettings();
        auto* form = makeForm(this);
        m_still = secondsSpin(b.stillSeconds);
        m_still->setObjectName(QStringLiteral("StillSeconds"));
        form->addRow(tr("Standard still duration"), m_still);
        m_generator = secondsSpin(b.generatorSeconds);
        m_generator->setObjectName(QStringLiteral("GeneratorSeconds"));
        form->addRow(tr("Standard title and generator duration"), m_generator);
        m_transition = secondsSpin(b.transitionSeconds);
        m_transition->setObjectName(QStringLiteral("TransitionSeconds"));
        form->addRow(tr("Standard transition duration"), m_transition);
        m_nudge = new QSpinBox;
        m_nudge->setObjectName(QStringLiteral("MultiFrameNudge"));
        m_nudge->setRange(1, kMaxNudgeFrames);
        m_nudge->setSuffix(tr(" frames"));
        m_nudge->setValue(b.multiFrameNudge);
        form->addRow(tr("Fast nudge"), m_nudge);

        m_zoom = new QComboBox;
        m_zoom->setObjectName(QStringLiteral("ZoomAnchor"));
        m_zoom->addItem(tr("Playhead"), QStringLiteral("playhead"));
        m_zoom->addItem(tr("Mouse pointer"), QStringLiteral("mouse"));
        m_zoom->setCurrentIndex(prefs::zoomAroundMouse() ? 1 : 0);
        m_zoom->setToolTip(tr("Point the timeline keeps in place when zooming with the "
                              "mouse wheel"));
        form->addRow(tr("Timeline zooms around"), m_zoom);
        m_wheel = new QComboBox;
        m_wheel->setObjectName(QStringLiteral("WheelScheme"));
        m_wheel->addItem(tr("Resolve (Alt zooms, Ctrl scrolls time, Shift track height)"),
            int(WheelScheme::Resolve));
        m_wheel->addItem(tr("Classic (Ctrl zooms, Shift scrolls time)"), int(WheelScheme::Classic));
        m_wheel->setCurrentIndex(m_wheel->findData(int(b.wheelScheme)));
        form->addRow(tr("Mouse wheel"), m_wheel);

        m_threePoint = new QCheckBox(tr("Use timeline in and out points for source edits"));
        m_threePoint->setChecked(b.threePointEditing);
        form->addRow(tr("Three-point editing"), m_threePoint);
        m_rippleGap = new QCheckBox(tr("Close the gap under the playhead when nothing is "
                                       "selected"));
        m_rippleGap->setChecked(b.rippleDeleteGapAtPlayhead);
        form->addRow(tr("Ripple delete"), m_rippleGap);
        // Off by default, keeping spec 10.5 (see 49.6 for what is known of Resolve).
        m_rippleClip = new QCheckBox(tr("Delete the clips under the playhead on the selected "
                                        "tracks when nothing is selected"));
        m_rippleClip->setObjectName(QStringLiteral("RippleDeleteClipAtPlayhead"));
        m_rippleClip->setChecked(b.rippleDeleteClipAtPlayhead);
        form->addRow(QString(), m_rippleClip);
        m_rollAtCut = new QCheckBox(tr("Selection tool rolls at the middle of an edit"));
        m_rollAtCut->setChecked(b.selectToolRollsAtCut);
        form->addRow(tr("Trimming"), m_rollAtCut);
        m_slideInName = new QCheckBox(tr("Trim tool slides in the clip name bar"));
        m_slideInName->setChecked(b.trimToolSlideInNameBar);
        form->addRow(QString(), m_slideInName);
        m_twoUpLook = new QCheckBox(tr("Trim view shows effects and transforms (slower)"));
        m_twoUpLook->setObjectName(QStringLiteral("TwoUpProgramLook"));
        m_twoUpLook->setChecked(b.twoUpProgramLook);
        form->addRow(QString(), m_twoUpLook);
        m_tabs = new QCheckBox(tr("Show open timelines as tabs"));
        m_tabs->setObjectName(QStringLiteral("TimelineTabs"));
        m_tabs->setToolTip(tr("Off: one timeline at a time, chosen from a drop-down above "
                              "the timeline"));
        m_tabs->setChecked(b.timelineTabs);
        form->addRow(tr("Timelines"), m_tabs);
        m_smartBins = new QCheckBox(tr("Show the default smart bins (Video, Audio, Stills, "
                                       "Timelines)"));
        m_smartBins->setObjectName(QStringLiteral("DefaultSmartBins"));
        m_smartBins->setChecked(prefs::showDefaultSmartBins());
        form->addRow(tr("Media pool"), m_smartBins);
        m_importTarget = new QComboBox;
        m_importTarget->setObjectName(QStringLiteral("ImportTarget"));
        m_importTarget->addItem(tr("Current bin"), false);
        m_importTarget->addItem(tr("Master bin"), true);
        m_importTarget->setToolTip(tr("Where File > Import, the Media page's Add to Media Pool, "
                                      "drops onto the media pool and timeline drops put new "
                                      "media. A drop onto a bin always goes into that bin."));
        m_importTarget->setCurrentIndex(prefs::importIntoMasterBin() ? 1 : 0);
        form->addRow(tr("Import new media into"), m_importTarget);
        m_rulerJumps = new QCheckBox(tr("Pressing the ruler moves the playhead there at once"));
        m_rulerJumps->setObjectName(QStringLiteral("RulerPressJumpsPlayhead"));
        m_rulerJumps->setToolTip(tr("On (Resolve): the playhead jumps to a press anywhere in "
                                    "the ruler and follows the drag. Off: only a click "
                                    "without moving the mouse jumps it; drag the playhead's "
                                    "head to scrub."));
        m_rulerJumps->setChecked(b.rulerPressJumpsPlayhead);
        form->addRow(tr("Timeline ruler"), m_rulerJumps);
    }

    void apply() override
    {
        QSettings s;
        s.setValue(QStringLiteral("edit/stillSeconds"), m_still->value());
        s.setValue(QStringLiteral("edit/generatorSeconds"), m_generator->value());
        s.setValue(QStringLiteral("edit/transitionSeconds"), m_transition->value());
        s.setValue(QStringLiteral("edit/multiFrameNudge"), m_nudge->value());
        s.setValue(QStringLiteral("edit/threePointEditing"), m_threePoint->isChecked());
        s.setValue(QStringLiteral("edit/rippleDeleteGapAtPlayhead"), m_rippleGap->isChecked());
        s.setValue(QStringLiteral("edit/rippleDeleteClipAtPlayhead"), m_rippleClip->isChecked());
        s.setValue(QStringLiteral("edit/selectToolRollsAtCut"), m_rollAtCut->isChecked());
        s.setValue(QStringLiteral("edit/trimToolSlideInNameBar"), m_slideInName->isChecked());
        s.setValue(QStringLiteral("edit/twoUpProgramLook"), m_twoUpLook->isChecked());
        s.setValue(QStringLiteral("edit/timelineTabs"), m_tabs->isChecked());
        s.setValue(QLatin1String(prefs::kZoomAnchor), m_zoom->currentData());
        prefs::setShowDefaultSmartBins(m_smartBins->isChecked());
        prefs::setImportIntoMasterBin(m_importTarget->currentData().toBool());
        EditBehaviour::setWheelScheme(WheelScheme(m_wheel->currentData().toInt()));
        s.setValue(QStringLiteral("edit/rulerPressJumpsPlayhead"), m_rulerJumps->isChecked());
    }

private:
    QDoubleSpinBox* m_still = nullptr;
    QDoubleSpinBox* m_generator = nullptr;
    QDoubleSpinBox* m_transition = nullptr;
    QSpinBox* m_nudge = nullptr;
    QComboBox* m_wheel = nullptr;
    QComboBox* m_zoom = nullptr;
    QCheckBox* m_threePoint = nullptr;
    QCheckBox* m_twoUpLook = nullptr;
    QCheckBox* m_rippleGap = nullptr;
    QCheckBox* m_rippleClip = nullptr;
    QCheckBox* m_rollAtCut = nullptr;
    QCheckBox* m_slideInName = nullptr;
    QCheckBox* m_tabs = nullptr;
    QCheckBox* m_smartBins = nullptr;
    QComboBox* m_importTarget = nullptr;
    QCheckBox* m_rulerJumps = nullptr;
};

// ---- User: Playback Settings ---------------------------------------------------------

/// Preview quality and the PlaybackBehaviour keys ("playback/...").
class PlaybackPage final : public SettingsPage
{
public:
    explicit PlaybackPage(QWidget* parent)
        : SettingsPage(parent)
    {
        const PlaybackBehaviour b = PlaybackBehaviour::fromSettings();
        auto* form = makeForm(this);
        m_quality = new QComboBox;
        m_quality->setObjectName(QStringLiteral("PreviewQuality"));
        m_quality->addItem(
            tr("Full (skip frames rather than lose sharpness)"), QStringLiteral("full"));
        m_quality->addItem(tr("Half"), QStringLiteral("half"));
        m_quality->addItem(tr("Quarter"), QStringLiteral("quarter"));
        m_quality->addItem(
            tr("Automatic (Half while playback cannot keep up)"), QStringLiteral("auto"));
        m_quality->setCurrentIndex(std::max(0, m_quality->findData(prefs::previewQuality())));
        m_quality->setToolTip(PlaybackResolution::toolTip());
        form->addRow(tr("Playback resolution"), m_quality);
        m_preroll = new QDoubleSpinBox;
        m_postroll = new QDoubleSpinBox;
        for (QDoubleSpinBox* spin : {m_preroll, m_postroll}) {
            spin->setRange(0.0, double(kMaxRollMs) / kMsPerSecond);
            spin->setDecimals(1);
            spin->setSingleStep(0.5);
            spin->setSuffix(tr(" s"));
        }
        m_preroll->setObjectName(QStringLiteral("PrerollSeconds"));
        m_postroll->setObjectName(QStringLiteral("PostrollSeconds"));
        m_preroll->setValue(double(b.prerollMs) / kMsPerSecond);
        m_postroll->setValue(double(b.postrollMs) / kMsPerSecond);
        form->addRow(tr("Pre-roll"), m_preroll);
        form->addRow(tr("Post-roll"), m_postroll);
        m_return = new QCheckBox(tr("Return to the start position after playing around"));
        m_return->setChecked(b.returnAfterPlayAround);
        form->addRow(tr("Play around"), m_return);
        m_restart = new QCheckBox(tr("Play from the start when the playhead is at the end"));
        m_restart->setChecked(b.restartAtEnd);
        form->addRow(tr("At the end"), m_restart);
        m_stopOnMove = new QCheckBox(tr("Stop playback when the playhead is moved"));
        m_stopOnMove->setObjectName(QStringLiteral("StopOnPlayheadMove"));
        m_stopOnMove->setToolTip(tr("On: clicking or dragging in the ruler or the scrub bar, "
                                    "or jumping to an edit or a marker while playing pauses "
                                    "there (Premiere). Off: playback carries on from the new "
                                    "position (Resolve)."));
        m_stopOnMove->setChecked(b.stopOnPlayheadMove);
        form->addRow(tr("Moving the playhead"), m_stopOnMove);
        m_afterEdit = new QComboBox;
        m_afterEdit->setObjectName(QStringLiteral("AfterEditDuringPlayback"));
        m_afterEdit->addItem(tr("Keep playing"), false);
        m_afterEdit->addItem(tr("Stop playback"), true);
        m_afterEdit->setToolTip(tr("What playback does after an edit of the timeline while it "
                                   "plays (ripple delete, split, trim, move, insert, undo...). "
                                   "Changing values in the Inspector never stops it."));
        m_afterEdit->setCurrentIndex(b.stopOnEdit ? 1 : 0);
        form->addRow(tr("After an edit during playback"), m_afterEdit);
    }

    void apply() override
    {
        QSettings s;
        prefs::setPreviewQuality(m_quality->currentData().toString());
        s.setValue(QStringLiteral("playback/prerollMs"), qRound(m_preroll->value() * kMsPerSecond));
        s.setValue(
            QStringLiteral("playback/postrollMs"), qRound(m_postroll->value() * kMsPerSecond));
        s.setValue(QStringLiteral("playback/returnAfterPlayAround"), m_return->isChecked());
        s.setValue(QStringLiteral("playback/restartAtEnd"), m_restart->isChecked());
        s.setValue(QStringLiteral("playback/stopOnPlayheadMove"), m_stopOnMove->isChecked());
        s.setValue(QStringLiteral("playback/stopOnEdit"), m_afterEdit->currentData().toBool());
    }

private:
    QComboBox* m_quality = nullptr;
    QDoubleSpinBox* m_preroll = nullptr;
    QDoubleSpinBox* m_postroll = nullptr;
    QCheckBox* m_return = nullptr;
    QCheckBox* m_restart = nullptr;
    QCheckBox* m_stopOnMove = nullptr;
    QComboBox* m_afterEdit = nullptr;
};

// ---- User: Keyboard ------------------------------------------------------------------

/// Entry point to the keyboard customization dialog (its own window, as in Resolve).
class KeyboardPage final : public SettingsPage
{
public:
    KeyboardPage(QAction* customize, QWidget* parent)
        : SettingsPage(parent)
    {
        auto* form = makeForm(this);
        auto* button = new QPushButton(tr("Keyboard Customization…"));
        button->setObjectName(QStringLiteral("OpenKeyboardCustomization"));
        button->setEnabled(customize != nullptr);
        if (customize) {
            QPointer<QAction> action(customize);
            // The key named in the tooltip follows the action (it can be rebound in the
            // editor this button opens).
            auto updateTip = [button, action] {
                const QList<QKeySequence> keys
                    = action ? action->shortcuts() : QList<QKeySequence> {};
                button->setToolTip(keys.isEmpty()
                        ? tr("Opens the keyboard shortcut editor")
                        : tr("Opens the keyboard shortcut editor (%1)")
                              .arg(keys.first().toString(QKeySequence::NativeText)));
            };
            updateTip();
            connect(customize, &QAction::changed, button, updateTip);
            connect(button, &QPushButton::clicked, this, [action] {
                if (action)
                    action->trigger();
            });
        }
        auto* row = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(button);
        layout->addStretch(1);
        form->addRow(tr("Shortcuts"), row);
        form->addRow(QString(),
            note(tr("Search commands, record new keys, resolve conflicts and import or export "
                    "keyboard layouts.")));

        // Off by default: a panel toggle leaves the focus where it was (spec 49.5).
        m_focusSearch
            = new QCheckBox(tr("Focus the search field when a panel is opened from the keyboard"));
        m_focusSearch->setObjectName(QStringLiteral("FocusSearchOnPanelKey"));
        m_focusSearch->setToolTip(tr("Media Pool, Effects and Edit Index: the shortcut that "
                                     "opens the panel puts the cursor into its search field"));
        m_focusSearch->setChecked(prefs::focusSearchOnPanelKey());
        form->addRow(tr("Panels"), m_focusSearch);
    }

    void apply() override { prefs::setFocusSearchOnPanelKey(m_focusSearch->isChecked()); }

private:
    QCheckBox* m_focusSearch = nullptr;
};

} // namespace

void registerStandardSettingsPages(SettingsPageRegistry& registry, ActionRegistry* actions)
{
    const QString system = QStringLiteral("system");
    const QString user = QStringLiteral("user");
    auto add = [&registry](const char* id, const QString& title, const char* icon, int order,
                   const QString& group, std::function<SettingsPage*(QWidget*)> create) {
        SettingsPageInfo info {QLatin1String(id), title, QLatin1String(icon), order,
            std::move(create), group, QString()};
        registry.add(std::move(info));
    };
    // System (machine) sections, Resolve's order.
    add("memory", tr("Memory and GPU"), "cpu", 10, system,
        [](QWidget* parent) { return new MemoryPage(parent); });
    add("storage", tr("Media Storage"), "drive-harddisk", 20, system,
        [](QWidget* parent) { return new StoragePage(parent); });
    add("decode", tr("Decode Options"), "media-playback-start", 30, system,
        [](QWidget* parent) { return new DecodePage(parent); });
    add("audio_io", tr("Audio I/O"), "audio-card", 40, system,
        [](QWidget* parent) { return new AudioIoPage(parent); });
    add("control", tr("Local Control"), "network-disconnect", 50, system,
        [](QWidget* parent) { return new ControlPage(parent); });
    // User sections. "ui" also receives the theme's "appearance" page.
    add("ui", tr("UI Settings"), "preferences-desktop-display", 100, user,
        [](QWidget* parent) { return new InterfacePage(parent); });
    add("project", tr("Project Save and Load"), "document-save", 120, user,
        [](QWidget* parent) { return new ProjectSavePage(parent); });
    add("editing", tr("Editing"), "edit-cut", 130, user,
        [](QWidget* parent) { return new EditingPage(parent); });
    add("playback", tr("Playback Settings"), "media-playback-start", 140, user,
        [](QWidget* parent) { return new PlaybackPage(parent); });
    QAction* keyboard = actions && actions->contains(QStringLiteral("edit.keyboard_customization"))
        ? actions->action(QStringLiteral("edit.keyboard_customization"))
        : nullptr;
    add("keyboard", tr("Keyboard"), "input-keyboard", 150, user,
        [keyboard](QWidget* parent) { return new KeyboardPage(keyboard, parent); });
}

void applyHwAccelPreference()
{
    media::setPreferredHwAccel(toMediaMode(prefs::hwAccel()));
}

void applyStartupPreferences()
{
    // The compositor reads KINOKLIP_RENDER when it is created; an explicit environment
    // variable (debugging) still wins over the stored choice.
    if (prefs::forceCpuRendering() && !qEnvironmentVariableIsSet("KINOKLIP_RENDER"))
        qputenv("KINOKLIP_RENDER", "cpu");
}

void applyRuntimePreferences(PlaybackEngine* playback)
{
    applyHwAccelPreference();
    uianim::reload();
    media::ImageLoader::setCacheLimitBytes(int64_t(prefs::stillCacheMiB()) * kBytesPerMiB);
    const QString cacheDir = prefs::cacheDirectory();
    media::MediaCache::instance().setCacheDirectory(
        cacheDir.isEmpty() ? defaultCacheDirectory() : cacheDir);
    if (playback)
        playback->setBehaviour(PlaybackBehaviour::fromSettings());
    // Playback resolution: the shared setting updates every viewer, its engine and the menu.
    PlaybackResolution::instance().load();
    if (playback)
        playback->setQualityMode(PlaybackResolution::instance().mode());
}

} // namespace kk
