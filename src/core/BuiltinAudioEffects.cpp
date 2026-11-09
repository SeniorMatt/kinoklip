// SPDX-License-Identifier: GPL-3.0-or-later
// The clip audio effects catalogue (EffectCategory::AudioEffect). The processors are in
// src/render/AudioClipEffects.cpp, keyed by these ids.
#include "core/AudioEffectKeys.h"
#include "core/EffectBuilders.h"
#include "core/EffectRegistry.h"
#include "core/MixerData.h"

#include <QCoreApplication>

namespace kk {

using namespace builders;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::BuiltinAudioEffects", text);
}

const QString kDb = QStringLiteral("dB");
const QString kHz = QStringLiteral("Hz");
const QString kMs = QStringLiteral("ms");
const QString kPercent = QStringLiteral("%");

/// A plain (not keyframable) number: the processors read one value per block.
ParamDescriptor value(const char* key, const QString& label, double min, double max, double def,
    double step, const QString& unit = {})
{
    ParamDescriptor p = number(QLatin1String(key), label, min, max, def, step, unit);
    p.animatable = false;
    return p;
}

EffectDescriptor effect(const char* id, const QString& name, const QString& description,
    QVector<ParamDescriptor> params)
{
    return descriptor(
        QLatin1String(id), name, EffectCategory::AudioEffect, description, std::move(params));
}

} // namespace

void EffectRegistry::registerAudioEffects()
{
    using namespace afxid;
    namespace k = afxkeys;
    add(effect(Gain, tr("Gain"), tr("Raise or lower the level of the clip"),
        {value(k::GainDb, tr("Gain"), -60, 24, 0, 0.1, kDb)}));
    add(effect(Eq, tr("Equalizer"), tr("Low shelf, mid bell and high shelf"),
        {
            value(k::LowGainDb, tr("Low Gain"), -kEqMaxGainDb, kEqMaxGainDb, 0, 0.1, kDb),
            value(k::LowFreqHz, tr("Low Frequency"), kEqMinFrequencyHz, 1000, 100, 1, kHz),
            value(k::MidGainDb, tr("Mid Gain"), -kEqMaxGainDb, kEqMaxGainDb, 0, 0.1, kDb),
            value(k::MidFreqHz, tr("Mid Frequency"), 100, kEqMaxFrequencyHz, 1000, 1, kHz),
            value(k::MidQ, tr("Mid Q"), kEqMinQ, kEqMaxQ, 1, 0.1),
            value(k::HighGainDb, tr("High Gain"), -kEqMaxGainDb, kEqMaxGainDb, 0, 0.1, kDb),
            value(k::HighFreqHz, tr("High Frequency"), 1000, kEqMaxFrequencyHz, 8000, 1, kHz),
        }));
    add(effect(Compressor, tr("Compressor"), tr("Reduce the level of loud parts"),
        {
            value(k::ThresholdDb, tr("Threshold"), kDynMinThresholdDb, 0, -20, 0.1, kDb),
            value(k::Ratio, tr("Ratio"), 1, kDynMaxRatio, 4, 0.1),
            value(k::AttackMs, tr("Attack"), kDynMinTimeMs, kDynMaxAttackMs, 5, 0.1, kMs),
            value(k::ReleaseMs, tr("Release"), kDynMinTimeMs, kDynMaxReleaseMs, 120, 1, kMs),
            value(k::MakeupDb, tr("Make-up Gain"), 0, kDynMaxMakeupDb, 0, 0.1, kDb),
        }));
    add(effect(Limiter, tr("Limiter"), tr("Never let a sample go above the ceiling"),
        {
            value(k::CeilingDb, tr("Ceiling"), kLimiterMinCeilingDb, 0, -1, 0.1, kDb),
            value(k::ReleaseMs, tr("Release"), 1, kDynMaxReleaseMs, 50, 1, kMs),
        }));
    add(effect(HighPass, tr("High-pass Filter"), tr("Remove low frequencies"),
        {value(k::FrequencyHz, tr("Frequency"), kEqMinFrequencyHz, 2000, 80, 1, kHz)}));
    add(effect(LowPass, tr("Low-pass Filter"), tr("Remove high frequencies"),
        {value(k::FrequencyHz, tr("Frequency"), 200, kEqMaxFrequencyHz, 12000, 1, kHz)}));
    add(effect(Delay, tr("Delay"), tr("Echoes of the sound"),
        {
            value(k::TimeMs, tr("Time"), 1, kAudioDelayMaxMs, 250, 1, kMs),
            value(k::Feedback, tr("Feedback"), 0, 95, 35, 1, kPercent),
            value(k::Mix, tr("Mix"), 0, 100, 30, 1, kPercent),
        }));
    add(effect(Reverb, tr("Reverb"), tr("A simple room: the sound dies away slowly"),
        {
            value(k::RoomSize, tr("Room Size"), 0, 100, 50, 1, kPercent),
            value(k::Damping, tr("Damping"), 0, 100, 50, 1, kPercent),
            value(k::Mix, tr("Mix"), 0, 100, 25, 1, kPercent),
        }));
}

} // namespace kk
