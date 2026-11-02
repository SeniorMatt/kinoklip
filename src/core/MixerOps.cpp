// SPDX-License-Identifier: GPL-3.0-or-later
// MixerOps — bus, routing, strip and automation edits.
#include "core/MixerOps.h"

#include <algorithm>

namespace kk::mixerops {

namespace {

constexpr double kPanMin = -1.0;
constexpr double kPanMax = 1.0;

double clampAutomation(const QString& key, double value)
{
    return key == mixkeys::kPan ? std::clamp(value, kPanMin, kPanMax)
                                : std::clamp(value, kFaderMinDb, kFaderMaxDb);
}

} // namespace

TrackData* audioTrack(TimelineData& data, quint64 trackId)
{
    for (TrackData& t : data.audioTracks) {
        if (t.id == trackId)
            return &t;
    }
    return nullptr;
}

const TrackData* audioTrack(const TimelineData& data, quint64 trackId)
{
    return audioTrack(const_cast<TimelineData&>(data), trackId);
}

int busIndex(const MixerData& mixer, int busId)
{
    for (int i = 0; i < int(mixer.buses.size()); ++i) {
        if (mixer.buses[size_t(i)].id == busId)
            return i;
    }
    return -1;
}

BusData* bus(TimelineData& data, int busId)
{
    const int i = busIndex(data.mixer, busId);
    return i < 0 ? nullptr : &data.mixer.buses[size_t(i)];
}

const BusData* bus(const TimelineData& data, int busId)
{
    return bus(const_cast<TimelineData&>(data), busId);
}

ChannelStrip* strip(TimelineData& data, const MixTarget& target)
{
    if (target.isBus()) {
        BusData* b = bus(data, int(target.id));
        return b ? &b->strip : nullptr;
    }
    TrackData* t = audioTrack(data, target.id);
    return t ? &t->mix.strip : nullptr;
}

const ChannelStrip* strip(const TimelineData& data, const MixTarget& target)
{
    return strip(const_cast<TimelineData&>(data), target);
}

QString targetName(const TimelineData& data, const MixTarget& target)
{
    if (target.isBus()) {
        const BusData* b = bus(data, int(target.id));
        return b ? busDisplayName(*b) : QString();
    }
    for (int i = 0; i < int(data.audioTracks.size()); ++i) {
        if (data.audioTracks[size_t(i)].id == target.id)
            return trackDisplayName(data.audioTracks[size_t(i)], i);
    }
    return {};
}

int addBus(TimelineData& data, bool mono, const QString& name)
{
    BusData b;
    b.id = std::max(data.mixer.nextBusId, kMainBusId + 1);
    // Never reuse an id that is still present (damaged files with a stale allocator).
    for (const BusData& existing : data.mixer.buses)
        b.id = std::max(b.id, existing.id + 1);
    b.name = name;
    b.mono = mono;
    data.mixer.buses.push_back(b);
    data.mixer.nextBusId = b.id + 1;
    return b.id;
}

bool renameBus(TimelineData& data, int busId, const QString& name)
{
    BusData* b = bus(data, busId);
    if (!b || b->name == name)
        return false;
    b->name = name;
    return true;
}

bool setBusMono(TimelineData& data, int busId, bool mono)
{
    BusData* b = bus(data, busId);
    if (!b || b->mono == mono)
        return false;
    b->mono = mono;
    return true;
}

bool removeBus(TimelineData& data, int busId)
{
    const int i = busIndex(data.mixer, busId);
    if (i < 0 || busId == kMainBusId)
        return false;
    data.mixer.buses.erase(data.mixer.buses.begin() + i);
    for (TrackData& t : data.audioTracks)
        t.mix.buses.removeAll(busId);
    if (data.mixer.outputBus == busId)
        data.mixer.outputBus = kMainBusId;
    return true;
}

bool setOutputBus(TimelineData& data, int busId)
{
    if (!bus(data, busId) || data.mixer.outputBus == busId)
        return false;
    data.mixer.outputBus = busId;
    return true;
}

bool setTrackRoute(TimelineData& data, quint64 trackId, int busId, bool on)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t || !bus(data, busId) || t->mix.buses.contains(busId) == on)
        return false;
    if (on) {
        t->mix.buses.append(busId);
        std::sort(t->mix.buses.begin(), t->mix.buses.end());
    } else {
        t->mix.buses.removeAll(busId);
    }
    return true;
}

