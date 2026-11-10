// SPDX-License-Identifier: GPL-3.0-or-later
// Default values of the well-known animated parameter keys.
#include "core/ParamKeys.h"

namespace kk::keys {

namespace {
struct KeyDefault
{
    const QString& key;
    double value;
};
} // namespace

double defaultValue(QStringView key)
{
    // Only keys whose default is not 0 need an entry. Function-local so the referenced key
    // constants are guaranteed to be initialised. Comparisons do not allocate.
    static const KeyDefault nonZeroDefaults[] = {
        {ZoomX, 1.0},
        {ZoomY, 1.0},
        {Opacity, 100.0},
        {Size, 72.0},
        {LineSpacing, 100.0},
        {BgOpacity, 60.0},
        {TextPosX, 0.5},
        {TextPosY, 0.5},
        {LayerOpacity, 100.0},
        {ShapeWidth, 400.0},
        {ShapeHeight, 200.0},
        {CornerRadius, 40.0},
        {FillOpacity, 100.0},
    };
    for (const KeyDefault& d : nonZeroDefaults) {
        if (key == d.key)
            return d.value;
    }
    return 0.0;
}

} // namespace kk::keys
