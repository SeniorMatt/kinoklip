// SPDX-License-Identifier: GPL-3.0-or-later
// Well-known parameter keys of the built-in clip parameter sets (transform, crop, audio, title)
// and their documented defaults. Single source of truth for UI, renderer and serialization.
#pragma once

#include <QString>
#include <QStringList>
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
// ---- Composite (ClipData::transform; the Inspector's Composite group, spec 24.19) ----
inline const QString Opacity = QStringLiteral("opacity"); ///< percent 0..100; default 100
/// Static blend mode id of the clip over the tracks below (core/NodeRegistry.h blendmodes::;
/// absent = "normal").
inline const QString CompositeMode = QStringLiteral("compositeMode");
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
// ---- Dynamic Zoom (ClipData::transform statics; core/DynamicZoom.h) ----
inline const QString DynamicZoom = QStringLiteral("dynamicZoom"); ///< bool; default false
/// Ease of the move from the start to the end rectangle: "linear" (default), "easeIn",
/// "easeOut", "easeInOut".
inline const QString DynamicZoomEase = QStringLiteral("dynamicZoomEase");
/// Start / end rectangles: centre (fraction of the frame, 0.5 = middle) and width (fraction of
/// the frame; the height is the same fraction, the rectangle keeps the frame's aspect).
inline const QString DynamicZoomStartX = QStringLiteral("dynamicZoomStartX");
inline const QString DynamicZoomStartY = QStringLiteral("dynamicZoomStartY");
inline const QString DynamicZoomStartSize = QStringLiteral("dynamicZoomStartSize");
inline const QString DynamicZoomEndX = QStringLiteral("dynamicZoomEndX");
inline const QString DynamicZoomEndY = QStringLiteral("dynamicZoomEndY");
inline const QString DynamicZoomEndSize = QStringLiteral("dynamicZoomEndSize");

// ---- Audio (ClipData::audio, animated) ----
inline const QString VolumeDb
    = QStringLiteral("volumeDb"); ///< gain in dB (−96 = silence); default 0
inline const QString Pan = QStringLiteral("pan"); ///< −1 (left) .. +1 (right); default 0
/// Pitch (not animated): semitones −24 .. 24 plus cents −100 .. 100; the length stays.
inline const QString PitchSemitones = QStringLiteral("pitchSemitones");
inline const QString PitchCents = QStringLiteral("pitchCents");
/// Clip equalizer (static bool, off by default) and its four bands (not animated): band 1
/// low shelf, 2 and 3 bells, 4 high shelf; frequency in Hz, gain in dB, Q.
inline const QString EqEnabled = QStringLiteral("eqEnabled");
inline const QStringList EqBandFreq = {QStringLiteral("eq1Hz"), QStringLiteral("eq2Hz"),
    QStringLiteral("eq3Hz"), QStringLiteral("eq4Hz")};
inline const QStringList EqBandGain = {QStringLiteral("eq1Db"), QStringLiteral("eq2Db"),
    QStringLiteral("eq3Db"), QStringLiteral("eq4Db")};
inline const QStringList EqBandQ = {
    QStringLiteral("eq1Q"), QStringLiteral("eq2Q"), QStringLiteral("eq3Q"), QStringLiteral("eq4Q")};

// ---- Retime (ClipData::retime, animated; see core/Retime.h) ----
/// Source position (in source frames counted at the timeline frame rate, relative: only
/// differences matter) shown at a clip-local frame. Its keys are the speed points.
inline const QString RetimeFrame = QStringLiteral("retimeFrame");

// ---- Title (ClipData::title; Title clips only) ----
inline const QString Text
    = QStringLiteral("text"); ///< static QString; new titles "Basic Title", absent "Title"
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
/// Animated position of the text block in the frame, 0..1 from the left (X) and from the bottom
/// (Y), as Resolve's Text title; default 0.5 / 0.5 (centred, as titles were before).
inline const QString TextPosX = QStringLiteral("textPosX");
inline const QString TextPosY = QStringLiteral("textPosY");
/// static choice "left"|"center"|"right": which side of the text block sits at Position X
/// (a left lower third grows to the right); default center.
inline const QString Anchor = QStringLiteral("anchor");
/// static bool: the text rolls up through the frame over the clip's duration, from just
/// below the bottom edge to just above the top (rolling credits); Position Y is not used.
inline const QString Scroll = QStringLiteral("scroll");

/// Per layer (Text and Shape layers, spec section 68): rotation in degrees around the layer's
/// centre (animated; default 0) and the opacity of the whole layer in percent (animated;
/// default 100).
inline const QString LayerRotation = QStringLiteral("layerRotation");
inline const QString LayerOpacity = QStringLiteral("layerOpacity");

// ---- Shape layers (ClipData::titleLayers; the position uses TextPosX / TextPosY, the fill
// colour keys::Color, the stroke keys::OutlineColor / keys::OutlineWidth) ----
/// static choice "rect"|"rounded"|"ellipse"|"line"; default rect. A line is a bar Shape Width
/// long and Shape Height thick with round ends.
inline const QString ShapeType = QStringLiteral("shapeType");
inline const QString ShapeWidth = QStringLiteral("shapeWidth"); ///< animated px; default 400
inline const QString ShapeHeight = QStringLiteral("shapeHeight"); ///< animated px; default 200
/// animated px of the corners of a rounded rectangle; default 40
inline const QString CornerRadius = QStringLiteral("cornerRadius");
inline const QString FillOpacity = QStringLiteral("fillOpacity"); ///< animated percent; default 100

/// Documented default of an animated well-known key (see the comments above).
/// Unknown keys return 0.
[[nodiscard]] double defaultValue(QStringView key);

} // namespace kk::keys
