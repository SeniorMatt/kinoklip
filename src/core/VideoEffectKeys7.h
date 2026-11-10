// SPDX-License-Identifier: GPL-3.0-or-later
// VideoEffectKeys7 — ids, parameter keys and Choice values of the tracking effects of the 1.2 gap
// list (docs/behavior-spec.md section 70): Match Move, Surface Tracker and Warper. Included at the
// end of VideoEffectKeys.h, so users of that header see these too.
#pragma once

#include <QString>

namespace kk {

namespace fxid {
inline constexpr char MatchMove[] = "match_move";
inline constexpr char SurfaceTracker[] = "surface_tracker";
inline constexpr char Warper[] = "warper";
} // namespace fxid

namespace fxkeys {
// Tracking effects (Match Move, Surface Tracker)
inline const QString TrackSource = QStringLiteral("trackSource");
inline const QString TrackKind = QStringLiteral("trackKind");
inline const QString TrackDirection = QStringLiteral("trackDirection");
inline const QString RegionSize = QStringLiteral("regionSize"); ///< percent of the picture width
inline const QString GridDensity = QStringLiteral("gridDensity"); ///< points per side
inline const QString SurfaceMode = QStringLiteral("surfaceMode");
inline const QString TrackData = QStringLiteral("trackData"); ///< hidden: core/EffectTrack.h
} // namespace fxkeys

namespace fxchoice {
inline const QString TrackClip = QStringLiteral("clip");
inline const QString TrackBelow = QStringLiteral("below");
inline const QString KindPosition = QStringLiteral("position");
inline const QString KindPlanar = QStringLiteral("planar");
inline const QString DirectionFollow = QStringLiteral("follow");
inline const QString DirectionLock = QStringLiteral("lock");
inline const QString SurfacePin = QStringLiteral("pin");
inline const QString SurfaceWarp = QStringLiteral("warp");
} // namespace fxchoice

/// Warper: a grid of control points the user moves (columns x rows, 2 - kMaxSide per side). Point
/// (row, column) has two keyframable parameters, its movement in px on x and y; row 0 is at the top.
namespace warper {
inline constexpr int kDefaultSide = 3;
inline constexpr int kMaxSide = 4;
[[nodiscard]] inline QString pointKey(int row, int column, int axis)
{
    return QStringLiteral("point%1%2%3").arg(row).arg(column).arg(axis == 0 ? 'X' : 'Y');
}
} // namespace warper

} // namespace kk
