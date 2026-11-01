// SPDX-License-Identifier: GPL-3.0-or-later
// Default values of the well-known animated parameter keys.
#include "core/ParamKeys.h"

namespace cw::keys {

namespace {
struct KeyDefault
{
    const char* key;
    double value;
};

// Only keys whose default is not 0 need an entry.
constexpr KeyDefault kNonZeroDefaults[] = {
    {ZoomX, 1.0},
    {ZoomY, 1.0},
    {Opacity, 100.0},
    {Size, 72.0},
    {LineSpacing, 100.0},
    {BgOpacity, 60.0},
};
} // namespace

double defaultValue(QStringView key)
{
    for (const KeyDefault& d : kNonZeroDefaults) {
        if (key == QLatin1String(d.key))
            return d.value;
    }
    return 0.0;
}

} // namespace cw::keys
