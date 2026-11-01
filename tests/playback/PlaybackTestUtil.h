// SPDX-License-Identifier: GPL-3.0-or-later
// PlaybackTestUtil — fake audio device and timeline helpers for the playback tests.
#pragma once

#include "playback/AudioOutput.h"
#include "playback/AudioRing.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace kk::test {

/// AudioOutput that behaves like a sound card without making sound: a thread pulls one
/// "period" from the ring every periodMs (consuming at 48 kHz * drift) and reports the heard
/// position latencyMs behind what it pulled, updated once per period (the step-wise,
/// jittery reports real devices give). trueHeardFrames() is the exact ground truth.
class FakeAudioOutput final : public AudioOutput
{
public:
    explicit FakeAudioOutput(int periodMs = 10, int latencyMs = 40, double drift = 1.0)
        : m_periodMs(periodMs)
        , m_latencyMs(latencyMs)
        , m_drift(drift)
    { }
    ~FakeAudioOutput() override { stop(); }

    bool isAvailable() const override { return true; }
    QString description() const override { return QStringLiteral("fake"); }
    void setVolume(float) override { }

    bool start(AudioRing* ring) override
    {
        stop();
        m_ring = ring;
        m_played = 0;
        m_atUs = monotonicUs();
        m_startUs = monotonicUs();
        m_starts.fetch_add(1);
        m_quit = false;
        m_thread = std::thread([this] { run(); });
        return true;
    }

    void stop() override
    {
        if (m_thread.joinable()) {
            m_quit = true;
            m_thread.join();
        }
    }

    void playedFrames(int64_t* frames, int64_t* atUs) const override
    {
        *atUs = m_atUs.load();
        *frames = m_played.load();
    }

    /// Exact heard sample frames since start() at wall time @p nowUs.
    [[nodiscard]] double trueHeardFrames(int64_t nowUs) const
    {
        const double heard
            = (double(nowUs - m_startUs) / 1e6 - m_latencyMs / 1000.0) * 48000.0 * m_drift;
        return std::max(0.0, heard);
    }
    [[nodiscard]] int starts() const { return m_starts.load(); }
    /// Frames pulled that were silence (ring empty).
    [[nodiscard]] int64_t underrunFrames() const { return m_underrun.load(); }

private:
    void run()
    {
        std::vector<float> buffer;
        int64_t pulled = 0;
        while (!m_quit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_periodMs));
            const int64_t now = monotonicUs();
            // Pull up to where real time says the device is.
            const int64_t target = int64_t(double(now - m_startUs) / 1e6 * 48000.0 * m_drift);
            const int64_t want = target - pulled;
            if (want > 0) {
                buffer.resize(size_t(want) * 2);
                const size_t got = m_ring->read(buffer.data(), size_t(want));
                m_underrun.fetch_add(want - int64_t(got));
                pulled = target;
            }
            m_played = std::max<int64_t>(0, int64_t(trueHeardFrames(now)));
            m_atUs = now;
        }
    }

    int m_periodMs;
    int m_latencyMs;
    double m_drift;
    AudioRing* m_ring = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_quit {false};
    std::atomic<int64_t> m_played {0};
    std::atomic<int64_t> m_atUs {0};
    std::atomic<int> m_starts {0};
    std::atomic<int64_t> m_underrun {0};
    int64_t m_startUs = 0;
};

} // namespace kk::test
