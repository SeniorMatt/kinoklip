// SPDX-License-Identifier: GPL-3.0-or-later
// The template looks of the 1.2 gap list (docs/behavior-spec.md section 65): CCTV, Digital
// Glitch, Drone Overlay, DSLR, DVE, Video Call and Video Camera. Each is designed from the
// general idea of its name; every element is drawn by the program (src/render: LookHud.cpp for
// the overlays, GlTemplateLooks.cpp for the picture passes). The same effects also come as
// ready-made adjustment clips in Effects > Effects (core/Generators.h).
#include "core/EffectBuilders.h"
#include "core/EffectRegistry.h"
#include "core/VideoEffectKeys.h"

#include <QCoreApplication>

namespace kk {

using namespace builders;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::BuiltinVideoEffects", text);
}

const QString kPx = QStringLiteral("px");
const QString kPercent = QStringLiteral("%");
const QString kDegrees = QStringLiteral("°");

EffectDescriptor effect(const char* id, const QString& name, const QString& description,
    QVector<ParamDescriptor> params)
{
    EffectDescriptor d = descriptor(
        QLatin1String(id), name, EffectCategory::VideoEffect, description, std::move(params));
    d.group = QLatin1String(effectgroups::Stylize);
    return d;
}

ParamDescriptor percent(
    const QString& key, const QString& label, double min, double max, double def)
{
    return number(key, label, min, max, def, 0.5, kPercent);
}

/// Font, text size and overlay opacity: the last parameters of every overlay look.
void addOverlayStyle(QVector<ParamDescriptor>& params)
{
    namespace k = fxkeys;
    params.append(text(k::LookFont, tr("Font"), QString()));
    params.append(percent(k::TextSize, tr("Text Size"), 25, 300, 100));
    params.append(percent(k::OverlayOpacity, tr("Overlay Opacity"), 0, 100, 100));
}

/// A whole-number parameter that cannot be keyframed (a seed).
ParamDescriptor fixedInt(const QString& key, const QString& label, int min, int max, int def)
{
    ParamDescriptor p = integer(key, label, min, max, def);
    p.animatable = false;
    return p;
}

ParamDescriptor startTime()
{
    return text(
        fxkeys::StartTime, tr("Start Date and Time"), QStringLiteral("2026-03-14 22:05:00"));
}

} // namespace

