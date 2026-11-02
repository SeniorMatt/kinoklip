// SPDX-License-Identifier: GPL-3.0-or-later
// MixerData — the Audio page's mixer model: channel strips (EQ, dynamics, pan, fader) of
// audio tracks and buses, bus routing, the output bus and track fader / pan automation.
#pragma once

#include "core/Animation.h"

#include <QString>
#include <QVector>

#include <array>
#include <cstdint>
#include <vector>

namespace kk {

/// Shape of one equalizer band (biquads from the RBJ audio EQ cookbook, render/Biquad.h).
enum class EqBandType {
    HighPass, ///< 12 dB / octave high-pass (gain unused)
    LowShelf,
    Bell, ///< peaking band
    Notch, ///< band reject (gain unused)
    HighShelf,
    LowPass ///< 12 dB / octave low-pass (gain unused)
};

/// One band of the parametric equalizer.
struct EqBand
{
    EqBandType type = EqBandType::Bell;
    bool enabled = true;
    double frequencyHz = 1000.0; ///< centre / corner frequency
    double gainDb = 0.0; ///< bells and shelves; ignored by the filters and the notch
    double q = 1.0; ///< bandwidth (bells, notch) or slope (filters, shelves; 0.707 = flat)

    friend bool operator==(const EqBand&, const EqBand&) = default;
};

/// Number of equalizer bands per channel strip (band 1 and 6 default to the filters).
inline constexpr int kEqBandCount = 6;
/// Limits of the equalizer controls (UI, ops and the loader clamp to them).
inline constexpr double kEqMinFrequencyHz = 20.0;
inline constexpr double kEqMaxFrequencyHz = 20000.0;
inline constexpr double kEqMaxGainDb = 24.0; ///< bells and shelves: -24 ... +24 dB
inline constexpr double kEqMinQ = 0.1;
inline constexpr double kEqMaxQ = 18.0;

/// The default bands: high-pass 80 Hz and low-pass 16 kHz (both off), low shelf 100 Hz,
/// bells at 500 Hz and 2 kHz, high shelf 8 kHz (all 0 dB, so the equalizer is neutral).
[[nodiscard]] std::array<EqBand, kEqBandCount> defaultEqBands();

/// The 6-band parametric equalizer of a channel strip.
struct EqSettings
{
    bool enabled = false; ///< the strip's EQ switch (off = bypassed)
    std::array<EqBand, kEqBandCount> bands = defaultEqBands();

    friend bool operator==(const EqSettings&, const EqSettings&) = default;
};

/// Compressor and limiter of a channel strip (render/Dynamics.h implements them).
struct DynamicsSettings
{
    bool compressorEnabled = false;
    double thresholdDb = -20.0; ///< level above which the gain is reduced (dBFS)
    double ratio = 4.0; ///< input dB above the threshold per output dB (>= 1)
    double attackMs = 5.0; ///< time constant of the gain reduction rising
    double releaseMs = 120.0; ///< time constant of the gain reduction falling
    double makeupDb = 0.0; ///< gain added after the compressor
    double kneeDb = 0.0; ///< soft knee width (0 = hard knee)
    bool limiterEnabled = false;
    double limiterCeilingDb = -1.0; ///< the output never exceeds this sample level (dBFS)
    double limiterReleaseMs = 50.0;

    friend bool operator==(const DynamicsSettings&, const DynamicsSettings&) = default;
};

/// Limits of the dynamics controls.
inline constexpr double kDynMinThresholdDb = -60.0;
inline constexpr double kDynMaxRatio = 20.0;
inline constexpr double kDynMinTimeMs = 0.1;
inline constexpr double kDynMaxAttackMs = 300.0;
inline constexpr double kDynMaxReleaseMs = 4000.0;
inline constexpr double kDynMaxMakeupDb = 24.0;
inline constexpr double kDynMaxKneeDb = 24.0;
inline constexpr double kLimiterMinCeilingDb = -30.0;

/// The processing of one mixer channel (a track or a bus), in signal flow order:
/// EQ -> dynamics -> pan -> fader -> mute.
struct ChannelStrip
{
    EqSettings eq;
    DynamicsSettings dynamics;
    double volumeDb = 0.0; ///< fader (static value; tracks may automate it)
    double pan = 0.0; ///< -1 (left) ... +1 (right); buses ignore it
    bool muted = false; ///< buses only: tracks use TrackData::muted (header and mixer agree)

    friend bool operator==(const ChannelStrip&, const ChannelStrip&) = default;
};

/// Fader range (dB): -96 is silence ("-inf" in the UI), +12 the top of the fader.
inline constexpr double kFaderMinDb = -96.0;
inline constexpr double kFaderMaxDb = 12.0;

/// What a track does with its fader / pan automation (Resolve's automation modes).
enum class AutomationMode {
    Off, ///< automation is ignored; the fader's static value plays
    Read, ///< the recorded curve plays (default)
    Touch, ///< reads; while the fader is held during playback its moves are recorded
    Write ///< during playback the fader's value is recorded continuously
};

/// Bus id of the main bus (Bus 1, always present, stereo by default).
inline constexpr int kMainBusId = 1;

/// Mixer settings of an audio track (TrackData::mix).
struct TrackMix
{
    ChannelStrip strip;
    bool mono = false; ///< channel format: mono (sources summed) or stereo
    QVector<int> buses {kMainBusId}; ///< bus ids the track feeds (none = not heard)
    AutomationMode automation = AutomationMode::Read;
    /// Fader ("volumeDb") and pan ("pan") automation, keyed in timeline frames (not clip
    /// local: track automation stays where it was written).
    ParamSet automationKeys;

    friend bool operator==(const TrackMix&, const TrackMix&) = default;
};

/// A mix bus: the sum of the tracks routed to it, with its own channel strip.
struct BusData
{
    int id = kMainBusId; ///< stable, unique within the timeline (1 = main bus)
    QString name; ///< empty = automatic ("Bus <id>")
    bool mono = false;
    ChannelStrip strip;

    friend bool operator==(const BusData&, const BusData&) = default;
};

/// The timeline's buses and output selection (TimelineData::mixer).
struct MixerData
{
    std::vector<BusData> buses {BusData {}}; ///< always starts with the main bus
    int outputBus = kMainBusId; ///< heard in playback and rendered by export
    int nextBusId = kMainBusId + 1;

    friend bool operator==(const MixerData&, const MixerData&) = default;
};

/// Automation parameter keys (TrackMix::automationKeys).
namespace mixkeys {
inline const QString kVolumeDb = QStringLiteral("volumeDb");
inline const QString kPan = QStringLiteral("pan");
} // namespace mixkeys

/// Display name of @p bus ("Bus 2" when unnamed).
[[nodiscard]] QString busDisplayName(const BusData& bus);
/// Stable string of an automation mode ("off", "read", "touch", "write").
[[nodiscard]] QString automationModeToString(AutomationMode mode);
/// Parses automationModeToString(); unknown text gives @p fallback.
[[nodiscard]] AutomationMode automationModeFromString(const QString& text, AutomationMode fallback);
/// Stable string of a band type ("highpass", "lowshelf", "bell", "notch", "highshelf",
/// "lowpass").
[[nodiscard]] QString eqBandTypeToString(EqBandType type);
/// Parses eqBandTypeToString(); unknown text gives @p fallback.
[[nodiscard]] EqBandType eqBandTypeFromString(const QString& text, EqBandType fallback);
/// True if the band type uses its gain (bells and shelves).
[[nodiscard]] bool eqBandHasGain(EqBandType type);

} // namespace kk