bool setTrackMono(TimelineData& data, quint64 trackId, bool mono)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t || t->mix.mono == mono)
        return false;
    t->mix.mono = mono;
    return true;
}

bool setAutomationMode(TimelineData& data, quint64 trackId, AutomationMode mode)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t || t->mix.automation == mode)
        return false;
    t->mix.automation = mode;
    return true;
}

void clampStrip(ChannelStrip& s)
{
    for (EqBand& b : s.eq.bands) {
        b.frequencyHz = std::clamp(b.frequencyHz, kEqMinFrequencyHz, kEqMaxFrequencyHz);
        b.gainDb = std::clamp(b.gainDb, -kEqMaxGainDb, kEqMaxGainDb);
        b.q = std::clamp(b.q, kEqMinQ, kEqMaxQ);
    }
    DynamicsSettings& d = s.dynamics;
    d.thresholdDb = std::clamp(d.thresholdDb, kDynMinThresholdDb, 0.0);
    d.ratio = std::clamp(d.ratio, 1.0, kDynMaxRatio);
    d.attackMs = std::clamp(d.attackMs, kDynMinTimeMs, kDynMaxAttackMs);
    d.releaseMs = std::clamp(d.releaseMs, kDynMinTimeMs, kDynMaxReleaseMs);
    d.makeupDb = std::clamp(d.makeupDb, 0.0, kDynMaxMakeupDb);
    d.kneeDb = std::clamp(d.kneeDb, 0.0, kDynMaxKneeDb);
    d.limiterCeilingDb = std::clamp(d.limiterCeilingDb, kLimiterMinCeilingDb, 0.0);
    d.limiterReleaseMs = std::clamp(d.limiterReleaseMs, kDynMinTimeMs, kDynMaxReleaseMs);
    s.volumeDb = std::clamp(s.volumeDb, kFaderMinDb, kFaderMaxDb);
    s.pan = std::clamp(s.pan, kPanMin, kPanMax);
}

bool readsAutomation(const TrackMix& mix, const QString& key)
{
    if (mix.automation == AutomationMode::Off)
        return false;
    const auto it = mix.automationKeys.values.constFind(key);
    return it != mix.automationKeys.values.constEnd() && it->isAnimated();
}

double faderDbAt(const TrackData& track, double frame)
{
    if (readsAutomation(track.mix, mixkeys::kVolumeDb))
        return track.mix.automationKeys.values.value(mixkeys::kVolumeDb).valueAtTime(frame);
    return track.mix.strip.volumeDb;
}

double panAt(const TrackData& track, double frame)
{
    if (readsAutomation(track.mix, mixkeys::kPan))
        return track.mix.automationKeys.values.value(mixkeys::kPan).valueAtTime(frame);
    return track.mix.strip.pan;
}

bool writeAutomation(TimelineData& data, quint64 trackId, const QString& key, int64_t frame,
    double value, int64_t eraseFrom)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t || frame < 0)
        return false;
    const double base = key == mixkeys::kPan ? t->mix.strip.pan : t->mix.strip.volumeDb;
    AnimatedValue& curve = t->mix.automationKeys.animated(key, base);
    const AnimatedValue before = curve;
    // Overwrite what the fader passed over since the last write (either direction).
    const int64_t lo = std::min(eraseFrom, frame);
    const int64_t hi = std::max(eraseFrom, frame);
    curve.keys.erase(std::remove_if(curve.keys.begin(), curve.keys.end(),
                         [lo, hi](const Keyframe& k) { return k.frame > lo && k.frame < hi; }),
        curve.keys.end());
    curve.setKey(frame, clampAutomation(key, value));
    return !(curve == before);
}

bool clearAutomation(TimelineData& data, quint64 trackId, const QString& key)
{
    TrackData* t = audioTrack(data, trackId);
    if (!t || !t->mix.automationKeys.values.contains(key))
        return false;
    t->mix.automationKeys.values.remove(key);
    return true;
}

} // namespace kk::mixerops