void EffectRegistry::registerVideoEffects6()
{
    namespace k = fxkeys;
    namespace c = fxchoice;

    {
        QVector<ParamDescriptor> p = {
            text(k::CameraName, tr("Camera Name"), QStringLiteral("CAM 01 - FRONT DOOR")),
            startTime(),
            text(k::TimeFormat, tr("Time Format"), QStringLiteral("yyyy-MM-dd  HH:mm:ss")),
            color(k::Color, tr("Text Color"), QColor(0xee, 0xee, 0xe6)),
            boolean(k::RecDot, tr("Recording Dot"), true),
            percent(k::Desaturate, tr("Desaturation"), 0, 100, 85),
            percent(k::Noise, tr("Noise"), 0, 100, 25),
            percent(k::ScanlineAmount, tr("Scanlines"), 0, 100, 30),
            percent(k::VignetteAmount, tr("Vignette"), 0, 100, 40),
        };
        addOverlayStyle(p);
        add(effect(fxid::LookCctv, tr("CCTV"),
            tr("A security-camera picture: washed colors, noise, scanlines and corner shading, "
               "with a camera name, a running date and time, and a blinking recording dot"),
            p));
    }

    add(effect(fxid::LookDigitalGlitch, tr("Digital Glitch"),
        tr("Digital damage: bands of the picture slide sideways, blocks jump, the color channels "
           "split apart and bright noise flickers"),
        {
            percent(k::Amount, tr("Amount"), 0, 100, 50),
            number(k::ShiftAmount, tr("Band Shift"), 0, 1000, 120, 1, kPx),
            number(k::BlockSize, tr("Block Size"), 4, 400, 64, 1, kPx),
            number(k::ColorSplit, tr("Color Split"), 0, 200, 8, 0.5, kPx),
            percent(k::Noise, tr("Digital Noise"), 0, 100, 20),
            number(k::GlitchRate, tr("Glitches per Second"), 0.5, 50, 8, 0.5),
            fixedInt(k::Seed, tr("Seed"), 0, 9999, 1),
        }));

    {
        QVector<ParamDescriptor> p = {
            number(k::Altitude, tr("Altitude"), -500, 20000, 120, 0.5, QStringLiteral("m")),
            number(k::GroundSpeed, tr("Speed"), 0, 500, 12.5, 0.1, QStringLiteral("m/s")),
            number(k::HomeDistance, tr("Distance to Home"), 0, 50000, 340, 1, QStringLiteral("m")),
            number(k::Heading, tr("Heading"), 0, 360, 45, 0.5, kDegrees),
            number(k::GimbalPitch, tr("Gimbal Pitch"), -90, 30, -12, 0.5, kDegrees),
            percent(k::BatteryLevel, tr("Battery"), 0, 100, 76),
            integer(k::Satellites, tr("Satellites"), 0, 40, 14),
            choice(k::Units, tr("Units"), {c::Metric, c::Imperial}, {tr("Metric"), tr("Imperial")},
                c::Metric),
            boolean(k::ShowMarks, tr("Framing Marks"), true),
            boolean(k::ShowCompass, tr("Compass Bar"), true),
            color(k::Color, tr("Overlay Color"), QColor(0xf4, 0xf4, 0xf0)),
        };
        addOverlayStyle(p);
        add(effect(fxid::LookDroneOverlay, tr("Drone Overlay"),
            tr("A drone-camera display: corner framing marks, a compass bar, a horizon line with "
               "the gimbal pitch, and readouts of altitude, speed, distance, battery and "
               "satellites that can be keyframed"),
            p));
    }

    {
        QVector<ParamDescriptor> p = {
            text(k::ShutterSpeed, tr("Shutter Speed"), QStringLiteral("1/250")),
            number(k::Aperture, tr("Aperture (f/)"), 0.7, 64, 2.8, 0.1),
            integer(k::Iso, tr("ISO"), 50, 409600, 400),
            number(
                k::ExposureComp, tr("Exposure Compensation"), -5, 5, 0, 0.1, QStringLiteral("EV")),
            percent(k::FocusX, tr("Focus Point X"), -100, 100, 0),
            percent(k::FocusY, tr("Focus Point Y"), -100, 100, 0),
            boolean(k::FocusLocked, tr("Focus Locked"), true),
            choice(k::FocusMode, tr("Focus Mode"), {c::AfSingle, c::AfContinuous, c::ManualFocus},
                {tr("AF-S"), tr("AF-C"), tr("MF")}, c::AfSingle),
            choice(k::GridStyle, tr("Grid"), {c::GridOff, c::GridThirds, c::GridFine},
                {tr("Off"), tr("Rule of Thirds"), tr("Fine Grid")}, c::GridThirds),
            integer(k::ShotsLeft, tr("Shots Remaining"), 0, 9999, 1024),
            color(k::Color, tr("Overlay Color"), QColor(0xf2, 0xf2, 0xee)),
        };
        addOverlayStyle(p);
        add(effect(fxid::LookDslr, tr("DSLR"),
            tr("A photo-camera viewfinder: focus points with a lock, frame lines, a grid, an "
               "exposure scale and a readout of shutter speed, aperture and ISO"),
            p));
    }

    add(effect(fxid::LookDve, tr("DVE"),
        tr("A picture-in-picture box: the picture shrinks into a box that you move, size, turn "
           "and tilt, with a border, rounded corners and a drop shadow"),
        {
            number(k::PosX, tr("Position X"), -10000, 10000, 0, 1, kPx),
            number(k::PosY, tr("Position Y"), -10000, 10000, 0, 1, kPx),
            percent(k::Scale, tr("Size"), 0, 400, 60),
            number(k::Rotation, tr("Rotation"), -3600, 3600, 0, 0.1, kDegrees),
            number(k::TiltX, tr("Tilt X"), -80, 80, 0, 0.1, kDegrees),
            number(k::TiltY, tr("Tilt Y"), -80, 80, 0, 0.1, kDegrees),
            number(k::BorderWidth, tr("Border Width"), 0, 500, 8, 0.5, kPx),
            color(k::BorderColor, tr("Border Color"), QColor(0xf5, 0xf5, 0xf5)),
            number(k::CornerRadius, tr("Corner Radius"), 0, 1000, 12, 0.5, kPx),
            percent(k::ShadowOpacity, tr("Shadow Opacity"), 0, 100, 55),
            number(k::ShadowBlur, tr("Shadow Blur"), 0, 500, 24, 0.5, kPx),
            number(k::ShadowOffsetX, tr("Shadow Offset X"), -1000, 1000, 0, 1, kPx),
            number(k::ShadowOffsetY, tr("Shadow Offset Y"), -1000, 1000, 12, 1, kPx),
            percent(k::Opacity, tr("Opacity"), 0, 100, 100),
        }));

    {
        QVector<ParamDescriptor> p = {
            text(k::ParticipantName, tr("Name"), QStringLiteral("Alex Morgan")),
            number(k::TileMargin, tr("Margin"), 0, 500, 48, 0.5, kPx),
            number(k::CornerRadius, tr("Corner Radius"), 0, 500, 28, 0.5, kPx),
            color(k::BackgroundColor, tr("Background"), QColor(0x20, 0x22, 0x28)),
            percent(k::BackgroundOpacity, tr("Background Opacity"), 0, 100, 100),
            color(k::Color, tr("Speaking Ring Color"), QColor(0x3d, 0xd6, 0x8c)),
            boolean(k::Speaking, tr("Speaking"), false),
            boolean(k::Muted, tr("Muted"), false),
            integer(k::Signal, tr("Connection Bars"), 0, 4, 4),
            boolean(k::ShowTimer, tr("Call Timer"), true),
        };
        addOverlayStyle(p);
        add(effect(fxid::LookVideoCall, tr("Video Call"),
            tr("A video-call tile: the picture in a rounded frame on a dark background, with a "
               "name label, a microphone mark, connection bars, a call timer and a ring that "
               "lights when the person speaks"),
            p));
    }

    {
        QVector<ParamDescriptor> p = {
            boolean(k::Recording, tr("Recording"), true),
            boolean(k::Blink, tr("Blink Recording Dot"), true),
            text(k::StartTimecode, tr("Start Timecode"), QStringLiteral("00:00:00:00")),
            text(k::FormatLabel, tr("Format Label"), QStringLiteral("HD 1080 50i")),
            text(k::CardRemaining, tr("Card Remaining"), QStringLiteral("REMAIN 1h25m")),
            percent(k::BatteryLevel, tr("Battery"), 0, 100, 80),
            percent(k::ZoomLevel, tr("Zoom"), 0, 100, 20),
            boolean(k::ShowFocusFrame, tr("Focus Frame"), true),
            boolean(k::ShowMarks, tr("Corner Marks"), true),
            color(k::Color, tr("Overlay Color"), QColor(0xf4, 0xf4, 0xf0)),
        };
        addOverlayStyle(p);
        add(effect(fxid::LookVideoCamera, tr("Video Camera"),
            tr("A camcorder viewfinder: a blinking recording dot, a running timecode, battery, "
               "zoom and card readouts, a focus frame and corner marks"),
            p));
    }
}

} // namespace kk
