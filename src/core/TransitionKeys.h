// SPDX-License-Identifier: GPL-3.0-or-later
// TransitionKeys — parameter keys of the video transitions in the Effects library folders
// (BuiltinTransitions.cpp describes them, src/render/Gl*Transitions.cpp draws them). The older
// four (cross dissolve, dip to color, wipe, push) keep their own keys.
#pragma once

#include <QString>

namespace kk::tkeys {

// Edge set shared by the iris, shape and wipe transitions.
inline const QString Color = QStringLiteral("color"); ///< border colour
inline const QString Border = QStringLiteral("border"); ///< border width, px
inline const QString Feather = QStringLiteral("feather"); ///< edge softness, %
inline const QString Reverse = QStringLiteral("reverse"); ///< run the other way round

// Dissolves.
inline const QString StartRatio = QStringLiteral("startRatio"); ///< % of B at the start
inline const QString EndRatio = QStringLiteral("endRatio"); ///< % of B at the end
inline const QString BlurH = QStringLiteral("blurH"); ///< Blur Dissolve, % of the width
inline const QString BlurV = QStringLiteral("blurV"); ///< Blur Dissolve, % of the height

// Shapes and wipes.
inline const QString Preset = QStringLiteral("preset");
inline const QString Aspect = QStringLiteral("aspect"); ///< shape width : height
inline const QString Rotation = QStringLiteral("rotation"); ///< degrees clockwise
inline const QString OffsetX = QStringLiteral("offsetX"); ///< centre offset, % of the width
inline const QString OffsetY = QStringLiteral("offsetY"); ///< centre offset, % of the height
inline const QString Angle = QStringLiteral("angle"); ///< degrees
inline const QString Clockwise = QStringLiteral("clockwise");
inline const QString Repeat = QStringLiteral("repeat"); ///< slats / bands
inline const QString Mode = QStringLiteral("mode");

// Fusion and Resolve FX style transitions.
inline const QString Brightness = QStringLiteral("brightness"); ///< %
inline const QString Zoom = QStringLiteral("zoom"); ///< %
inline const QString Strength = QStringLiteral("strength"); ///< %
inline const QString Speed = QStringLiteral("speed");
inline const QString Size = QStringLiteral("size"); ///< px or %, see the descriptor
inline const QString Softness = QStringLiteral("softness"); ///< %
inline const QString Count = QStringLiteral("count"); ///< rings, slices
inline const QString Glow = QStringLiteral("glow"); ///< %
inline const QString Raggedness = QStringLiteral("raggedness"); ///< %
inline const QString Char = QStringLiteral("char"); ///< %

// Fusion style motion and reveal transitions (Slide, Pan, Fall and Bounce, Film Strip, ...).
inline const QString Curve = QStringLiteral("curve"); ///< "linear", "easing" or "custom"
inline const QString Power = QStringLiteral("power"); ///< steepness of the custom curve
inline const QString Invert = QStringLiteral("invert"); ///< Foreground Wipe: run the other way
inline const QString Shadow = QStringLiteral("shadow"); ///< Paint On: shadow at the brush edge, %

// Motion.
inline const QString Direction = QStringLiteral("direction");
inline const QString MotionBlur = QStringLiteral("motionBlur"); ///< shutter, % of the transition

} // namespace kk::tkeys
