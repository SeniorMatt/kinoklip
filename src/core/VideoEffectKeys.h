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
// Batch 2 (BuiltinVideoEffects2.cpp)
inline constexpr char Curves[] = "curves";
inline constexpr char HslAdjust[] = "hsl_adjust";
inline constexpr char ApplyLut[] = "apply_lut";
inline constexpr char Vibrance[] = "vibrance";
inline constexpr char ChannelMixer[] = "channel_mixer";
inline constexpr char UnsharpMask[] = "unsharp_mask";
inline constexpr char TiltShift[] = "tilt_shift";
inline constexpr char LensBlur[] = "lens_blur";
inline constexpr char NoiseReduction[] = "noise_reduction";
inline constexpr char LightRays[] = "light_rays";
inline constexpr char Bloom[] = "bloom";
inline constexpr char Halation[] = "halation";
inline constexpr char LensFlare[] = "lens_flare";
inline constexpr char Pixelate[] = "pixelate";
inline constexpr char Halftone[] = "halftone";
inline constexpr char Scanlines[] = "scanlines";
inline constexpr char PencilSketch[] = "pencil_sketch";
inline constexpr char Abstraction[] = "abstraction";
inline constexpr char Prism[] = "prism";
inline constexpr char Duotone[] = "duotone";
inline constexpr char Twirl[] = "twirl";
inline constexpr char BulgePinch[] = "bulge_pinch";
inline constexpr char Kaleidoscope[] = "kaleidoscope";
inline constexpr char MirrorSplit[] = "mirror_split";
inline constexpr char CornerPin[] = "corner_pin";
inline constexpr char CameraShake[] = "camera_shake";
inline constexpr char Echo[] = "echo";
inline constexpr char Strobe[] = "strobe";
inline constexpr char Flicker[] = "flicker";
inline constexpr char Tile[] = "tile";
// 1.2 (spec 56, BuiltinVideoEffects3.cpp)
inline constexpr char ColoredBorder[] = "colored_border";
inline constexpr char Despill[] = "despill";
inline constexpr char AlphaMatteShrinkGrow[] = "alpha_matte_shrink_grow";
inline constexpr char Transform[] = "transform";
inline constexpr char Dent[] = "dent";
inline constexpr char FastNoise[] = "fast_noise";
inline constexpr char GamutLimiter[] = "gamut_limiter";
inline constexpr char ContrastPop[] = "contrast_pop";
inline constexpr char ChromaticAberrationFix[] = "chromatic_aberration_fix";
inline constexpr char FalseColor[] = "false_color";
inline constexpr char Dehaze[] = "dehaze";
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
// Batch 2: colour
inline const QString MasterCurve = QStringLiteral("masterCurve");
inline const QString RedCurve = QStringLiteral("redCurve");
inline const QString GreenCurve = QStringLiteral("greenCurve");
inline const QString BlueCurve = QStringLiteral("blueCurve");
inline const QString HueShift = QStringLiteral("hueShift");
inline const QString Saturation = QStringLiteral("saturation");
inline const QString Lightness = QStringLiteral("lightness");
inline const QString Range = QStringLiteral("range"); ///< HSL adjust: all or one hue range
inline const QString LutFile = QStringLiteral("lutFile");
inline const QString Vibrance = QStringLiteral("vibrance");
/// Channel mixer: output channel from input channel (rg = red output from green input).
inline const QString MixRR = QStringLiteral("rr");
inline const QString MixRG = QStringLiteral("rg");
inline const QString MixRB = QStringLiteral("rb");
inline const QString MixGR = QStringLiteral("gr");
inline const QString MixGG = QStringLiteral("gg");
inline const QString MixGB = QStringLiteral("gb");
inline const QString MixBR = QStringLiteral("br");
inline const QString MixBG = QStringLiteral("bg");
inline const QString MixBB = QStringLiteral("bb");
inline const QString ShadowColor = QStringLiteral("shadowColor");
inline const QString HighlightColor = QStringLiteral("highlightColor");
// Batch 2: blur, light, stylize
inline const QString FocusWidth = QStringLiteral("focusWidth");
inline const QString Transition = QStringLiteral("transition");
inline const QString Highlights = QStringLiteral("highlights");
inline const QString Streaks = QStringLiteral("streaks");
inline const QString Ghosts = QStringLiteral("ghosts");
inline const QString Shape = QStringLiteral("shape");
inline const QString BackgroundColor = QStringLiteral("backgroundColor");
inline const QString Mode = QStringLiteral("mode");
inline const QString Spacing = QStringLiteral("spacing");
inline const QString Thickness = QStringLiteral("thickness");
inline const QString ColorAmount = QStringLiteral("colorAmount");
inline const QString Edges = QStringLiteral("edges");
inline const QString RepeatEdges = QStringLiteral("repeatEdges");
// Batch 2: distort, transform, time
inline const QString Segments = QStringLiteral("segments");
inline const QString TopLeftX = QStringLiteral("tlX");
inline const QString TopLeftY = QStringLiteral("tlY");
inline const QString TopRightX = QStringLiteral("trX");
inline const QString TopRightY = QStringLiteral("trY");
inline const QString BottomRightX = QStringLiteral("brX");
inline const QString BottomRightY = QStringLiteral("brY");
inline const QString BottomLeftX = QStringLiteral("blX");
inline const QString BottomLeftY = QStringLiteral("blY");
inline const QString Frequency = QStringLiteral("frequency");
inline const QString Rotation = QStringLiteral("rotation");
inline const QString MotionBlur = QStringLiteral("motionBlur");
inline const QString Seed = QStringLiteral("seed");
inline const QString Echoes = QStringLiteral("echoes");
inline const QString Delay = QStringLiteral("delay");
inline const QString Decay = QStringLiteral("decay");
inline const QString Hold = QStringLiteral("hold");
inline const QString Randomness = QStringLiteral("randomness");
inline const QString OffsetX = QStringLiteral("offsetX");
inline const QString OffsetY = QStringLiteral("offsetY");
inline const QString Mirror = QStringLiteral("mirror");
// 1.2
inline const QString Scale = QStringLiteral("scale"); ///< percent (transform) or px (noise)
inline const QString Detail = QStringLiteral("detail");
inline const QString Contrast = QStringLiteral("contrast");
inline const QString Evolution = QStringLiteral("evolution");
inline const QString SourceGamut = QStringLiteral("sourceGamut");
inline const QString TargetGamut = QStringLiteral("targetGamut");
inline const QString RedShift = QStringLiteral("redShift");
inline const QString BlueShift = QStringLiteral("blueShift");
inline const QString Defringe = QStringLiteral("defringe");
inline const QString Blend = QStringLiteral("blend");
inline const QString Palette = QStringLiteral("palette");
inline const QString HazeColor = QStringLiteral("hazeColor");
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
// HSL adjust ranges (centre hues 0, 60, ..., 300 degrees)
inline const QString All = QStringLiteral("all");
inline const QString Reds = QStringLiteral("reds");
inline const QString Yellows = QStringLiteral("yellows");
inline const QString Greens = QStringLiteral("greens");
inline const QString Cyans = QStringLiteral("cyans");
inline const QString Blues = QStringLiteral("blues");
inline const QString Magentas = QStringLiteral("magentas");
// Pixelate shapes
inline const QString Hexagons = QStringLiteral("hexagons");
inline const QString Circles = QStringLiteral("circles");
inline const QString Diamonds = QStringLiteral("diamonds");
// Halftone modes
inline const QString Mono = QStringLiteral("mono");
inline const QString Colored = QStringLiteral("color");
// Mirror split modes: which half is kept and mirrored onto the other
inline const QString KeepLeft = QStringLiteral("left");
inline const QString KeepRight = QStringLiteral("right");
inline const QString KeepTop = QStringLiteral("top");
inline const QString KeepBottom = QStringLiteral("bottom");
inline const QString Quad = QStringLiteral("quad");
// 1.2: despill channels, gamut limiter gamuts and methods, noise blend modes, dent types
inline const QString Red = QStringLiteral("red");
inline const QString Rec709 = QStringLiteral("rec709");
inline const QString DciP3 = QStringLiteral("p3d65");
inline const QString Rec2020 = QStringLiteral("rec2020");
inline const QString Desaturate = QStringLiteral("desaturate");
inline const QString Clip = QStringLiteral("clip");
inline const QString Normal = QStringLiteral("normal");
inline const QString Add = QStringLiteral("add");
inline const QString Multiply = QStringLiteral("multiply");
inline const QString Screen = QStringLiteral("screen");
inline const QString Overlay = QStringLiteral("overlay");
inline const QString Transparent = QStringLiteral("transparent");
inline const QString Repeat = QStringLiteral("repeat");
inline const QString Mirrored = QStringLiteral("mirrored");
inline const QString Bulge = QStringLiteral("bulge");
inline const QString Pinch = QStringLiteral("pinch");
inline const QString BowHorizontal = QStringLiteral("bowHorizontal");
inline const QString BowVertical = QStringLiteral("bowVertical");
inline const QString SquareBulge = QStringLiteral("squareBulge");
inline const QString SquarePinch = QStringLiteral("squarePinch");
inline const QString Exposure = QStringLiteral("exposure");
inline const QString Thermal = QStringLiteral("thermal");
inline const QString Rainbow = QStringLiteral("rainbow");
} // namespace fxchoice

} // namespace kk

#include "core/VideoEffectKeys4.h"
#include "core/VideoEffectKeys5.h"
#include "core/VideoEffectKeys6.h"
#include "core/VideoEffectKeys7.h"
