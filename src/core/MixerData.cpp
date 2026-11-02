// SPDX-License-Identifier: GPL-3.0-or-later
// MixerData — defaults and string ids of the mixer model.
#include "core/MixerData.h"

#include <QCoreApplication>

namespace kk {

std::array<EqBand, kEqBandCount> defaultEqBands()
{
    constexpr double kButterworthQ = 0.7071; // maximally flat filters and shelves
    return {{
        {EqBandType::HighPass, false, 80.0, 0.0, kButterworthQ},
        {EqBandType::LowShelf, true, 100.0, 0.0, kButterworthQ},
        {EqBandType::Bell, true, 500.0, 0.0, 1.0},
        {EqBandType::Bell, true, 2000.0, 0.0, 1.0},
        {EqBandType::HighShelf, true, 8000.0, 0.0, kButterworthQ},
        {EqBandType::LowPass, false, 16000.0, 0.0, kButterworthQ},
    }};
}

QString busDisplayName(const BusData& bus)
{
    if (!bus.name.isEmpty())
        return bus.name;
    return QCoreApplication::translate("kk::Mixer", "Bus %1").arg(bus.id);
}

QString automationModeToString(AutomationMode mode)
{
    switch (mode) {
    case AutomationMode::Off:
        return QStringLiteral("off");
    case AutomationMode::Read:
        return QStringLiteral("read");
    case AutomationMode::Touch:
        return QStringLiteral("touch");
    case AutomationMode::Write:
        return QStringLiteral("write");
    }
    return QStringLiteral("read");
}

AutomationMode automationModeFromString(const QString& text, AutomationMode fallback)
{
    for (AutomationMode m :
        {AutomationMode::Off, AutomationMode::Read, AutomationMode::Touch, AutomationMode::Write}) {
        if (text == automationModeToString(m))
            return m;
    }
    return fallback;
}

QString eqBandTypeToString(EqBandType type)
{
    switch (type) {
    case EqBandType::HighPass:
        return QStringLiteral("highpass");
    case EqBandType::LowShelf:
        return QStringLiteral("lowshelf");
    case EqBandType::Bell:
        return QStringLiteral("bell");
    case EqBandType::Notch:
        return QStringLiteral("notch");
    case EqBandType::HighShelf:
        return QStringLiteral("highshelf");
    case EqBandType::LowPass:
        return QStringLiteral("lowpass");
    }
    return QStringLiteral("bell");
}

EqBandType eqBandTypeFromString(const QString& text, EqBandType fallback)
{
    for (EqBandType t : {EqBandType::HighPass, EqBandType::LowShelf, EqBandType::Bell,
             EqBandType::Notch, EqBandType::HighShelf, EqBandType::LowPass}) {
        if (text == eqBandTypeToString(t))
            return t;
    }
    return fallback;
}

bool eqBandHasGain(EqBandType type)
{
    return type == EqBandType::LowShelf || type == EqBandType::Bell
        || type == EqBandType::HighShelf;
}

} // namespace kk
