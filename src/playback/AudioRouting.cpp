// SPDX-License-Identifier: GPL-3.0-or-later
// AudioRouting — device choice, played-frame bookkeeping and ring pulling (pure logic).
#include "playback/AudioRouting.h"

#include "playback/AudioRing.h"

#include <algorithm>

namespace kk {

namespace {
/// Initial size of the int16 conversion scratch buffer (frames); grows if a device asks more.
constexpr size_t kScratchFrames = 16384;

bool contains(const std::vector<AudioDeviceInfo>& devices, const QByteArray& id)
{
    return std::any_of(devices.begin(), devices.end(),
        [&id](const AudioDeviceInfo& device) { return device.id == id; });
}
} // namespace

AudioRouteChoice chooseAudioOutput(const QByteArray& preferredId,
    const std::vector<AudioDeviceInfo>& available, const QByteArray& defaultId)
{
    using Reason = AudioRouteChoice::Reason;
    if (!preferredId.isEmpty() && contains(available, preferredId)) {
        return {preferredId, Reason::Chosen};
    }
    const Reason fallback = preferredId.isEmpty() ? Reason::SystemDefault : Reason::ChosenMissing;
    if (!defaultId.isEmpty() && contains(available, defaultId)) {
        return {defaultId, fallback};
    }
    // The default is unknown or already gone (the server announces the removal of a device
    // and the new default in separate events): any connected device beats silence.
    if (!available.empty()) {
        return {available.front().id, fallback};
    }
    // No list at all but a default (some backends report the default before the list).
    if (!defaultId.isEmpty()) {
        return {defaultId, fallback};
    }
    return {};
}

int64_t PlayedFramesTracker::update(
    int64_t processedFrames, int64_t bufferedFrames, int64_t pulledTotal)
{
    // A stream that has not played anything yet says nothing (not "at its first frame"):
    // after a switch the count holds until the new device really starts.
    const int64_t reported = processedFrames > 0 ? m_base + processedFrames : 0;
    const int64_t handedOver = pulledTotal - std::max<int64_t>(0, bufferedFrames);
    const int64_t played = std::max<int64_t>(0, std::min(reported, handedOver));
    m_played = std::max(m_played, played);
    return m_played;
}

RingPuller::RingPuller(AudioRing* ring, bool int16)
    : m_ring(ring)
{
    setInt16(int16);
}

void RingPuller::setInt16(bool int16)
{
    m_int16 = int16;
    if (m_int16 && m_scratch.empty()) {
        m_scratch.resize(kScratchFrames * kChannels); // float staging of the conversion
    }
}

void RingPuller::pull(char* data, size_t frames)
{
    if (frames == 0) {
        return;
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
}

} // namespace kk
