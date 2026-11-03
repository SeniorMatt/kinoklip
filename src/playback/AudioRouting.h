// SPDX-License-Identifier: GPL-3.0-or-later
// AudioRouting — pure logic behind the output device: which device to use (the user's choice
// or the system default), the played-position bookkeeping across a device switch, and the pull
// side of the ring that the sound device reads. No Qt Multimedia types, so all of it is
// unit-tested without sound hardware (tests/playback/test_audiorouting.cpp).
#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <vector>

namespace kk {

class AudioRing;

/// One output device as the routing logic sees it.
struct AudioDeviceInfo
{
    QByteArray id; ///< QAudioDevice::id() (PulseAudio / PipeWire sink name)
    QString description; ///< human-readable name ("Bluetooth Headphones")
};

/// Result of chooseAudioOutput().
struct AudioRouteChoice
{
    enum class Reason {
        None, ///< no output device at all
        SystemDefault, ///< no device chosen: the system default
        Chosen, ///< the device chosen in Preferences, connected
        ChosenMissing, ///< the chosen device is not connected: the system default instead
    };
    QByteArray id; ///< empty for Reason::None
    Reason reason = Reason::None;
};

/// Picks the output device. @p preferredId: the device chosen in Preferences > System > Audio
/// I/O ("" = Automatic). @p available: the connected outputs. @p defaultId: the system default
/// output (QMediaDevices::defaultAudioOutput(); may be empty or not in @p available while the
/// sound server is between states).
///
/// * Automatic follows the system default.
/// * A chosen device that is connected wins over the default.
/// * A chosen device that is missing falls back to the default; once it is connected again
///   the next call returns it again (the choice is kept, nothing is forgotten).
/// * Without a usable default, the first available device is used.
[[nodiscard]] AudioRouteChoice chooseAudioOutput(const QByteArray& preferredId,
    const std::vector<AudioDeviceInfo>& available, const QByteArray& defaultId);

/// Played-frame bookkeeping of one play (AudioOutput::playedFrames()), across device switches.
///
/// A sound device reports how much it has played *of its own stream*; a new stream (device
/// switch) starts at 0 again. The tracker knows how many ring frames had been handed to devices
/// when the current stream started (beginStream()), so the current stream's frame k is ring
/// frame base + k. The frames the previous device had taken but not yet played are lost with
/// it (their sound is skipped, at most the device buffer plus its latency); the played count
/// therefore steps forwards over them and stays exactly the number of the ring frame being
/// heard, which keeps the picture in sync with the sound. The count never goes backwards.
///
/// Thread-safety: none (used by the audio thread only).
class PlayedFramesTracker
{
public:
    /// A new play: nothing played, nothing pulled.
    void reset()
    {
        m_base = 0;
        m_played = 0;
    }
    /// A device stream starts after @p pulledTotal ring frames were handed to earlier streams.
    void beginStream(int64_t pulledTotal) { m_base = pulledTotal; }
    /// New estimate from the current stream: @p processedFrames played by the stream (its own
    /// count since beginStream()), @p bufferedFrames still queued in the stream's buffer,
    /// @p pulledTotal ring frames handed over in this play (all streams). Heard = the smaller of
    /// the stream's own report and "handed over minus queued" (backends differ in what
    /// processedUSecs() means, see AudioOutput.cpp). Returns the played count (monotonic).
    int64_t update(int64_t processedFrames, int64_t bufferedFrames, int64_t pulledTotal);
    /// Last value returned by update() (0 after reset()).
    [[nodiscard]] int64_t played() const { return m_played; }
    /// Ring frames handed to earlier streams of this play.
    [[nodiscard]] int64_t streamBase() const { return m_base; }

private:
    int64_t m_base = 0;
    int64_t m_played = 0;
};

/// The pull side of the device: hands ring frames to the device, pads underruns with silence
/// and later skips as many ring frames, so device frame n always carries ring frame n. It is
/// kept across a device switch (with the ring), so the next device continues exactly where the
/// previous one stopped. Allocation-free in steady state.
///
/// Thread-safety: none (audio thread).
class RingPuller
{
public:
    static constexpr int kChannels = 2;

    /// @p ring: not owned. @p int16: produce int16 samples instead of float.
    RingPuller(AudioRing* ring, bool int16);

    /// Fills @p frames frames into @p out (float or int16 interleaved stereo, see setInt16()).
    /// Always produces all @p frames (silence where the ring is empty).
    void pull(char* out, size_t frames);
    /// Sample format of the next pull() (a device switch may change it).
    void setInt16(bool int16);
    [[nodiscard]] bool isInt16() const { return m_int16; }
    [[nodiscard]] int bytesPerFrame() const
    {
        return kChannels * (m_int16 ? int(sizeof(int16_t)) : int(sizeof(float)));
    }
    /// Frames handed out so far, silence padding included.
    [[nodiscard]] int64_t pulled() const { return m_pulled; }
    /// Ring frames still to be skipped for earlier underruns.
    [[nodiscard]] int64_t skipDebt() const { return m_skipDebt; }

private:
    AudioRing* m_ring;
    bool m_int16 = false;
    std::vector<float> m_scratch;
    int64_t m_pulled = 0;
    int64_t m_skipDebt = 0;
};

} // namespace kk
