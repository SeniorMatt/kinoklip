// SPDX-License-Identifier: GPL-3.0-or-later
// Well-known parameter keys of the built-in clip parameter sets (transform, crop, audio, title)
// and their documented defaults. Single source of truth for UI, renderer and serialization.
#pragma once

#include <QString>
#include <QStringView>

namespace cw::keys {

// The constants below behave like enum values (qualified as keys::PosX), hence no k prefix.
// Units and defaults are part of the contract; the renderer and inspector rely on them.

// ---- Transform (ClipData::transform, animated) ----
inline constexpr char PosX[] = "posX"; ///< px from frame centre, +right; default 0
inline constexpr char PosY[] = "posY"; ///< px from frame centre, +down; default 0
inline constexpr char ZoomX[] = "zoomX"; ///< scale factor; default 1.0
inline constexpr char ZoomY[] = "zoomY"; ///< scale factor; default 1.0
inline constexpr char Rotation[] = "rotation"; ///< degrees clockwise; default 0
inline constexpr char AnchorX[] = "anchorX"; ///< px offset of the pivot from clip centre; default 0
inline constexpr char AnchorY[] = "anchorY"; ///< px offset of the pivot from clip centre; default 0
inline constexpr char Opacity[] = "opacity"; ///< percent 0..100; default 100
// ---- Cropping (ClipData::transform, animated) ----
inline constexpr char CropL[] = "cropL"; ///< px cut from the left edge (source pixels); default 0
inline constexpr char CropR[] = "cropR"; ///< px cut from the right edge; default 0
inline constexpr char CropT[] = "cropT"; ///< px cut from the top edge; default 0
inline constexpr char CropB[] = "cropB"; ///< px cut from the bottom edge; default 0
// ---- Transform statics ----
inline constexpr char FlipH[] = "flipH"; ///< bool; default false
inline constexpr char FlipV[] = "flipV"; ///< bool; default false
inline constexpr char ZoomLinked[] = "zoomLinked"; ///< bool, UI keeps zoomX == zoomY; default true

// ---- Audio (ClipData::audio, animated) ----
inline constexpr char VolumeDb[] = "volumeDb"; ///< gain in dB (−96 = silence); default 0
inline constexpr char Pan[] = "pan"; ///< −1 (left) .. +1 (right); default 0

// ---- Title (ClipData::title; Title clips only) ----
inline constexpr char Text[] = "text"; ///< static QString; default "Title"
inline constexpr char Font[] = "font"; ///< static QString font family; default "Sans Serif"
inline constexpr char Bold[] = "bold"; ///< static bool; default false
inline constexpr char Italic[] = "italic"; ///< static bool; default false
inline constexpr char Align[] = "align"; ///< static choice "left"|"center"|"right"; default center
inline constexpr char Color[] = "color"; ///< static QColor text colour; default white
inline constexpr char OutlineColor[] = "outlineColor"; ///< static QColor; default black
inline constexpr char BgColor[] = "bgColor"; ///< static QColor background box; default black
inline constexpr char BgEnabled[] = "bgEnabled"; ///< static bool; default false
inline constexpr char Shadow[] = "shadow"; ///< static bool drop shadow; default false
inline constexpr char Size[] = "size"; ///< animated font px at timeline res; default 72
inline constexpr char Tracking[] = "tracking"; ///< animated percent letter spacing; default 0
inline constexpr char LineSpacing[] = "lineSpacing"; ///< animated percent; default 100
inline constexpr char OutlineWidth[] = "outlineWidth"; ///< animated px; default 0
inline constexpr char BgOpacity[] = "bgOpacity"; ///< animated percent 0..100; default 60

/// Documented default of an animated well-known key (see the comments above).
/// Unknown keys return 0.
[[nodiscard]] double defaultValue(QStringView key);

} // namespace cw::keys
