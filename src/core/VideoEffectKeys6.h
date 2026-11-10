// SPDX-License-Identifier: GPL-3.0-or-later
// VideoEffectKeys6 — ids, parameter keys and Choice values of the template looks of the 1.2 gap
// list (docs/behavior-spec.md section 65): CCTV, Digital Glitch, Drone Overlay, DSLR, DVE,
// Video Call and Video Camera. Included at the end of VideoEffectKeys.h, so users of that
// header see these too. Pixel code: src/render (GlTemplateLooks.cpp, LookHud.cpp).
#pragma once

#include <QString>

namespace kk {

namespace fxid {
inline constexpr char LookCctv[] = "look_cctv";
inline constexpr char LookDigitalGlitch[] = "look_digital_glitch";
inline constexpr char LookDroneOverlay[] = "look_drone_overlay";
inline constexpr char LookDslr[] = "look_dslr";
inline constexpr char LookDve[] = "look_dve";
inline constexpr char LookVideoCall[] = "look_video_call";
inline constexpr char LookVideoCamera[] = "look_video_camera";
} // namespace fxid

namespace fxkeys {
// Shared by the overlay looks (CCTV, Drone, DSLR, Video Call, Video Camera).
inline const QString LookFont = QStringLiteral("font"); ///< family; empty = the default font
inline const QString TextSize = QStringLiteral("textSize"); ///< percent of the design size
inline const QString TextShadow = QStringLiteral("textShadow"); ///< percent, 0 = none
inline const QString OverlayOpacity = QStringLiteral("overlayOpacity"); ///< percent
// CCTV
inline const QString CameraName = QStringLiteral("cameraName");
inline const QString StartTime = QStringLiteral("startTime"); ///< "yyyy-MM-dd HH:mm:ss"
inline const QString TimeFormat = QStringLiteral("timeFormat"); ///< QDateTime format string
inline const QString Desaturate = QStringLiteral("desaturate"); ///< percent
inline const QString VignetteAmount = QStringLiteral("vignetteAmount"); ///< percent
inline const QString RecDot = QStringLiteral("recDot"); ///< bool
// Digital Glitch
inline const QString ShiftAmount = QStringLiteral("shiftAmount"); ///< px, widest slice shift
inline const QString ColorSplit = QStringLiteral("colorSplit"); ///< px
inline const QString GlitchRate = QStringLiteral("glitchRate"); ///< new glitch patterns per second
// DVE
inline const QString PosX = QStringLiteral("posX"); ///< px
inline const QString PosY = QStringLiteral("posY"); ///< px
inline const QString TiltX = QStringLiteral("tiltX"); ///< degrees, about the horizontal axis
inline const QString TiltY = QStringLiteral("tiltY"); ///< degrees, about the vertical axis
inline const QString BorderWidth = QStringLiteral("borderWidth"); ///< px
inline const QString BorderColor = QStringLiteral("borderColor");
inline const QString CornerRadius = QStringLiteral("cornerRadius"); ///< px
inline const QString ShadowBlur = QStringLiteral("shadowBlur"); ///< px
inline const QString ShadowOffsetX = QStringLiteral("shadowOffsetX"); ///< px
inline const QString ShadowOffsetY = QStringLiteral("shadowOffsetY"); ///< px
// Drone Overlay
inline const QString Altitude = QStringLiteral("altitude"); ///< metres
inline const QString GroundSpeed = QStringLiteral("groundSpeed"); ///< metres per second
inline const QString HomeDistance = QStringLiteral("homeDistance"); ///< metres
inline const QString Heading = QStringLiteral("heading"); ///< degrees, 0 = north
inline const QString BatteryLevel = QStringLiteral("batteryLevel"); ///< percent
inline const QString GimbalPitch = QStringLiteral("gimbalPitch"); ///< degrees, negative = down
inline const QString Satellites = QStringLiteral("satellites");
inline const QString Units = QStringLiteral("units"); ///< metric / imperial
inline const QString ShowMarks = QStringLiteral("showMarks"); ///< bool, framing marks
inline const QString ShowCompass = QStringLiteral("showCompass"); ///< bool
// DSLR
inline const QString ShutterSpeed = QStringLiteral("shutterSpeed"); ///< text, e.g. 1/250
inline const QString Aperture = QStringLiteral("aperture"); ///< f-number
inline const QString Iso = QStringLiteral("iso");
inline const QString ExposureComp = QStringLiteral("exposureComp"); ///< EV
inline const QString FocusX = QStringLiteral("focusX"); ///< percent of half the width
inline const QString FocusY = QStringLiteral("focusY"); ///< percent of half the height
inline const QString FocusLocked = QStringLiteral("focusLocked"); ///< bool
inline const QString FocusMode = QStringLiteral("focusMode");
inline const QString GridStyle = QStringLiteral("gridStyle");
inline const QString ShotsLeft = QStringLiteral("shotsLeft");
// Video Call
inline const QString ParticipantName = QStringLiteral("participantName");
inline const QString TileMargin = QStringLiteral("tileMargin"); ///< px
inline const QString Speaking = QStringLiteral("speaking"); ///< bool
inline const QString Muted = QStringLiteral("muted"); ///< bool
inline const QString Signal = QStringLiteral("signal"); ///< 0 - 4 bars
inline const QString ShowTimer = QStringLiteral("showTimer"); ///< bool
inline const QString BackgroundOpacity = QStringLiteral("backgroundOpacity"); ///< percent
// Video Camera
inline const QString Recording = QStringLiteral("recording"); ///< bool, REC or STBY
inline const QString Blink = QStringLiteral("blink"); ///< bool, the REC dot blinks
inline const QString StartTimecode = QStringLiteral("startTimecode"); ///< HH:MM:SS:FF
inline const QString FormatLabel = QStringLiteral("formatLabel");
inline const QString CardRemaining = QStringLiteral("cardRemaining");
inline const QString ZoomLevel = QStringLiteral("zoomLevel"); ///< percent
inline const QString ShowFocusFrame = QStringLiteral("showFocusFrame"); ///< bool
} // namespace fxkeys

namespace fxchoice {
inline const QString Metric = QStringLiteral("metric");
inline const QString Imperial = QStringLiteral("imperial");
inline const QString AfSingle = QStringLiteral("AF-S");
inline const QString AfContinuous = QStringLiteral("AF-C");
inline const QString ManualFocus = QStringLiteral("MF");
inline const QString GridOff = QStringLiteral("off");
inline const QString GridThirds = QStringLiteral("thirds");
inline const QString GridFine = QStringLiteral("grid");
} // namespace fxchoice

} // namespace kk
