// SPDX-License-Identifier: GPL-3.0-or-later
// VideoEffectKeys5 — ids, parameter keys and Choice values of the fifth batch of video effects
// (1.2, docs/behavior-spec.md section 60): the effects that read other frames of the clip
// (Motion Trails, Smear, Frame Replacer, Motion Blur, Deflicker, Color Stabilizer, Automatic
// Dirt Removal, Dust Buster) and Patch Replacer. Included at the end of VideoEffectKeys.h, so
// users of that header see these too.
#pragma once

#include <QString>

namespace kk {

namespace fxid {
inline constexpr char MotionTrails[] = "motion_trails";
inline constexpr char Smear[] = "smear";
inline constexpr char FrameReplacer[] = "frame_replacer";
inline constexpr char TemporalMotionBlur[] = "motion_blur";
inline constexpr char Deflicker[] = "deflicker";
inline constexpr char ColorStabilizer[] = "color_stabilizer";
inline constexpr char DirtRemoval[] = "automatic_dirt_removal";
inline constexpr char DustBuster[] = "dust_buster";
inline constexpr char PatchReplacer[] = "patch_replacer";
} // namespace fxid

namespace fxkeys {
inline const QString Trails = QStringLiteral("trails");
inline const QString TrailMode = QStringLiteral("trailMode");
inline const QString SmearDirection = QStringLiteral("smearDirection");
inline const QString FrameSource = QStringLiteral("frameSource");
inline const QString ShutterAngle = QStringLiteral("shutterAngle");
inline const QString Samples = QStringLiteral("samples");
inline const QString FrameWindow = QStringLiteral("frameWindow");
inline const QString PerChannel = QStringLiteral("perChannel");
inline const QString ReferenceFrame = QStringLiteral("referenceFrame");
inline const QString Match = QStringLiteral("match");
inline const QString RegionWidth = QStringLiteral("regionWidth");
inline const QString RegionHeight = QStringLiteral("regionHeight");
inline const QString RegionX = QStringLiteral("regionX");
inline const QString RegionY = QStringLiteral("regionY");
inline const QString MaxCorrection = QStringLiteral("maxCorrection");
inline const QString MotionThreshold = QStringLiteral("motionThreshold");
inline const QString DirtFrames = QStringLiteral("dirtFrames");
inline const QString Dilate = QStringLiteral("dilate");
inline const QString SourceX = QStringLiteral("sourceX");
inline const QString SourceY = QStringLiteral("sourceY");
inline const QString TargetX = QStringLiteral("targetX");
inline const QString TargetY = QStringLiteral("targetY");
inline const QString RegionMode = QStringLiteral("regionMode");
} // namespace fxkeys

namespace fxchoice {
inline const QString Lighten = QStringLiteral("lighten");
inline const QString Behind = QStringLiteral("behind");
inline const QString Ahead = QStringLiteral("ahead");
inline const QString Previous = QStringLiteral("previous");
inline const QString Next = QStringLiteral("next");
inline const QString Mix = QStringLiteral("mix");
inline const QString WhiteBalance = QStringLiteral("whiteBalance");
inline const QString DetectedDirt = QStringLiteral("dirt");
inline const QString WholeRegion = QStringLiteral("region");
} // namespace fxchoice

} // namespace kk
