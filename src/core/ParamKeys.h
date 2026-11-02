// SPDX-License-Identifier: GPL-3.0-or-later
// Well-known parameter keys of the built-in clip parameter sets (transform, crop, audio, title)
// and their documented defaults. Single source of truth for UI, renderer and serialization.
#pragma once

#include <QString>
#include <QStringView>

namespace kk::keys {

// The constants below behave like enum values (qualified as keys::PosX), hence no k prefix.
// Units and defaults are part of the contract; the renderer and inspector rely on them.
//
// Performance: they are QStrings built with QStringLiteral (static data, no heap allocation),
// so `params.value(keys::PosX, frame)` performs no allocation — important because the
// compositor and audio mixer query parameters for every clip on every frame. A `const char*`
// constant would allocate a temporary QString per lookup.

// ---- Transform (ClipData::transform, animated) ----
inline const QString PosX = QStringLiteral("posX"); ///< px from frame centre, +right; default 0
inline const QString PosY = QStringLiteral("posY"); ///< px from frame centre, +down; default 0
inline const QString ZoomX = QStringLiteral("zoomX"); ///< scale factor; default 1.0
inline const QString ZoomY = QStringLiteral("zoomY"); ///< scale factor; default 1.0
inline const QString Rotation = QStringLiteral("rotation"); ///< degrees clockwise; default 0
inline const QString AnchorX
    = QStringLiteral("anchorX"); ///< px offset of the pivot from clip centre; default 0
inline const QString AnchorY
    = QStringLiteral("anchorY"); ///< px offset of the pivot from clip centre; default 0
inline const QString Opacity = QStringLiteral("opacity"); ///< percent 0..100; default 100
// ---- Cropping (ClipData::transform, animated) ----
inline const QString CropL
    = QStringLiteral("cropL"); ///< px cut from the left edge (source pixels); default 0
inline const QString CropR = QStringLiteral("cropR"); ///< px cut from the right edge; default 0
inline const QString CropT = QStringLiteral("cropT"); ///< px cut from the top edge; default 0
inline const QString CropB = QStringLiteral("cropB"); ///< px cut from the bottom edge; default 0
// ---- Transform statics ----
inline const QString FlipH = QStringLiteral("flipH"); ///< bool; default false
inline const QString FlipV = QStringLiteral("flipV"); ///< bool; default false
inline const QString ZoomLinked
    = QStringLiteral("zoomLinked"); ///< bool, UI keeps zoomX == zoomY; default true

// ---- Audio (ClipData::audio, animated) ----
inline const QString VolumeDb
    = QStringLiteral("volumeDb"); ///< gain in dB (−96 = silence); default 0
inline const QString Pan = QStringLiteral("pan"); ///< −1 (left) .. +1 (right); default 0

// ---- Retime (ClipData::retime, animated; see core/Retime.h) ----
/// Source position (in source frames counted at the timeline frame rate, relative: only
/// differences matter) shown at a clip-local frame. Its keys are the speed points.
inline const QString RetimeFrame = QStringLiteral("retimeFrame");

// ---- Title (ClipData::title; Title clips only) ----
inline const QString Text = QStringLiteral("text"); ///< static QString; default "Title"
inline const QString Font
    = QStringLiteral("font"); ///< static QString font family; default "Sans Serif"
inline const QString Bold = QStringLiteral("bold"); ///< static bool; default false
inline const QString Italic = QStringLiteral("italic"); ///< static bool; default false
inline const QString Align
    = QStringLiteral("align"); ///< static choice "left"|"center"|"right"; default center
inline const QString Color = QStringLiteral("color"); ///< static QColor text colour; default white
inline const QString OutlineColor
    = QStringLiteral("outlineColor"); ///< static QColor; default black
inline const QString BgColor
    = QStringLiteral("bgColor"); ///< static QColor background box; default black
inline const QString BgEnabled = QStringLiteral("bgEnabled"); ///< static bool; default false
inline const QString Shadow = QStringLiteral("shadow"); ///< static bool drop shadow; default false
inline const QString Size
    = QStringLiteral("size"); ///< animated font px at timeline res; default 72
inline const QString Tracking
    = QStringLiteral("tracking"); ///< animated percent letter spacing; default 0
inline const QString LineSpacing = QStringLiteral("lineSpacing"); ///< animated percent; default 100
inline const QString OutlineWidth = QStringLiteral("outlineWidth"); ///< animated px; default 0
inline const QString BgOpacity
    = QStringLiteral("bgOpacity"); ///< animated percent 0..100; default 60

/// Documented default of an animated well-known key (see the comments above).
/// Unknown keys return 0.
[[nodiscard]] double defaultValue(QStringView key);

} // namespace kk::keys
