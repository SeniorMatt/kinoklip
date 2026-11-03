// SPDX-License-Identifier: GPL-3.0-or-later
// AudioDeviceMonitor — the one place that decides which sound device playback uses, and tells
// every output when that changes (device connected / removed, system default changed,
// Preferences changed).
#pragma once

#include "playback/AudioRouting.h"

#include <QAudioDevice>
#include <QList>
#include <QObject>

class QMediaDevices;

namespace kk {

/// Process-wide watcher of the sound outputs (one per application, created on first use).
///
/// It listens to QMediaDevices::audioOutputsChanged (Qt reports device additions, removals
/// and changes of the system default through it) and to Preferences > System > Audio I/O
/// (reloadPreference()), resolves the device with chooseAudioOutput() and emits
/// currentChanged() when the result differs. Every QAudioSink-based AudioOutput follows it:
/// an idle one uses the new device on its next start, a playing one moves its stream at once.
/// Each change is logged in kk.playback with the old device, the new one and the reason.
///
/// Why this is needed: Qt opens a sink on a *named* device (the PulseAudio sink name, or the
/// PipeWire node serial), also for QMediaDevices::defaultAudioOutput(). Such a stream is pinned
/// to that device and does not follow the desktop's default (e.g. headphones connected later).
///
/// Thread-safety: GUI thread only (QMediaDevices must be used on the thread of the
/// application object).
class AudioDeviceMonitor final : public QObject
{
    Q_OBJECT
public:
    /// The application's monitor (parented to the QCoreApplication, so it is destroyed with
    /// it). Created on first use.
    static AudioDeviceMonitor* instance();

    /// The device playback should use now (null if there is no output at all).
    [[nodiscard]] QAudioDevice current() const { return m_current; }
    /// Why current() was chosen.
    [[nodiscard]] AudioRouteChoice::Reason reason() const { return m_reason; }
    /// The system default output (what "Automatic" resolves to).
    [[nodiscard]] QAudioDevice systemDefault() const;
    /// The connected outputs.
    [[nodiscard]] QList<QAudioDevice> outputs() const;

    /// Re-reads prefs::audioOutputDevice() and switches if the result differs. Called by the
    /// Preferences page after it saved the setting.
    void reloadPreference();

Q_SIGNALS:
    /// The device list or the system default changed (also when current() stays the same).
    void outputsChanged();
    /// All outputs should use @p device from now on (null: no device left).
    void currentChanged(const QAudioDevice& device);

private:
    explicit AudioDeviceMonitor(QObject* parent);
    enum class Trigger {
        Startup,
        DevicesChanged,
        PreferenceChanged
    };
    void refresh(Trigger trigger);

    QMediaDevices* m_devices = nullptr;
    QAudioDevice m_current;
    AudioRouteChoice::Reason m_reason = AudioRouteChoice::Reason::None;
    QByteArray m_preferred;
};

/// "Bluetooth Headphones" or "none", for logs.
[[nodiscard]] QString audioDeviceLabel(const QAudioDevice& device);

} // namespace kk
