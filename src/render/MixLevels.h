// SPDX-License-Identifier: GPL-3.0-or-later
// MixLevels — the meter readings of one AudioMixer::mix() call (per track, per bus, output)
// and MeterFrame, the time-stamped copy the playback engine hands to the meters.
#pragma once

#include <QtGlobal>

#include <array>
#include <cstdint>

namespace kk {

/// Peak level of one mixer channel over a mix block.
struct ChannelLevel
{
    quint64 id = 0; ///< TrackData::id or BusData::id
    float peak[2] = {0.0f, 0.0f}; ///< linear sample peak per side (post fader)
    float gainReductionDb = 0.0f; ///< dynamics reduction at the end of the block (>= 0)
    bool mono = false; ///< mono channel: peak[1] repeats peak[0]
};

/// Fixed-capacity list of channel levels (no allocation in the audio path).
template<int Capacity>
struct LevelList
{
    std::array<ChannelLevel, Capacity> items {};
    int count = 0;

    void clear() { count = 0; }
    /// Appends (silently drops entries beyond the capacity).
    ChannelLevel* add()
    {
        if (count >= Capacity)
            return nullptr;
        items[size_t(count)] = ChannelLevel {};
        return &items[size_t(count++)];
    }
    /// The entry with @p id, or nullptr.
    [[nodiscard]] const ChannelLevel* find(quint64 id) const
    {
        for (int i = 0; i < count; ++i) {
            if (items[size_t(i)].id == id)
                return &items[size_t(i)];
        }
        return nullptr;
    }
};

/// Most audio tracks / buses metered (more are mixed but not metered).
inline constexpr int kMaxMeteredTracks = 64;
inline constexpr int kMaxMeteredBuses = 16;

/// Meter readings of one mix() call of the master mixer (AudioMixer::setLevels()).
struct MixLevels
{
    LevelList<kMaxMeteredTracks> tracks;
    LevelList<kMaxMeteredBuses> buses;
    float output[2] = {0.0f, 0.0f}; ///< linear peak of what the mixer returned

    /// Clears for the next block.
    void clear()
    {
        tracks.clear();
        buses.clear();
        output[0] = output[1] = 0.0f;
    }
    /// Merges @p other in (the larger peak per entry; entries missing here are added).
    void merge(const MixLevels& other);
};

/// What the meters show for the audio heard at one moment: the levels of one mix block of
/// the playback engine plus the loudness of the output up to that block.
struct MeterFrame
{
    int64_t sample = -1; ///< timeline sample (48 kHz) at the start of the block; -1 = none
    MixLevels levels;
    double momentaryLufs = -200.0;
    double shortTermLufs = -200.0;
    double integratedLufs = -200.0;
    double truePeakDb = -200.0; ///< of this block (dBTP)
    double truePeakMaxDb = -200.0; ///< since the last loudness reset (dBTP)
};

} // namespace kk
