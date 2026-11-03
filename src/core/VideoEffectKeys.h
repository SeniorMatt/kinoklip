// SPDX-License-Identifier: GPL-3.0-or-later
// VideoEffectKeys — descriptor ids and parameter keys of the video effects added in v4
// (keyers, colour, blurs, light, stylize, distort). Shared by the catalogue
// (core/BuiltinVideoEffects.cpp), the GPU passes and the CPU reference (src/render).
#pragma once

#include <QString>

namespace kk {

/// Descriptor ids (stable: stored in project files).
namespace fxid {
inline constexpr char ChromaKey[] = "chroma_key";
inline constexpr char LumaKey[] = "luma_key";
inline constexpr char HslKey[] = "hsl_key";
inline constexpr char Exposure[] = "exposure";
inline constexpr char TemperatureTint[] = "temperature_tint";
inline constexpr char Levels[] = "levels";
inline constexpr char Invert[] = "invert";
inline constexpr char BlackWhite[] = "black_white";
inline constexpr char Posterize[] = "posterize";
inline constexpr char ColorBalance[] = "color_balance";
inline constexpr char BoxBlur[] = "box_blur";
inline constexpr char DirectionalBlur[] = "directional_blur";
inline constexpr char RadialBlur[] = "radial_blur";
inline constexpr char ZoomBlur[] = "zoom_blur";
inline constexpr char Glow[] = "glow";
inline constexpr char FilmGrain[] = "film_grain";
inline constexpr char ChromaticAberration[] = "chromatic_aberration";
inline constexpr char EdgeDetect[] = "edge_detect";
inline constexpr char Emboss[] = "emboss";
inline constexpr char LensDistortion[] = "lens_distortion";
inline constexpr char Ripple[] = "ripple";
inline constexpr char Wave[] = "wave";
inline constexpr char FeatheredCrop[] = "feathered_crop";
inline constexpr char Stroke[] = "stroke";
} // namespace fxid

/// Parameter keys. QStringLiteral keeps the renderer's per-frame lookups allocation-free.
namespace fxkeys {
// Keyers
inline const QString Screen = QStringLiteral("screen"); ///< chroma key preset: green/blue/custom
inline const QString KeyColor = QStringLiteral("keyColor");
inline const QString Tolerance = QStringLiteral("tolerance");
inline const QString Softness = QStringLiteral("softness");
inline const QString ClipBlack = QStringLiteral("clipBlack");
inline const QString ClipWhite = QStringLiteral("clipWhite");
inline const QString ShrinkGrow = QStringLiteral("shrinkGrow");
inline const QString EdgeBlur = QStringLiteral("edgeBlur");
inline const QString SpillAmount = QStringLiteral("spillAmount");
inline const QString SpillMethod = QStringLiteral("spillMethod");
inline const QString Invert = QStringLiteral("invert");
inline const QString View = QStringLiteral("view"); ///< final / matte / status
inline const QString Low = QStringLiteral("low");
inline const QString High = QStringLiteral("high");
inline const QString Hue = QStringLiteral("hue");
inline const QString HueWidth = QStringLiteral("hueWidth");
inline const QString HueSoftness = QStringLiteral("hueSoftness");
inline const QString SatLow = QStringLiteral("satLow");
inline const QString SatHigh = QStringLiteral("satHigh");
inline const QString SatSoftness = QStringLiteral("satSoftness");
inline const QString LumLow = QStringLiteral("lumLow");
inline const QString LumHigh = QStringLiteral("lumHigh");
inline const QString LumSoftness = QStringLiteral("lumSoftness");
// Colour
inline const QString Exposure = QStringLiteral("exposure");
inline const QString Gamma = QStringLiteral("gamma");
inline const QString Temperature = QStringLiteral("temperature");
inline const QString Tint = QStringLiteral("tint");
inline const QString InBlack = QStringLiteral("inBlack");
inline const QString InWhite = QStringLiteral("inWhite");
inline const QString OutBlack = QStringLiteral("outBlack");
inline const QString OutWhite = QStringLiteral("outWhite");
inline const QString Amount = QStringLiteral("amount");
inline const QString TintColor = QStringLiteral("tintColor");
inline const QString TintAmount = QStringLiteral("tintAmount");
inline const QString Levels = QStringLiteral("levels");
inline const QString Lift = QStringLiteral("lift");
inline const QString LiftR = QStringLiteral("liftR");
inline const QString LiftG = QStringLiteral("liftG");
inline const QString LiftB = QStringLiteral("liftB");
inline const QString GammaR = QStringLiteral("gammaR");
inline const QString GammaG = QStringLiteral("gammaG");
inline const QString GammaB = QStringLiteral("gammaB");
inline const QString Gain = QStringLiteral("gain");
inline const QString GainR = QStringLiteral("gainR");
inline const QString GainG = QStringLiteral("gainG");
inline const QString GainB = QStringLiteral("gainB");
// Blur, light, stylize, distort, transform
inline const QString Radius = QStringLiteral("radius");
inline const QString Length = QStringLiteral("length");
inline const QString Angle = QStringLiteral("angle");
inline const QString CenterX = QStringLiteral("centerX");
inline const QString CenterY = QStringLiteral("centerY");
inline const QString Threshold = QStringLiteral("threshold");
inline const QString Intensity = QStringLiteral("intensity");
inline const QString Color = QStringLiteral("color");
inline const QString Size = QStringLiteral("size");
inline const QString Monochrome = QStringLiteral("monochrome");
inline const QString Animated = QStringLiteral("animated");
inline const QString Strength = QStringLiteral("strength");
inline const QString Height = QStringLiteral("height");
inline const QString Distortion = QStringLiteral("distortion");
inline const QString Zoom = QStringLiteral("zoom");
inline const QString Amplitude = QStringLiteral("amplitude");
inline const QString Wavelength = QStringLiteral("wavelength");
inline const QString Speed = QStringLiteral("speed");
inline const QString Phase = QStringLiteral("phase");
inline const QString Left = QStringLiteral("left");
inline const QString Right = QStringLiteral("right");
inline const QString Top = QStringLiteral("top");
inline const QString Bottom = QStringLiteral("bottom");
inline const QString Feather = QStringLiteral("feather");
inline const QString Width = QStringLiteral("width");
inline const QString Position = QStringLiteral("position");
inline const QString Opacity = QStringLiteral("opacity");
} // namespace fxkeys

/// Values of the Choice parameters above.
namespace fxchoice {
inline const QString Green = QStringLiteral("green");
inline const QString Blue = QStringLiteral("blue");
inline const QString Custom = QStringLiteral("custom");
inline const QString Average = QStringLiteral("average");
inline const QString Maximum = QStringLiteral("maximum");
inline const QString Final = QStringLiteral("final");
inline const QString Matte = QStringLiteral("matte");
inline const QString Status = QStringLiteral("status");
inline const QString Outside = QStringLiteral("outside");
inline const QString Inside = QStringLiteral("inside");
} // namespace fxchoice

} // namespace kk
