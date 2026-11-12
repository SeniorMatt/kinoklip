// SPDX-License-Identifier: GPL-3.0-or-later
// VideoEffectKeys8 — id, parameter keys and Choice values of Background Removal (1.4, AI tool,
// docs/behavior-spec.md section 78). Included at the end of VideoEffectKeys.h.
#pragma once

#include <QString>

namespace kk {

namespace fxid {
inline constexpr char BackgroundRemoval[] = "background_removal";
} // namespace fxid

namespace fxkeys {
inline const QString BgOutput = QStringLiteral("bgOutput");
inline const QString BgThreshold = QStringLiteral("bgThreshold");
inline const QString BgSoftness = QStringLiteral("bgSoftness");
inline const QString BgShrinkGrow = QStringLiteral("bgShrinkGrow");
inline const QString BgFeather = QStringLiteral("bgFeather");
inline const QString BgQuality = QStringLiteral("bgQuality");
inline const QString BgModel = QStringLiteral("bgModel");
} // namespace fxkeys

namespace fxchoice {
inline const QString BgForeground = QStringLiteral("foreground");
inline const QString BgMatte = QStringLiteral("matte");
inline const QString BgBackground = QStringLiteral("background");
inline const QString BgPreview = QStringLiteral("preview");
inline const QString BgFull = QStringLiteral("full");
// Model: automatic (still image -> any-subject model, video -> people model), or fixed.
inline const QString BgModelAuto = QStringLiteral("auto");
inline const QString BgModelVideo = QStringLiteral("video");
inline const QString BgModelImage = QStringLiteral("image");
} // namespace fxchoice

} // namespace kk
