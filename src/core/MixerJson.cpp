// SPDX-License-Identifier: GPL-3.0-or-later
// MixerJson — (de)serialization of channel strips, track mixer settings and buses.
#include "core/MixerJson.h"

#include "core/MixerOps.h"
#include "core/Serialization.h"
#include "core/TimelineData.h"

#include <QJsonArray>

#include <algorithm>

namespace kk::serialization {

namespace {

QLatin1String key(const char* name)
{
    return QLatin1String(name);
}

/// Writes @p value under @p name when it differs from @p def.
void putIf(QJsonObject& o, const char* name, double value, double def)
{
    if (value != def)
        o[key(name)] = value;
}

void putIf(QJsonObject& o, const char* name, bool value, bool def)
{
    if (value != def)
        o[key(name)] = value;
}

QJsonObject bandToJson(const EqBand& b)
{
    return QJsonObject {{key("type"), eqBandTypeToString(b.type)}, {key("on"), b.enabled},
        {key("freq"), b.frequencyHz}, {key("gain"), b.gainDb}, {key("q"), b.q}};
}

EqBand bandFromJson(const QJsonObject& o, const EqBand& def)
{
    EqBand b;
    b.type = eqBandTypeFromString(o.value(key("type")).toString(), def.type);
    b.enabled = o.value(key("on")).toBool(def.enabled);
    b.frequencyHz = o.value(key("freq")).toDouble(def.frequencyHz);
    b.gainDb = o.value(key("gain")).toDouble(def.gainDb);
    b.q = o.value(key("q")).toDouble(def.q);
    return b;
}

} // namespace

QJsonObject stripToJson(const ChannelStrip& s)
{
    const ChannelStrip def;
    QJsonObject o;
    if (s.eq != def.eq) {
        QJsonObject eq;
        eq[key("on")] = s.eq.enabled;
        QJsonArray bands;
        for (const EqBand& b : s.eq.bands)
            bands.append(bandToJson(b));
        eq[key("bands")] = bands;
        o[key("eq")] = eq;
    }
    if (s.dynamics != def.dynamics) {
        const DynamicsSettings& d = s.dynamics;
        const DynamicsSettings dd;
        QJsonObject dyn;
        putIf(dyn, "compressor", d.compressorEnabled, dd.compressorEnabled);
        putIf(dyn, "threshold", d.thresholdDb, dd.thresholdDb);
        putIf(dyn, "ratio", d.ratio, dd.ratio);
        putIf(dyn, "attack", d.attackMs, dd.attackMs);
        putIf(dyn, "release", d.releaseMs, dd.releaseMs);
        putIf(dyn, "makeup", d.makeupDb, dd.makeupDb);
        putIf(dyn, "knee", d.kneeDb, dd.kneeDb);
        putIf(dyn, "limiter", d.limiterEnabled, dd.limiterEnabled);
        putIf(dyn, "ceiling", d.limiterCeilingDb, dd.limiterCeilingDb);
        putIf(dyn, "limiterRelease", d.limiterReleaseMs, dd.limiterReleaseMs);
        o[key("dynamics")] = dyn;
    }
    putIf(o, "volume", s.volumeDb, def.volumeDb);
    putIf(o, "pan", s.pan, def.pan);
    putIf(o, "muted", s.muted, def.muted);
    return o;
}

ChannelStrip stripFromJson(const QJsonObject& o)
{
    ChannelStrip s;
    if (o.contains(key("eq"))) {
        const QJsonObject eq = o.value(key("eq")).toObject();
        s.eq.enabled = eq.value(key("on")).toBool(false);
        const QJsonArray bands = eq.value(key("bands")).toArray();
        for (int i = 0; i < kEqBandCount && i < bands.size(); ++i)
            s.eq.bands[size_t(i)] = bandFromJson(bands.at(i).toObject(), s.eq.bands[size_t(i)]);
    }
    if (o.contains(key("dynamics"))) {
        const QJsonObject dyn = o.value(key("dynamics")).toObject();
        DynamicsSettings& d = s.dynamics;
        d.compressorEnabled = dyn.value(key("compressor")).toBool(d.compressorEnabled);
        d.thresholdDb = dyn.value(key("threshold")).toDouble(d.thresholdDb);
        d.ratio = dyn.value(key("ratio")).toDouble(d.ratio);
        d.attackMs = dyn.value(key("attack")).toDouble(d.attackMs);
        d.releaseMs = dyn.value(key("release")).toDouble(d.releaseMs);
        d.makeupDb = dyn.value(key("makeup")).toDouble(d.makeupDb);
        d.kneeDb = dyn.value(key("knee")).toDouble(d.kneeDb);
        d.limiterEnabled = dyn.value(key("limiter")).toBool(d.limiterEnabled);
        d.limiterCeilingDb = dyn.value(key("ceiling")).toDouble(d.limiterCeilingDb);
        d.limiterReleaseMs = dyn.value(key("limiterRelease")).toDouble(d.limiterReleaseMs);
    }
    s.volumeDb = o.value(key("volume")).toDouble(s.volumeDb);
    s.pan = o.value(key("pan")).toDouble(s.pan);
    s.muted = o.value(key("muted")).toBool(s.muted);
    mixerops::clampStrip(s);
    return s;
}

QJsonObject trackMixToJson(const TrackMix& mix)
{
    const TrackMix def;
    QJsonObject o;
    if (mix.strip != def.strip)
        o[key("strip")] = stripToJson(mix.strip);
    putIf(o, "mono", mix.mono, def.mono);
    if (mix.buses != def.buses) {
        QJsonArray buses;
        for (int id : mix.buses)
            buses.append(id);
        o[key("buses")] = buses;
    }
    if (mix.automation != def.automation)
        o[key("automation")] = automationModeToString(mix.automation);
    if (mix.automationKeys != ParamSet {})
        o[key("automationKeys")] = paramSetToJson(mix.automationKeys);
    return o;
}

TrackMix trackMixFromJson(const QJsonObject& o)
{
    TrackMix mix;
    mix.strip = stripFromJson(o.value(key("strip")).toObject());
    mix.mono = o.value(key("mono")).toBool(false);
    if (o.contains(key("buses"))) {
        mix.buses.clear();
        for (const QJsonValue& v : o.value(key("buses")).toArray()) {
            const int id = v.toInt(0);
            if (id > 0 && !mix.buses.contains(id))
                mix.buses.append(id);
        }
        std::sort(mix.buses.begin(), mix.buses.end());
    }
    mix.automation
        = automationModeFromString(o.value(key("automation")).toString(), mix.automation);
    mix.automationKeys = paramSetFromJson(o.value(key("automationKeys")).toObject());
    return mix;
}

QJsonObject mixerToJson(const MixerData& mixer)
{
    if (mixer == MixerData {})
        return {};
    QJsonObject o;
    QJsonArray buses;
    for (const BusData& b : mixer.buses) {
        QJsonObject bo {{key("id"), b.id}};
        if (!b.name.isEmpty())
            bo[key("name")] = b.name;
        putIf(bo, "mono", b.mono, false);
        if (b.strip != ChannelStrip {})
            bo[key("strip")] = stripToJson(b.strip);
        buses.append(bo);
    }
    o[key("buses")] = buses;
    o[key("output")] = mixer.outputBus;
    o[key("nextBusId")] = mixer.nextBusId;
    return o;
}

MixerData mixerFromJson(const QJsonObject& o)
{
    MixerData mixer;
    if (o.isEmpty())
        return mixer;
    mixer.buses.clear();
    for (const QJsonValue& v : o.value(key("buses")).toArray()) {
        const QJsonObject bo = v.toObject();
        BusData b;
        b.id = bo.value(key("id")).toInt(0);
        if (b.id <= 0 || mixerops::busIndex(mixer, b.id) >= 0)
            continue; // invalid or duplicate
        b.name = bo.value(key("name")).toString();
        b.mono = bo.value(key("mono")).toBool(false);
        b.strip = stripFromJson(bo.value(key("strip")).toObject());
        mixer.buses.push_back(b);
    }
    if (mixerops::busIndex(mixer, kMainBusId) < 0)
        mixer.buses.insert(mixer.buses.begin(), BusData {});
    // The main bus first, the others in their stored order.
    std::stable_partition(mixer.buses.begin(), mixer.buses.end(),
        [](const BusData& b) { return b.id == kMainBusId; });
    mixer.outputBus = o.value(key("output")).toInt(kMainBusId);
    if (mixerops::busIndex(mixer, mixer.outputBus) < 0)
        mixer.outputBus = kMainBusId;
    int maxId = kMainBusId;
    for (const BusData& b : mixer.buses)
        maxId = std::max(maxId, b.id);
    mixer.nextBusId = std::max(o.value(key("nextBusId")).toInt(maxId + 1), maxId + 1);
    return mixer;
}

void repairRoutes(const MixerData& mixer, std::vector<TrackData>& tracks)
{
    for (TrackData& t : tracks) {
        t.mix.buses.erase(std::remove_if(t.mix.buses.begin(), t.mix.buses.end(),
                              [&mixer](int id) { return mixerops::busIndex(mixer, id) < 0; }),
            t.mix.buses.end());
    }
}

} // namespace kk::serialization
