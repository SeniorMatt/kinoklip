// SPDX-License-Identifier: GPL-3.0-or-later
// Typed accessors for preference keys stored in QSettings.
#include "core/Preferences.h"

#include <QSettings>

namespace cw::prefs {

namespace {
constexpr int kDefaultAutosaveMinutes = 2;

struct HwAccelName
{
    HwAccel mode;
    const char* id;
};

constexpr HwAccelName kHwAccelNames[] = {
    {HwAccel::Auto, "auto"},
    {HwAccel::Off, "off"},
    {HwAccel::Vaapi, "vaapi"},
    {HwAccel::Cuda, "cuda"},
    {HwAccel::Qsv, "qsv"},
    {HwAccel::Vulkan, "vulkan"},
};
} // namespace

QString hwAccelToString(HwAccel mode)
{
    for (const HwAccelName& n : kHwAccelNames) {
        if (n.mode == mode)
            return QLatin1String(n.id);
    }
    return QStringLiteral("auto");
}

HwAccel hwAccelFromString(const QString& id)
{
    for (const HwAccelName& n : kHwAccelNames) {
        if (id == QLatin1String(n.id))
            return n.mode;
    }
    return HwAccel::Auto;
}

QStringList hwAccelIds()
{
    QStringList ids;
    for (const HwAccelName& n : kHwAccelNames)
        ids.append(QLatin1String(n.id));
    return ids;
}

HwAccel hwAccel()
{
    return hwAccelFromString(QSettings().value(QLatin1String(kHwAccel)).toString());
}

void setHwAccel(HwAccel mode)
{
    QSettings().setValue(QLatin1String(kHwAccel), hwAccelToString(mode));
}

int autosaveMinutes()
{
    return QSettings().value(QLatin1String(kAutosaveMinutes), kDefaultAutosaveMinutes).toInt();
}

void setAutosaveMinutes(int minutes)
{
    QSettings().setValue(QLatin1String(kAutosaveMinutes), qMax(0, minutes));
}

} // namespace cw::prefs
