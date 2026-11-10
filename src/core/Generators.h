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
/// Test patterns and utility generators added for 1.2 (spec 56). 100mV Steps is the Grey
/// Scale formula with eight steps of exactly 100 mV of a 700 mV signal; the three BT.2111 ids
/// are the HDR colour bar patterns of ITU-R BT.2111 (HLG narrow range, PQ narrow range, PQ
/// full range).
inline const QString Steps100mV = QStringLiteral("steps_100mv");
inline const QString EbuBars = QStringLiteral("ebu_color_bars");
inline const QString Bt2111HlgNarrow = QStringLiteral("bt2111_hlg_narrow");
inline const QString Bt2111PqNarrow = QStringLiteral("bt2111_pq_narrow");
inline const QString Bt2111PqFull = QStringLiteral("bt2111_pq_full");
inline const QString Window = QStringLiteral("window");
inline const QString YcbcrRamp = QStringLiteral("ycbcr_ramp");
inline const QString NoiseGradient = QStringLiteral("noise_gradient");
inline const QString Grid = QStringLiteral("grid");
/// Effects library > Effects: creates a ClipKind::Adjustment clip (no parameters of its own).
inline const QString AdjustmentClip = QStringLiteral("adjustment_clip");
/// Effects library > Effects: an adjustment clip that comes with a Gaussian Blur and a
/// rectangle mask, for blurring a region (faces, number plates; spec 49.8). Once placed it is
/// an ordinary adjustment clip: the mask moves / sizes / keyframes in the viewer.
inline const QString RegionBlur = QStringLiteral("region_blur");
/// Effects library > Effects, 1.2 (spec 56.4): adjustment clips that come ready-made. Colored
/// Border carries a Colored Border effect, Binoculars a black-outside-two-circles mask stack
/// with a Vignette, Night Vision a green duotone with grain, scanlines and a vignette. Once
/// placed they are ordinary adjustment clips, every part editable.
inline const QString ColoredBorderClip = QStringLiteral("template_colored_border");
inline const QString BinocularsClip = QStringLiteral("template_binoculars");
inline const QString NightVisionClip = QStringLiteral("template_night_vision");
/// Effects library > Effects, 1.2 (spec 65): adjustment clips that each carry one template look
/// effect (CCTV, Digital Glitch, Drone Overlay, DSLR, DVE, Video Call, Video Camera).
inline const QString CctvClip = QStringLiteral("template_cctv");
inline const QString DigitalGlitchClip = QStringLiteral("template_digital_glitch");
inline const QString DroneOverlayClip = QStringLiteral("template_drone_overlay");
inline const QString DslrClip = QStringLiteral("template_dslr");
inline const QString DveClip = QStringLiteral("template_dve");
inline const QString VideoCallClip = QStringLiteral("template_video_call");
inline const QString VideoCameraClip = QStringLiteral("template_video_camera");
/// Effects library > Effects: a see-through clip that carries an empty composition (the
/// Compositing page opens it): a place to build a node graph that is not attached to footage.
inline const QString CompositionClip = QStringLiteral("composition_clip");

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

// Keys of the 1.2 generators (window, YCbCr ramp, noise gradient, grid).
inline const QString Shape = QStringLiteral("shape"); ///< window: "rectangle" | "ellipse"
inline const QString Width = QStringLiteral("width"); ///< window width, px
inline const QString Height = QStringLiteral("height"); ///< window height, px
inline const QString Softness = QStringLiteral("softness"); ///< window edge softness, px
inline const QString Channel = QStringLiteral("channel"); ///< ycbcr ramp: all | y | cb | cr
inline const QString Scale = QStringLiteral("scale"); ///< noise gradient feature size, px
inline const QString Detail = QStringLiteral("detail"); ///< noise gradient octaves (static int)
inline const QString Contrast = QStringLiteral("contrast"); ///< noise gradient, percent
inline const QString Evolution = QStringLiteral("evolution"); ///< noise gradient time axis
inline const QString Columns = QStringLiteral("columns"); ///< grid: cells across (static int)
inline const QString Rows = QStringLiteral("rows"); ///< grid: cells down (static int)
inline const QString LineWidth = QStringLiteral("lineWidth"); ///< grid line width, px
inline const QString LineColor = QStringLiteral("lineColor"); ///< grid line colour
inline const QString MajorEvery = QStringLiteral("majorEvery"); ///< every n-th line is major
inline const QString MajorWidth = QStringLiteral("majorWidth"); ///< major line width, px
/// grid: percent opacity of the background colour (0 = see-through, for use as an overlay)
inline const QString BackgroundOpacity = QStringLiteral("backgroundOpacity");

} // namespace kk::generators
