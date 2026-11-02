// SPDX-License-Identifier: GPL-3.0-or-later
// AudioScales — fader, meter and frequency axis maths.
#include "ui/audio/AudioScales.h"

#include "core/MixerData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace kk::audioscale {

namespace {

/// Fader marks: (dB, travel position), descending dB.
constexpr std::array<std::pair<double, double>, 10> kFaderMarks {{
    {12.0, 1.0},
    {6.0, 0.89},
    {0.0, 0.76},
    {-6.0, 0.63},
    {-12.0, 0.51},
    {-20.0, 0.38},
    {-30.0, 0.26},
    {-40.0, 0.16},
    {-60.0, 0.06},
    {kFaderMinDb, 0.0},
}};

constexpr double kMinHz = 20.0;
constexpr double kMaxHz = 20000.0;

} // namespace

double faderPosition(double db)
{
    if (db >= kFaderMarks.front().first)
        return 1.0;
    for (size_t i = 1; i < kFaderMarks.size(); ++i) {
        const auto [hiDb, hiPos] = kFaderMarks[i - 1];
        const auto [loDb, loPos] = kFaderMarks[i];
        if (db >= loDb)
            return loPos + (db - loDb) / (hiDb - loDb) * (hiPos - loPos);
    }
    return 0.0;
}

double faderDb(double position)
{
    const double p = std::clamp(position, 0.0, 1.0);
    for (size_t i = 1; i < kFaderMarks.size(); ++i) {
        const auto [hiDb, hiPos] = kFaderMarks[i - 1];
        const auto [loDb, loPos] = kFaderMarks[i];
        if (p >= loPos)
            return loDb + (p - loPos) / (hiPos - loPos) * (hiDb - loDb);
    }
    return kFaderMinDb;
}

double meterPosition(double db)
{
    return std::clamp((db - kMeterFloorDb) / -kMeterFloorDb, 0.0, 1.0);
}

double toDb(double linear)
{
    return linear > 1e-10 ? 20.0 * std::log10(linear) : -200.0;
}

double frequencyPosition(double hz)
{
    return std::log(std::clamp(hz, kMinHz, kMaxHz) / kMinHz) / std::log(kMaxHz / kMinHz);
}

double positionFrequency(double x)
{
    return kMinHz * std::pow(kMaxHz / kMinHz, std::clamp(x, 0.0, 1.0));
}

} // namespace kk::audioscale
