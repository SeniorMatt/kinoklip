// SPDX-License-Identifier: GPL-3.0-or-later
// EditBehaviour — QSettings lookup of the editing conventions.
#include "core/EditBehaviour.h"

#include <QSettings>

#include <algorithm>
#include <cmath>

namespace kk {

EditBehaviour EditBehaviour::fromSettings()
{
    const QSettings s;
    EditBehaviour b;
    auto real
        = [&s](const char* key, double def) { return s.value(QLatin1String(key), def).toDouble(); };
    auto flag
        = [&s](const char* key, bool def) { return s.value(QLatin1String(key), def).toBool(); };
    b.stillSeconds = real("edit/stillSeconds", b.stillSeconds);
    b.generatorSeconds = real("edit/generatorSeconds", b.generatorSeconds);
    b.transitionSeconds = real("edit/transitionSeconds", b.transitionSeconds);
    b.multiFrameNudge
        = std::max(1, s.value(QStringLiteral("edit/multiFrameNudge"), b.multiFrameNudge).toInt());
    b.threePointEditing = flag("edit/threePointEditing", b.threePointEditing);
    b.rippleDeleteGapAtPlayhead
        = flag("edit/rippleDeleteGapAtPlayhead", b.rippleDeleteGapAtPlayhead);
    b.trimToPlayheadRipples = flag("edit/trimToPlayheadRipples", b.trimToPlayheadRipples);
    b.selectToolRollsAtCut = flag("edit/selectToolRollsAtCut", b.selectToolRollsAtCut);
    b.trimToolSlideInNameBar = flag("edit/trimToolSlideInNameBar", b.trimToolSlideInNameBar);
    // Same key and values as prefs::kZoomAnchor / prefs::zoomAroundMouse().
    b.zoomAnchor = s.value(QStringLiteral("edit/zoomAnchor")).toString() == QLatin1String("mouse")
        ? ZoomAnchor::Cursor
        : ZoomAnchor::Playhead;
    b.wheelScheme
        = s.value(QStringLiteral("edit/wheelScheme")).toString() == QLatin1String("classic")
        ? WheelScheme::Classic
        : WheelScheme::Resolve;
    b.dynamicTrimWithJkl = flag("edit/dynamicTrimWithJkl", b.dynamicTrimWithJkl);
    return b;
}

int64_t EditBehaviour::frames(double seconds, Rational fps)
{
    // fps.num / fps.den frames per second; round to the nearest frame.
    const double f = seconds * double(fps.num) / double(fps.den);
    return std::max<int64_t>(1, int64_t(std::llround(f)));
}

} // namespace kk

namespace kk {

void EditBehaviour::setZoomAnchor(ZoomAnchor anchor)
{
    QSettings().setValue(QStringLiteral("edit/zoomAnchor"),
        anchor == ZoomAnchor::Cursor ? QStringLiteral("mouse") : QStringLiteral("playhead"));
}

void EditBehaviour::setWheelScheme(WheelScheme scheme)
{
    QSettings().setValue(QStringLiteral("edit/wheelScheme"),
        scheme == WheelScheme::Classic ? QStringLiteral("classic") : QStringLiteral("resolve"));
}

} // namespace kk
