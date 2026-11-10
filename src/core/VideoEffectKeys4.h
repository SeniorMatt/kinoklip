// SPDX-License-Identifier: GPL-3.0-or-later
// VideoEffectKeys4 — ids, parameter keys and Choice values of the fourth batch of video effects
// (1.2, docs/behavior-spec.md section 57): sharpen group, texture, revival, damage, paint,
// optics, Beauty, Color Palette, 3D Keyer, Video Collage. Included at the end of
// VideoEffectKeys.h, so users of that header see these too.
#pragma once

#include <QString>

namespace kk {

namespace fxid {
inline constexpr char SharpenEdges[] = "sharpen_edges";
inline constexpr char SoftenSharpen[] = "soften_and_sharpen";
inline constexpr char TexturePop[] = "texture_pop";
inline constexpr char DetailRecovery[] = "detail_recovery";
inline constexpr char Deband[] = "deband";
inline constexpr char DeadPixelFixer[] = "dead_pixel_fixer";
inline constexpr char AnalogDamage[] = "analog_damage";
inline constexpr char FilmDamage[] = "film_damage";
inline constexpr char JpegDamage[] = "jpeg_damage";
inline constexpr char Watercolor[] = "watercolor";
inline constexpr char Stylize[] = "stylize";
inline constexpr char ApertureDiffraction[] = "aperture_diffraction";
inline constexpr char LensReflections[] = "lens_reflections";
inline constexpr char Beauty[] = "beauty";
inline constexpr char ColorPalette[] = "color_palette";
inline constexpr char Keyer3d[] = "keyer_3d";
inline constexpr char VideoCollage[] = "video_collage";
} // namespace fxid

namespace fxkeys {
inline const QString HaloLimit = QStringLiteral("haloLimit");
inline const QString Soften = QStringLiteral("soften");
inline const QString Sharpen = QStringLiteral("sharpen");
inline const QString LumaOnly = QStringLiteral("lumaOnly");
inline const QString Shadows = QStringLiteral("shadows");
inline const QString Dither = QStringLiteral("dither");
inline const QString Smoothing = QStringLiteral("smoothing");
inline const QString EdgePreserve = QStringLiteral("edgePreserve");
inline const QString DetailRecover = QStringLiteral("detailRecover");
inline const QString SkinOnly = QStringLiteral("skinOnly");
inline const QString Jitter = QStringLiteral("jitter");
inline const QString ColorBleed = QStringLiteral("colorBleed");
inline const QString Noise = QStringLiteral("noise");
inline const QString Tracking = QStringLiteral("tracking");
inline const QString Dirt = QStringLiteral("dirt");
inline const QString Scratches = QStringLiteral("scratches");
inline const QString Vignette = QStringLiteral("vignette");
inline const QString Blur = QStringLiteral("blur");
inline const QString Weave = QStringLiteral("weave");
inline const QString Quality = QStringLiteral("quality");
inline const QString BlockSize = QStringLiteral("blockSize");
inline const QString EdgeDarkening = QStringLiteral("edgeDarkening");
inline const QString Paper = QStringLiteral("paper");
inline const QString Points = QStringLiteral("points");
inline const QString Distance = QStringLiteral("distance");
inline const QString Count = QStringLiteral("count");
inline const QString Color2 = QStringLiteral("color2");
inline const QString Color3 = QStringLiteral("color3");
inline const QString UseColor2 = QStringLiteral("useColor2");
inline const QString UseColor3 = QStringLiteral("useColor3");
inline const QString LumaWeight = QStringLiteral("lumaWeight");
inline const QString Columns = QStringLiteral("columns");
inline const QString Rows = QStringLiteral("rows");
inline const QString Fit = QStringLiteral("fit");
inline const QString ShadowOpacity = QStringLiteral("shadowOpacity");
inline const QString Detect = QStringLiteral("detect");
inline const QString Style = QStringLiteral("style");
inline const QString Gap = QStringLiteral("gap");
inline const QString ScanlineAmount = QStringLiteral("scanlineAmount");
inline const QString FlickerAmount = QStringLiteral("flickerAmount");
} // namespace fxkeys

namespace fxchoice {
inline const QString Both = QStringLiteral("both");
inline const QString Bright = QStringLiteral("bright");
inline const QString Dark = QStringLiteral("dark");
inline const QString Relief = QStringLiteral("relief");
inline const QString Ink = QStringLiteral("ink");
inline const QString Cutout = QStringLiteral("cutout");
inline const QString Fill = QStringLiteral("fill");
inline const QString FitInside = QStringLiteral("fit");
inline const QString Stretch = QStringLiteral("stretch");
inline const QString PosBottom = QStringLiteral("bottom");
inline const QString PosTop = QStringLiteral("top");
inline const QString PosLeft = QStringLiteral("left");
inline const QString PosRight = QStringLiteral("right");
} // namespace fxchoice

} // namespace kk
