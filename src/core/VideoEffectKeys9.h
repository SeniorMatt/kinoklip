// SPDX-License-Identifier: GPL-3.0-or-later
// VideoEffectKeys9 — ids, parameter keys and Choice values of the colour management effects
// (docs/behavior-spec.md section 205): Color Space Transform, ACES Transform, Gamut Mapping,
// Chromatic Adaptation, Color Compressor, DCTL and the DCTL transition. Included at the end of
// VideoEffectKeys.h. The gamut / gamma / white point ids are the ones of core/ColorSpaces.h.
#pragma once

#include <QString>

namespace kk {

namespace fxid {
inline constexpr char ColorSpaceTransform[] = "color_space_transform";
inline constexpr char AcesTransform[] = "aces_transform";
inline constexpr char GamutMapping[] = "gamut_mapping";
inline constexpr char ChromaticAdaptation[] = "chromatic_adaptation";
inline constexpr char ColorCompressor[] = "color_compressor";
inline constexpr char Dctl[] = "dctl";
inline constexpr char DctlTransition[] = "dctl_transition";
} // namespace fxid

namespace fxkeys {
inline const QString CmInGamut = QStringLiteral("cmInGamut");
inline const QString CmInGamma = QStringLiteral("cmInGamma");
inline const QString CmOutGamut = QStringLiteral("cmOutGamut");
inline const QString CmOutGamma = QStringLiteral("cmOutGamma");
inline const QString CmTone = QStringLiteral("cmTone");
inline const QString CmGamutMap = QStringLiteral("cmGamutMap");
inline const QString CmFwdOotf = QStringLiteral("cmFwdOotf");
inline const QString CmInvOotf = QStringLiteral("cmInvOotf");
inline const QString CmAdapt = QStringLiteral("cmAdapt");
inline const QString CmMaxIn = QStringLiteral("cmMaxIn");
inline const QString CmMaxOut = QStringLiteral("cmMaxOut");
inline const QString AcesVersion = QStringLiteral("acesVersion");
inline const QString AcesInput = QStringLiteral("acesInput");
inline const QString AcesOutput = QStringLiteral("acesOutput");
inline const QString AcesInvert = QStringLiteral("acesInvert");
inline const QString CmTarget = QStringLiteral("cmTarget");
inline const QString CmGmMode = QStringLiteral("cmGmMode");
inline const QString CmThreshold = QStringLiteral("cmThreshold");
inline const QString CmLimit = QStringLiteral("cmLimit");
inline const QString CmPower = QStringLiteral("cmPower");
inline const QString CmMethod = QStringLiteral("cmMethod");
inline const QString CmSrcWhite = QStringLiteral("cmSrcWhite");
inline const QString CmDstWhite = QStringLiteral("cmDstWhite");
inline const QString CmSrcX = QStringLiteral("cmSrcX");
inline const QString CmSrcY = QStringLiteral("cmSrcY");
inline const QString CmDstX = QStringLiteral("cmDstX");
inline const QString CmDstY = QStringLiteral("cmDstY");
inline const QString CmHiThreshold = QStringLiteral("cmHiThreshold");
inline const QString CmHiLimit = QStringLiteral("cmHiLimit");
inline const QString CmSatThreshold = QStringLiteral("cmSatThreshold");
inline const QString CmSatLimit = QStringLiteral("cmSatLimit");
inline const QString DctlFile = QStringLiteral("dctlFile");
/// "dctlP1" ... "dctlP8": the sliders, check boxes and combo boxes of the file (offsets from
/// its defaults); "dctlC1", "dctlC2": its colour pickers.
inline QString dctlParam(int i)
{
    return QStringLiteral("dctlP%1").arg(i);
}
inline QString dctlColor(int i)
{
    return QStringLiteral("dctlC%1").arg(i);
}
} // namespace fxkeys

namespace fxchoice {
// Tone mapping
inline const QString ToneNone = QStringLiteral("none");
inline const QString ToneSimple = QStringLiteral("simple");
inline const QString ToneLuminance = QStringLiteral("luminance");
inline const QString ToneDavinci = QStringLiteral("davinci");
// Gamut mapping
inline const QString GmNone = QStringLiteral("none");
inline const QString GmSaturation = QStringLiteral("saturation");
inline const QString GmCompression = QStringLiteral("compression");
// Adaptation method
inline const QString CatBradford = QStringLiteral("bradford");
inline const QString CatCat02 = QStringLiteral("cat02");
inline const QString CatVonKries = QStringLiteral("vonkries");
} // namespace fxchoice

} // namespace kk
