// SPDX-License-Identifier: GPL-3.0-or-later
// AudioOutput — QAudioSink (pull mode) on a dedicated thread, reading from an AudioRing.
#include "playback/AudioOutput.h"

#include "core/Preferences.h"
#include "playback/AudioRing.h"
#include "playback/PlaybackLog.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>

namespace kk {

int64_t monotonicUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
/// Device-side buffer. Small enough for responsive starts, large enough to ride out a busy
/// system (the AudioRing in front of it holds another ~170 ms).
constexpr int kDeviceBufferMs = 60;
/// How often the audio thread refreshes the played-position estimate between device pulls.
constexpr int kPositionPollMs = 5;
/// Initial size of the int16 conversion scratch buffer (frames); grows if a device asks more.
constexpr size_t kScratchFrames = 16384;

/// Played-position estimate shared between the audio thread (writer) and the GUI thread.
struct PositionState
{
    std::atomic<int64_t> played {0};
    std::atomic<int64_t> atUs {0};
};

/// QIODevice the QAudioSink pulls from. Lives on (and is only used by) the audio thread.
class RingDevice : public QIODevice
{
public:
    RingDevice(AudioRing* ring, bool int16)
        : m_ring(ring)
        , m_int16(int16)
    {
        if (m_int16) {
            m_scratch.resize(kScratchFrames * kChannels);
        }
    }

    /// Frames handed to the device so far, silence padding included.
    [[nodiscard]] int64_t pulled() const { return m_pulled; }

protected:
    qint64 readData(char* data, qint64 maxlen) override
    {
        const int bytesPerFrame = kChannels * (m_int16 ? int(sizeof(int16_t)) : int(sizeof(float)));
        const size_t frames = size_t(maxlen / bytesPerFrame);
        if (frames == 0) {
            return 0;
        }
        float* out = reinterpret_cast<float*>(data);
        if (m_int16) {
            if (m_scratch.size() < frames * kChannels) {
                m_scratch.resize(frames * kChannels); // rare: device asked for a bigger chunk
            }
            out = m_scratch.data();
        }
        // Pay back earlier underruns first: the frames that should have played during the
        // silence are dropped so the device position keeps matching the timeline position.
        while (m_skipDebt > 0) {
            const size_t skipped = m_ring->read(out, std::min<size_t>(size_t(m_skipDebt), frames));
            if (skipped == 0) {
                break;
            }
            m_skipDebt -= int64_t(skipped);
        }
        const size_t got = m_ring->read(out, frames);
        if (got < frames) {
            std::fill(out + got * kChannels, out + frames * kChannels, 0.0f);
            m_skipDebt += int64_t(frames - got);
        }
        if (m_int16) {
            auto* dst = reinterpret_cast<int16_t*>(data);
            for (size_t i = 0; i < frames * kChannels; ++i) {
                dst[i] = int16_t(std::clamp(out[i], -1.0f, 1.0f) * 32767.0f);
            }
        }
        m_pulled += int64_t(frames);
        return qint64(frames) * bytesPerFrame;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
    [[nodiscard]] bool isSequential() const override { return true; }
    [[nodiscard]] qint64 bytesAvailable() const override
    {
        // Always "has data": underruns are padded with silence in readData().
        return (1 << 20) + QIODevice::bytesAvailable();
    }

private:
    AudioRing* m_ring;
    bool m_int16;
    std::vector<float> m_scratch;
    int64_t m_pulled = 0;
    int64_t m_skipDebt = 0;
};

/// QAudioSink-backed AudioOutput. The sink, its QIODevice and a position timer are created on
/// the audio thread for every start() and destroyed on stop(): a fresh stream per play makes
/// processedUSecs() start at 0 and discards whatever the previous play left in the device.
class QtAudioOutput final : public AudioOutput
{
public:
    QtAudioOutput() = default;

    ~QtAudioOutput() override
    {
        if (m_initialised) {
            stop();
            m_thread.quit();
            m_thread.wait();
        }
    }

    bool isAvailable() const override
    {
        ensureInitialised();
        return !m_device.isNull();
    }

    QString description() const override
    {
        ensureInitialised();
        if (m_device.isNull()) {
            return QStringLiteral("none");
        }
        return m_device.description()
            + (m_int16 ? QStringLiteral(" (int16)") : QStringLiteral(" (float)"));
    }

    bool start(AudioRing* ring) override
    {
        ensureInitialised();
        if (m_device.isNull()) {
            return false;
        }
        stop();
        m_position.played.store(0);
        m_position.atUs.store(monotonicUs());
        bool ok = false;
        // Blocking: creating a stream takes a few ms; afterwards the device pulls on its own.
        QMetaObject::invokeMethod(
            &m_context, [this, ring, &ok] { ok = startOnAudioThread(ring); },
            Qt::BlockingQueuedConnection);
        return ok;
    }

    void stop() override
    {
        if (!m_initialised) {
            return;
        }
        QMetaObject::invokeMethod(
            &m_context, [this] { stopOnAudioThread(); }, Qt::BlockingQueuedConnection);
    }

