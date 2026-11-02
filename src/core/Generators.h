// SPDX-License-Identifier: GPL-3.0-or-later
// Generators — descriptor ids and parameter keys of the built-in generators (Effects library >
// Generators) and of the adjustment clip. Shared by the descriptor catalogue
// (BuiltinEffects.cpp), the renderers (src/render/Generators.*) and the UI.
#pragma once

#include <QString>

namespace kk::generators {

// The constants behave like enum values (generators::Noise), hence no k prefix. Ids are
// stored in project files (ClipData::generator): never change them.

// ---- Descriptor ids ----
inline const QString SolidColor = QStringLiteral("solid_color"); ///< ClipKind::Solid
inline const QString FourColorGradient = QStringLiteral("four_color_gradient");
inline const QString LinearGradient = QStringLiteral("linear_gradient");
inline const QString RadialGradient = QStringLiteral("radial_gradient");
inline const QString ColorBars = QStringLiteral("color_bars");
inline const QString GreyScale = QStringLiteral("grey_scale");
inline const QString Noise = QStringLiteral("noise");
inline const QString Countdown = QStringLiteral("countdown");
inline const QString Timecode = QStringLiteral("timecode_burn_in");
/// Effects library > Effects: creates a ClipKind::Adjustment clip (no parameters of its own).
inline const QString AdjustmentClip = QStringLiteral("adjustment_clip");

// ---- Parameter keys (ClipData::title of a Generator clip). Colours are statics, numbers
//      animatable unless noted. Pixel values are timeline pixels. ----
inline const QString Color1 = QStringLiteral("color1"); ///< first / inner / top-left colour
inline const QString Color2 = QStringLiteral("color2"); ///< second / outer / top-right colour
inline const QString Color3 = QStringLiteral("color3"); ///< bottom-left colour (4-colour)
inline const QString Color4 = QStringLiteral("color4"); ///< bottom-right colour (4-colour)
inline const QString CenterX = QStringLiteral("centerX"); ///< px from the frame centre, +right
inline const QString CenterY = QStringLiteral("centerY"); ///< px from the frame centre, +down
inline const QString Angle = QStringLiteral("angle"); ///< degrees clockwise, 0 = left to right
inline const QString Length = QStringLiteral("length"); ///< linear gradient length, px
inline const QString Radius = QStringLiteral("radius"); ///< radial gradient radius, px
inline const QString Style = QStringLiteral("style"); ///< colour bars: "smpte" | "full"
inline const QString Steps = QStringLiteral("steps"); ///< grey scale steps (0 = smooth ramp)
inline const QString Orientation = QStringLiteral("orientation"); ///< "horizontal" | "vertical"
inline const QString Monochrome = QStringLiteral("monochrome"); ///< bool
inline const QString Brightness = QStringLiteral("brightness"); ///< percent, mean level
inline const QString Amount = QStringLiteral("amount"); ///< percent, noise strength
inline const QString Grain = QStringLiteral("grain"); ///< px per noise cell
inline const QString Seed = QStringLiteral("seed"); ///< int (static)
inline const QString Animated = QStringLiteral("animated"); ///< bool: new noise every frame
inline const QString StartSeconds
    = QStringLiteral("startSeconds"); ///< countdown start (0 = clip length)
inline const QString Background = QStringLiteral("background"); ///< colour
inline const QString Accent = QStringLiteral("accent"); ///< countdown sweep colour
inline const QString Source = QStringLiteral("source"); ///< timecode: "timeline" | "clip"
inline const QString Prefix = QStringLiteral("prefix"); ///< timecode text before the value
/// timecode: percent of the frame height the text sits below the centre
inline const QString OffsetY = QStringLiteral("offsetY");
inline const QString StartFrame
    = QStringLiteral("startFrame"); ///< timecode value shown at frame 0, frames (static int)

} // namespace kk::generators
