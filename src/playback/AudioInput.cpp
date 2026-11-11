// SPDX-License-Identifier: GPL-3.0-or-later
#include "playback/AudioInput.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QCoreApplication>
#include <QIODevice>
#include <QMediaDevices>
#include <QtGlobal>

#include <algorithm>
#include <cstring>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QPermissions>
#endif

namespace kk {

QList<AudioInputDevice> systemAudioInputs()
{
    QList<AudioInputDevice> out;
    const QAudioDevice def = QMediaDevices::defaultAudioInput();
    if (!def.isNull())
        out.append({def.id(), def.description(), true});
    for (const QAudioDevice& d : QMediaDevices::audioInputs()) {
        if (d.id() != def.id())
            out.append({d.id(), d.description(), false});
    }
    return out;
}

QString systemAudioInputProblem()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QCoreApplication::instance()
        && QCoreApplication::instance()->checkPermission(QMicrophonePermission {})
            == Qt::PermissionStatus::Denied) {
        return QCoreApplication::translate("kk::AudioInput",
            "The operating system does not let Kinoklip use the microphone. Allow it in the "
            "system's privacy settings.");
    }
#endif
    if (systemAudioInputs().isEmpty()) {
        return QCoreApplication::translate("kk::AudioInput",
            "No microphone or other audio input was found. Connect one and try again.");
    }
    return {};
}

namespace {

/// Microphone through QAudioSource, converted to interleaved float.
class QtAudioInput final : public AudioInput
{
public:
    explicit QtAudioInput(const QAudioDevice& device)
        : m_device(device)
    {
        m_format = device.preferredFormat();
        if (device.minimumSampleRate() <= 48000 && device.maximumSampleRate() >= 48000)
            m_format.setSampleRate(48000);
        if (m_format.channelCount() < 1)
            m_format.setChannelCount(1);
        if (!device.isFormatSupported(m_format))
            m_format = device.preferredFormat();
    }

    ~QtAudioInput() override { stop(); }

    bool start() override
    {
        if (m_source)
            return true;
        m_source = new QAudioSource(m_device, m_format, this);
        m_io = m_source->start();
        if (!m_io || m_source->error() != QAudio::NoError) {
            m_error = QCoreApplication::translate(
                "kk::AudioInput", "The microphone could not be opened (%1).")
                          .arg(m_device.description());
            stop();
            return false;
        }
        connect(m_io, &QIODevice::readyRead, this, &QtAudioInput::drain);
        connect(m_source, &QAudioSource::stateChanged, this, [this](QAudio::State state) {
            if (state == QAudio::StoppedState && m_source && m_source->error() != QAudio::NoError) {
                m_error = QCoreApplication::translate(
                    "kk::AudioInput", "The microphone stopped delivering sound (%1).")
                              .arg(m_device.description());
                stop();
                emit failed(m_error);
            }
        });
        return true;
    }

    void stop() override
    {
        if (!m_source)
            return;
        QAudioSource* source = m_source;
        m_source = nullptr;
        m_io = nullptr;
        source->disconnect(this);
        source->stop();
        source->deleteLater();
    }

    [[nodiscard]] int sampleRate() const override { return m_format.sampleRate(); }
    [[nodiscard]] int channels() const override { return m_format.channelCount(); }

    [[nodiscard]] qint64 latencyUs() const override
    {
        // Qt does not report the capture latency; the size of the capture buffer is the delay
        // between a sound and the moment its block can be read.
        const qint64 bps
            = qint64(m_format.bytesPerSample()) * m_format.channelCount() * m_format.sampleRate();
        const qint64 buffer = m_source ? m_source->bufferSize() : 0;
        return bps > 0 ? buffer * 1000000 / bps : 0;
    }

private:
    void drain()
    {
        if (!m_io)
            return;
        const QByteArray bytes = m_io->readAll();
        const int width = m_format.bytesPerSample();
        const qsizetype count = width > 0 ? bytes.size() / width : 0;
        if (count == 0)
            return;
        std::vector<float> block(static_cast<size_t>(count));
        const char* p = bytes.constData();
        switch (m_format.sampleFormat()) {
        case QAudioFormat::Float:
            std::memcpy(block.data(), p, size_t(count) * sizeof(float));
            break;
        case QAudioFormat::Int16:
            for (qsizetype i = 0; i < count; ++i) {
                qint16 v;
                std::memcpy(&v, p + i * 2, 2);
                block[size_t(i)] = float(v) / 32768.0f;
            }
            break;
        case QAudioFormat::Int32:
            for (qsizetype i = 0; i < count; ++i) {
                qint32 v;
                std::memcpy(&v, p + i * 4, 4);
                block[size_t(i)] = float(double(v) / 2147483648.0);
            }
            break;
        case QAudioFormat::UInt8:
            for (qsizetype i = 0; i < count; ++i)
                block[size_t(i)] = (float(quint8(p[i])) - 128.0f) / 128.0f;
            break;
        default:
            return;
        }
        emit samples(block);
    }

    QAudioDevice m_device;
    QAudioFormat m_format;
    QAudioSource* m_source = nullptr;
    QIODevice* m_io = nullptr;
};

} // namespace

AudioInputFactory qtAudioInputFactory()
{
    return [](const QByteArray& deviceId, QString* error) -> std::unique_ptr<AudioInput> {
        QAudioDevice chosen;
        if (!deviceId.isEmpty()) {
            for (const QAudioDevice& d : QMediaDevices::audioInputs()) {
                if (d.id() == deviceId)
                    chosen = d;
            }
        }
        if (chosen.isNull())
            chosen = QMediaDevices::defaultAudioInput();
        if (chosen.isNull()) {
            if (error)
                *error = systemAudioInputProblem();
            return nullptr;
        }
        return std::make_unique<QtAudioInput>(chosen);
    };
}

} // namespace kk