    void setVolume(float volume) override
    {
        m_volume = volume;
        if (!m_initialised) {
            return; // applied when the sink is created
        }
        QMetaObject::invokeMethod(&m_context, [this, volume] {
            if (m_sink) {
                m_sink->setVolume(volume);
            }
        });
    }

    void playedFrames(int64_t* frames, int64_t* atUs) const override
    {
        // Two relaxed-consistency loads: a torn read is off by at most one poll period
        // (kPositionPollMs), which the clock's slew absorbs.
        *atUs = m_position.atUs.load(std::memory_order_acquire);
        *frames = m_position.played.load(std::memory_order_acquire);
    }

private:
    /// Device query and audio thread are set up on first use: engines that never play (most
    /// source viewers, tests) cost nothing. GUI thread (QMediaDevices).
    void ensureInitialised() const
    {
        if (m_initialised) {
            return;
        }
        auto* self = const_cast<QtAudioOutput*>(this);
        self->m_initialised = true;
        // KK_NO_AUDIO=1 (tests, CI, headless renders): behave as if there were no device, so
        // nothing ever becomes audible and the engine uses its wall clock.
        if (qEnvironmentVariableIntValue("KK_NO_AUDIO") == 0) {
            self->m_device = QMediaDevices::defaultAudioOutput();
            // Preferences > System > Audio I/O: a chosen device, if it is connected.
            const QByteArray wanted = prefs::audioOutputDevice().toUtf8();
            for (const QAudioDevice& device : QMediaDevices::audioOutputs()) {
                if (!wanted.isEmpty() && device.id() == wanted)
                    self->m_device = device;
            }
        }
        self->m_format.setSampleRate(kSampleRate);
        self->m_format.setChannelCount(kChannels);
        self->m_format.setSampleFormat(QAudioFormat::Float);
        if (!m_device.isNull() && !m_device.isFormatSupported(m_format)) {
            self->m_format.setSampleFormat(QAudioFormat::Int16);
            self->m_int16 = true;
        }
        self->m_thread.setObjectName(QStringLiteral("kk.audio"));
        self->m_context.moveToThread(&self->m_thread);
        self->m_thread.start(QThread::TimeCriticalPriority);
        qCInfo(lcPlayback).noquote() << "audio output:" << description();
    }

    bool startOnAudioThread(AudioRing* ring)
    {
        m_sink = new QAudioSink(m_device, m_format);
        const int bytesPerFrame = m_format.bytesPerFrame();
        m_sink->setBufferSize(kSampleRate * kDeviceBufferMs / 1000 * bytesPerFrame);
        m_sink->setVolume(m_volume);
        m_ringDevice = new RingDevice(ring, m_int16);
        m_ringDevice->open(QIODevice::ReadOnly);
        m_sink->start(m_ringDevice);
        if (m_sink->error() != QAudio::NoError) {
            qCWarning(lcPlayback) << "audio sink failed to start, error" << m_sink->error();
            stopOnAudioThread();
            return false;
        }
        m_timer = new QTimer;
        m_timer->setTimerType(Qt::PreciseTimer);
        QObject::connect(m_timer, &QTimer::timeout, m_timer, [this] { updatePosition(); });
        m_timer->start(kPositionPollMs);
        return true;
    }

    void stopOnAudioThread()
    {
        delete m_timer;
        m_timer = nullptr;
        if (m_sink) {
            m_sink->stop();
            delete m_sink;
            m_sink = nullptr;
        }
        delete m_ringDevice;
        m_ringDevice = nullptr;
    }

    /// Heard frames = min(device-reported processed time, frames handed over minus what is
    /// still queued in the sink's buffer). Backends differ: some report processedUSecs() as
    /// "written to the server" (then the second term is the right one), PulseAudio/PipeWire
    /// report the playing position (already latency compensated, then the first term is
    /// smaller). Taking the minimum is right for both.
    void updatePosition()
    {
        if (!m_sink || !m_ringDevice) {
            return;
        }
        const int bytesPerFrame = m_format.bytesPerFrame();
        const int64_t processed = m_sink->processedUSecs() * kSampleRate / 1000000;
        const int64_t buffered = std::max<int64_t>(
            0, int64_t(m_sink->bufferSize() - m_sink->bytesFree()) / bytesPerFrame);
        const int64_t queuedEstimate = m_ringDevice->pulled() - buffered;
        const int64_t played = std::max<int64_t>(0, std::min(processed, queuedEstimate));
        const int64_t previous = m_position.played.load(std::memory_order_relaxed);
        if (played > previous) {
            m_position.played.store(played, std::memory_order_release);
            m_position.atUs.store(monotonicUs(), std::memory_order_release);
        }
    }

    bool m_initialised = false;
    QAudioDevice m_device;
    QAudioFormat m_format;
    bool m_int16 = false;
    float m_volume = 1.0f;
    QThread m_thread;
    QObject m_context; ///< lives on m_thread; target of invokeMethod()
    // Audio-thread objects:
    QAudioSink* m_sink = nullptr;
    RingDevice* m_ringDevice = nullptr;
    QTimer* m_timer = nullptr;
    PositionState m_position;
};

} // namespace

std::unique_ptr<AudioOutput> createDefaultAudioOutput()
{
    return std::make_unique<QtAudioOutput>();
}

} // namespace kk
