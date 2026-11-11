// SPDX-License-Identifier: GPL-3.0-or-later
// AudioInput — a source of microphone samples behind a small interface (voice-over, spec 75).
// The application uses QtAudioInput (QAudioSource); tests feed a generated signal instead.
#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

namespace kk {

/// A system audio input (microphone, line in, ...).
struct AudioInputDevice
{
    QByteArray id; ///< QAudioDevice::id()
    QString description; ///< name shown in menus
    bool isDefault = false;
    friend bool operator==(const AudioInputDevice&, const AudioInputDevice&) = default;
};

/// The system's audio inputs, the default one first (QMediaDevices::audioInputs()).
[[nodiscard]] QList<AudioInputDevice> systemAudioInputs();

/// Why no recording can start from the system's inputs: no input device, or the operating
/// system denied the microphone permission. Empty when recording looks possible.
[[nodiscard]] QString systemAudioInputProblem();

/// A running or startable stream of interleaved float samples.
///
/// start() opens the device; samples() then arrives on the GUI thread, in blocks, until stop().
/// A failure while running (device unplugged, permission revoked) is reported by failed() and
/// ends the stream.
///
/// Thread-safety: GUI thread only.
class AudioInput : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    /// Opens the device and starts delivering samples. False with error() set on failure.
    virtual bool start() = 0;
    /// Stops delivering samples and releases the device. Safe to call twice.
    virtual void stop() = 0;
    /// Samples per second per channel (valid after construction).
    [[nodiscard]] virtual int sampleRate() const = 0;
    /// Interleaved channels (valid after construction).
    [[nodiscard]] virtual int channels() const = 0;
    /// Delay between a sound reaching the microphone and its samples reaching samples(), in
    /// microseconds, as far as the device reports it (0 = unknown).
    [[nodiscard]] virtual qint64 latencyUs() const = 0;
    [[nodiscard]] QString error() const { return m_error; }

Q_SIGNALS:
    /// A block of interleaved samples, -1..1.
    void samples(const std::vector<float>& block);
    /// The stream broke; error() says why.
    void failed(const QString& message);

protected:
    QString m_error;
};

/// Creates the input for the device with id @p deviceId ("" = the system default). nullptr
/// and @p error set if there is no such device. Tests replace the factory they hand to
/// VoiceOverController.
using AudioInputFactory
    = std::function<std::unique_ptr<AudioInput>(const QByteArray& deviceId, QString* error)>;

/// The factory for real hardware: QAudioSource on the chosen (or default) input device.
[[nodiscard]] AudioInputFactory qtAudioInputFactory();

} // namespace kk
