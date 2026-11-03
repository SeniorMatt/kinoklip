// SPDX-License-Identifier: GPL-3.0-or-later
// AudioDeviceMonitor — resolves the playback device and follows device / preference changes.
#include "playback/AudioDeviceMonitor.h"

#include "core/Preferences.h"
#include "playback/PlaybackLog.h"

#include <QCoreApplication>
#include <QMediaDevices>
#include <QPointer>

#include <algorithm>

namespace kk {

QString audioDeviceLabel(const QAudioDevice& device)
{
    return device.isNull() ? QStringLiteral("none") : device.description();
}

AudioDeviceMonitor* AudioDeviceMonitor::instance()
{
    static QPointer<AudioDeviceMonitor> s_instance;
    if (!s_instance) {
        // Without an application object (some command line paths) the monitor is never
        // destroyed; it holds no resources besides the QMediaDevices connection.
        s_instance = new AudioDeviceMonitor(QCoreApplication::instance());
    }
    return s_instance;
}

AudioDeviceMonitor::AudioDeviceMonitor(QObject* parent)
    : QObject(parent)
    , m_devices(new QMediaDevices(this))
{
    m_preferred = prefs::audioOutputDevice().toUtf8();
    // Qt emits audioOutputsChanged for additions, removals and a new system default alike.
    connect(m_devices, &QMediaDevices::audioOutputsChanged, this,
        [this] { refresh(Trigger::DevicesChanged); });
    refresh(Trigger::Startup);
}

QAudioDevice AudioDeviceMonitor::systemDefault() const
{
    return QMediaDevices::defaultAudioOutput();
}

QList<QAudioDevice> AudioDeviceMonitor::outputs() const
{
    return QMediaDevices::audioOutputs();
}

void AudioDeviceMonitor::reloadPreference()
{
    const QByteArray preferred = prefs::audioOutputDevice().toUtf8();
    if (preferred == m_preferred) {
        return;
    }
    m_preferred = preferred;
    refresh(Trigger::PreferenceChanged);
}

void AudioDeviceMonitor::refresh(Trigger trigger)
{
    using Reason = AudioRouteChoice::Reason;
    const QList<QAudioDevice> devices = QMediaDevices::audioOutputs();
    const QAudioDevice systemDefault = QMediaDevices::defaultAudioOutput();
    std::vector<AudioDeviceInfo> available;
    available.reserve(size_t(devices.size()));
    for (const QAudioDevice& device : devices) {
        available.push_back({device.id(), device.description()});
    }
    const AudioRouteChoice choice = chooseAudioOutput(m_preferred, available, systemDefault.id());

    QAudioDevice next;
    for (const QAudioDevice& device : devices) {
        if (device.id() == choice.id) {
            next = device;
        }
    }
    if (next.isNull() && !choice.id.isEmpty() && systemDefault.id() == choice.id) {
        next = systemDefault;
    }

    const Reason previousReason = m_reason;
    m_reason = choice.reason;
    if (trigger != Trigger::Startup) {
        Q_EMIT outputsChanged();
    }
    if (trigger != Trigger::Startup && next.id() == m_current.id()
        && next.isNull() == m_current.isNull()) {
        return;
    }

    // Why: the most specific explanation of the new state.
    const bool previousGone = !m_current.isNull()
        && std::none_of(available.begin(), available.end(),
            [this](const AudioDeviceInfo& info) { return info.id == m_current.id(); });
    QString why;
    switch (choice.reason) {
    case Reason::None:
        why = QStringLiteral("no output device is connected");
        break;
    case Reason::Chosen:
        why = trigger == Trigger::PreferenceChanged ? QStringLiteral("chosen in Preferences")
            : previousReason == Reason::ChosenMissing
            ? QStringLiteral("the chosen device is connected again")
            : QStringLiteral("the chosen device");
        break;
    case Reason::ChosenMissing:
        why = QStringLiteral("the chosen device %1 is not connected, using the system default")
                  .arg(QString::fromUtf8(m_preferred));
        break;
    case Reason::SystemDefault:
        if (trigger == Trigger::PreferenceChanged) {
            why = QStringLiteral("Automatic chosen in Preferences, using the system default");
        } else if (previousGone) {
            why = QStringLiteral("%1 was disconnected, using the system default")
                      .arg(audioDeviceLabel(m_current));
        } else if (trigger == Trigger::DevicesChanged) {
            why = QStringLiteral("the system default output changed");
        } else {
            why = QStringLiteral("the system default output");
        }
        break;
    }
    if (trigger == Trigger::Startup) {
        qCInfo(lcPlayback).noquote()
            << QStringLiteral("audio device: %1 (%2)").arg(audioDeviceLabel(next), why);
    } else {
        qCInfo(lcPlayback).noquote()
            << QStringLiteral("audio device switch: %1 -> %2 (%3)")
                   .arg(audioDeviceLabel(m_current), audioDeviceLabel(next), why);
    }
    m_current = next;
    if (trigger != Trigger::Startup) {
        Q_EMIT currentChanged(m_current);
    }
}

} // namespace kk
