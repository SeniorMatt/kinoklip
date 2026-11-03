// SPDX-License-Identifier: GPL-3.0-or-later
// AudioOutput — QAudioSink (pull mode) on a dedicated thread, reading from an AudioRing.
#include "playback/AudioOutput.h"

#include "media/MediaLog.h"
#include "playback/AudioDeviceMonitor.h"
#include "playback/AudioRing.h"
#include "playback/AudioRouting.h"
#include "playback/PlaybackLog.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>

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

/// Played-position estimate shared between the audio thread (writer) and the GUI thread.
struct PositionState
{
    std::atomic<int64_t> played {0};
    std::atomic<int64_t> atUs {0};
};

/// QIODevice the QAudioSink pulls from (a RingPuller behind the QIODevice interface). Lives on
/// (and is only used by) the audio thread. One per play: a device switch keeps it, so the new
/// stream continues at the ring position (and with the underrun debt) of the previous one.
class RingDevice : public QIODevice
{
public:
    RingDevice(AudioRing* ring, bool int16)
        : m_puller(ring, int16)
    { }

    [[nodiscard]] RingPuller& puller() { return m_puller; }

protected:
    qint64 readData(char* data, qint64 maxlen) override
    {
        const int bytesPerFrame = m_puller.bytesPerFrame();
        const size_t frames = size_t(maxlen / bytesPerFrame);
        m_puller.pull(data, frames);
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
    RingPuller m_puller;
};

/// 48 kHz stereo float, or int16 when @p device rejects float.
QAudioFormat formatFor(const QAudioDevice& device)
{
    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(kChannels);
    format.setSampleFormat(QAudioFormat::Float);
    if (!device.isNull() && !device.isFormatSupported(format)) {
        format.setSampleFormat(QAudioFormat::Int16);
    }
    return format;
}

QString describe(const QAudioDevice& device, const QAudioFormat& format)
{
    if (device.isNull()) {
        return QStringLiteral("none");
    }
    return device.description()
        + (format.sampleFormat() == QAudioFormat::Int16 ? QStringLiteral(" (int16)")
                                                        : QStringLiteral(" (float)"));
}

/// QAudioSink-backed AudioOutput. The sink, its QIODevice and a position timer are created on
/// the audio thread for every start() and destroyed on stop(): a fresh stream per play makes
/// processedUSecs() start at 0 and discards whatever the previous play left in the device.
///
/// The device follows AudioDeviceMonitor (system default or the chosen device, see there).
/// When it changes during a play, only the QAudioSink is replaced: the ring, its contents, the
/// RingDevice (ring position, underrun debt) and the played-frame count are kept, so the engine
/// clock continues (it steps forwards over the few ms the old device had buffered, never
/// backwards) and playback goes on without a stop.
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
        return describe(m_device, m_format);
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
        const QAudioDevice device = m_device;
        const QAudioFormat format = m_format;
        QMetaObject::invokeMethod(
            &m_context,
            [this, ring, &device, &format, &ok] { ok = startOnAudioThread(ring, device, format); },
            Qt::BlockingQueuedConnection);
        m_running = ok;
        return ok;
    }

    void stop() override
    {
        if (!m_initialised) {
            return;
        }
        m_running = false;
        QMetaObject::invokeMethod(
            &m_context, [this] { stopOnAudioThread(); }, Qt::BlockingQueuedConnection);
    }

    void setVolume(float volume) override
    {
        m_volume.store(volume);
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
    /// source viewers, tests) cost nothing. GUI thread (AudioDeviceMonitor).
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
            AudioDeviceMonitor* monitor = AudioDeviceMonitor::instance();
            self->m_device = monitor->current();
            QObject::connect(monitor, &AudioDeviceMonitor::currentChanged, &self->m_guiContext,
                [self](const QAudioDevice& device) { self->switchDevice(device); });
        }
        // The monitor just initialised Qt Multimedia, whose FFmpeg backend replaces FFmpeg's
        // log callback: take FFmpeg's messages back into kk.media.
        media::installFfmpegLogRouting();
        self->m_format = formatFor(m_device);
        self->m_thread.setObjectName(QStringLiteral("kk.audio"));
        self->m_context.moveToThread(&self->m_thread);
        self->m_thread.start(QThread::TimeCriticalPriority);
        qCInfo(lcPlayback).noquote() << "audio output:" << description();
    }

    /// AudioDeviceMonitor::currentChanged (GUI thread): an idle output just remembers the
    /// device for its next start(); a playing one moves its stream now.
    void switchDevice(const QAudioDevice& device)
    {
        if (device.id() == m_device.id() && device.isNull() == m_device.isNull()) {
            return;
        }
        const QString from = describe(m_device, m_format);
        m_device = device;
        m_format = formatFor(device);
        if (!m_running) {
            return;
        }
        if (device.isNull()) {
            // Nothing left to play on: the engine notices the stalled position and falls back
            // to its wall clock; the next start() reports no device.
            stop();
            qCInfo(lcPlayback).noquote()
                << "audio output: stream on" << from << "stopped, no output device left";
            return;
        }
        bool ok = false;
        const QAudioFormat format = m_format;
        QMetaObject::invokeMethod(
            &m_context, [this, &device, &format, &ok] { ok = switchOnAudioThread(device, format); },
            Qt::BlockingQueuedConnection);
        if (ok) {
            qCInfo(lcPlayback).noquote() << "audio output: playing stream moved from" << from
                                         << "to" << describe(m_device, m_format);
        } else {
            qCWarning(lcPlayback).noquote()
                << "audio output: could not move the playing stream" << "from" << from << "to"
                << describe(m_device, m_format);
        }
    }

    bool startOnAudioThread(AudioRing* ring, const QAudioDevice& device, const QAudioFormat& format)
    {
        m_tracker.reset();
        m_ringDevice = new RingDevice(ring, format.sampleFormat() == QAudioFormat::Int16);
        m_ringDevice->open(QIODevice::ReadOnly);
        if (!openSink(device, format)) {
            stopOnAudioThread();
            return false;
        }
        m_timer = new QTimer;
        m_timer->setTimerType(Qt::PreciseTimer);
        QObject::connect(m_timer, &QTimer::timeout, m_timer, [this] { updatePosition(); });
        m_timer->start(kPositionPollMs);
        return true;
    }

    /// Opens a stream on @p device that continues pulling from m_ringDevice where the previous
    /// stream (if any) stopped.
    bool openSink(const QAudioDevice& device, const QAudioFormat& format)
    {
        m_sinkFormat = format;
        m_ringDevice->puller().setInt16(format.sampleFormat() == QAudioFormat::Int16);
        // The new stream's processedUSecs() starts at 0 at this ring frame.
        m_tracker.beginStream(m_ringDevice->puller().pulled());
        if (!m_ringDevice->isOpen()) {
            m_ringDevice->open(QIODevice::ReadOnly); // some backends close it on stop()
        }
        m_sink = new QAudioSink(device, format);
        m_sink->setBufferSize(kSampleRate * kDeviceBufferMs / 1000 * format.bytesPerFrame());
        m_sink->setVolume(m_volume.load());
        m_sink->start(m_ringDevice);
        if (m_sink->error() != QAudio::NoError) {
            qCWarning(lcPlayback) << "audio sink failed to start, error" << m_sink->error();
            closeSink(false);
            return false;
        }
        return true;
    }

    /// Stops and deletes the current stream; with @p measure, takes its last position first.
    void closeSink(bool measure)
    {
        if (!m_sink) {
            return;
        }
        if (measure) {
            updatePosition();
        }
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }

    /// Device switch during a play: a new stream on @p device, everything else kept.
    bool switchOnAudioThread(const QAudioDevice& device, const QAudioFormat& format)
    {
        if (!m_ringDevice) {
            return false; // stopped in the meantime
        }
        closeSink(true);
        return openSink(device, format);
    }

    void stopOnAudioThread()
    {
        delete m_timer;
        m_timer = nullptr;
        closeSink(false);
        delete m_ringDevice;
        m_ringDevice = nullptr;
    }

    /// Heard frames = min(device-reported processed time, frames handed over minus what is
    /// still queued in the sink's buffer). Backends differ: some report processedUSecs() as
    /// "written to the server" (then the second term is the right one), PulseAudio/PipeWire
    /// report the playing position (already latency compensated, then the first term is
    /// smaller). Taking the minimum is right for both. PlayedFramesTracker adds the frames of
    /// earlier streams of this play (device switches).
    void updatePosition()
    {
        if (!m_sink || !m_ringDevice) {
            return;
        }
        const int bytesPerFrame = m_sinkFormat.bytesPerFrame();
        const int64_t processed = m_sink->processedUSecs() * kSampleRate / 1000000;
        const int64_t buffered = std::max<int64_t>(
            0, int64_t(m_sink->bufferSize() - m_sink->bytesFree()) / bytesPerFrame);
        const int64_t previous = m_position.played.load(std::memory_order_relaxed);
        const int64_t played
            = m_tracker.update(processed, buffered, m_ringDevice->puller().pulled());
        if (played > previous) {
            m_position.played.store(played, std::memory_order_release);
            m_position.atUs.store(monotonicUs(), std::memory_order_release);
        }
    }

    // GUI thread:
    bool m_initialised = false;
    bool m_running = false; ///< a stream exists (between a successful start() and stop())
    QAudioDevice m_device;
    QAudioFormat m_format;
    QObject m_guiContext; ///< receiver of AudioDeviceMonitor signals (GUI thread)
    std::atomic<float> m_volume {1.0f};
    QThread m_thread;
    QObject m_context; ///< lives on m_thread; target of invokeMethod()
    // Audio-thread objects:
    QAudioSink* m_sink = nullptr;
    QAudioFormat m_sinkFormat;
    RingDevice* m_ringDevice = nullptr;
    QTimer* m_timer = nullptr;
    PlayedFramesTracker m_tracker;
    PositionState m_position;
};

} // namespace

std::unique_ptr<AudioOutput> createDefaultAudioOutput()
{
    return std::make_unique<QtAudioOutput>();
}

} // namespace kk
