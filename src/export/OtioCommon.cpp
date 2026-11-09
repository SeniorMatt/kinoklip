// SPDX-License-Identifier: GPL-3.0-or-later
// OtioCommon — frame rates and marker colours shared by the .otio reader and writer.
#include "export/Otio.h"

#include <QRgb>

#include <climits>
#include <cmath>

namespace kk::otio {

namespace {

/// The colours OpenTimelineIO names for markers (otio.schema.MarkerColor). Kinoklip's palette
/// supplies the shades it has; orange, black and white are its own values.
struct OtioColor
{
    const char* name;
    QRgb rgb;
};
constexpr OtioColor kColors[] = {
    {"PINK", qRgb(0xe8, 0x7a, 0xb4)},
    {"RED", qRgb(0xe0, 0x4a, 0x4a)},
    {"ORANGE", qRgb(0xe8, 0x8c, 0x3c)},
    {"YELLOW", qRgb(0xe8, 0xc8, 0x3c)},
    {"GREEN", qRgb(0x4c, 0xb8, 0x4c)},
    {"CYAN", qRgb(0x3f, 0xc8, 0xd8)},
    {"BLUE", qRgb(0x4c, 0x9b, 0xe8)},
    {"PURPLE", qRgb(0x9a, 0x6a, 0xd8)},
    {"MAGENTA", qRgb(0xc8, 0x44, 0xb4)},
    {"BLACK", qRgb(0x20, 0x20, 0x20)},
    {"WHITE", qRgb(0xf0, 0xf0, 0xf0)},
};

/// Largest relative distance from a whole frame rate at which a rate counts as NTSC.
constexpr double kNtscTolerance = 2e-3;

} // namespace

Rational rateToRational(double rate)
{
    if (!(rate > 0.0) || rate > 1e5)
        return {0, 1}; // not a usable rate: isValid() is false
    const double whole = std::round(rate);
    if (std::abs(rate - whole) < 1e-6)
        return {int(whole), 1};
    // NTSC family: 23.976, 29.97, 47.952, 59.94, 119.88 are n * 1000 / 1001.
    const double ntscBase = std::round(rate * 1001.0 / 1000.0);
    if (ntscBase >= 1.0 && std::abs(rate - ntscBase * 1000.0 / 1001.0) < ntscBase * kNtscTolerance)
        return {int(ntscBase) * 1000, 1001};
    return {int(std::llround(rate * 1000.0)), 1000};
}

QString markerColorToOtio(const QColor& color)
{
    const OtioColor* best = &kColors[0];
    int bestDistance = INT_MAX;
    for (const OtioColor& c : kColors) {
        const int dr = qRed(c.rgb) - color.red();
        const int dg = qGreen(c.rgb) - color.green();
        const int db = qBlue(c.rgb) - color.blue();
        const int d = dr * dr + dg * dg + db * db;
        if (d < bestDistance) {
            bestDistance = d;
            best = &c;
        }
    }
    return QLatin1String(best->name);
}

QColor markerColorFromOtio(const QString& name)
{
    for (const OtioColor& c : kColors) {
        if (name.compare(QLatin1String(c.name), Qt::CaseInsensitive) == 0)
            return QColor(c.rgb);
    }
    return {};
}

} // namespace kk::otio
