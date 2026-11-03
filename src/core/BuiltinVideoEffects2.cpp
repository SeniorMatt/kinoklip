// SPDX-License-Identifier: GPL-3.0-or-later
// The second batch of v4 video effects: Curves, HSL Adjust, Apply LUT, Vibrance, Channel
// Mixer, Duotone; Unsharp Mask, Tilt-Shift, Lens Blur, Noise Reduction; Light Rays, Bloom,
// Halation, Lens Flare; Pixelate, Halftone, Scanlines, Pencil Sketch, Abstraction, Prism;
// Twirl, Bulge / Pinch, Kaleidoscope, Mirror Split, Corner Pin, Camera Shake; Echo, Strobe,
// Flicker; Tile. Pixel code: src/render (GlGradeEffects.cpp, GlFocusEffects.cpp,
// GlLightEffects.cpp, GlLookEffects.cpp, GlWarpEffects.cpp, GlTimeEffects.cpp; CPU reference
// EffectMath.h). Behaviour: docs/behavior-spec.md section 40.
#include "core/CurveParam.h"
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
const QString kHz = QStringLiteral("Hz");

EffectDescriptor effect(const char* id, const QString& name, const char* group,
    const QString& description, QVector<ParamDescriptor> params)
{
    EffectDescriptor d = descriptor(
        QLatin1String(id), name, EffectCategory::VideoEffect, description, std::move(params));
    d.group = QLatin1String(group);
    return d;
}

ParamDescriptor center(const QString& key, const QString& label)
{
    return number(key, label, -10000, 10000, 0, 1, kPx);
}

ParamDescriptor percent(
    const QString& key, const QString& label, double min, double max, double def)
{
    return number(key, label, min, max, def, 0.5, kPercent);
}

/// One channel mixer gain (percent of an input channel in an output channel).
ParamDescriptor mix(const QString& key, const QString& label, double def)
{
    return number(key, label, -200, 200, def, 0.5, kPercent);
}

} // namespace

void EffectRegistry::registerVideoEffects2()
{
    namespace k = fxkeys;
    namespace c = fxchoice;
    namespace g = effectgroups;
    const QString kIdentityCurve = kk::curve::identityText();

    // ---- Color ----
    add(effect(fxid::Curves, tr("Curves"), g::Color,
        tr("Tone curves: a master curve and one per red, green and blue channel"),
        {
            builders::curve(k::MasterCurve, tr("Master"), kIdentityCurve),
            builders::curve(k::RedCurve, tr("Red"), kIdentityCurve),
            builders::curve(k::GreenCurve, tr("Green"), kIdentityCurve),
            builders::curve(k::BlueCurve, tr("Blue"), kIdentityCurve),
            percent(k::Amount, tr("Amount"), 0, 100, 100),
        }));
    add(effect(fxid::HslAdjust, tr("HSL Adjust"), g::Color,
        tr("Shifts the hue and changes saturation and lightness, of all colors or one hue "
           "range"),
        {
            choice(k::Range, tr("Range"),
                {c::All, c::Reds, c::Yellows, c::Greens, c::Cyans, c::Blues, c::Magentas},
                {tr("All Colors"), tr("Reds"), tr("Yellows"), tr("Greens"), tr("Cyans"),
                    tr("Blues"), tr("Magentas")},
                c::All),
            number(k::HueShift, tr("Hue Shift"), -180, 180, 0, 0.5, kDegrees),
            percent(k::Saturation, tr("Saturation"), -100, 100, 0),
            percent(k::Lightness, tr("Lightness"), -100, 100, 0),
            number(k::HueWidth, tr("Range Width"), 10, 180, 60, 1, kDegrees),
            number(k::HueSoftness, tr("Range Softness"), 0, 90, 30, 1, kDegrees),
        }));
    add(effect(fxid::ApplyLut, tr("Apply LUT"), g::Color,
        tr("Applies a 1D or 3D look-up table from a .cube file"),
        {
            file(k::LutFile, tr("LUT File"), tr("Cube LUT files (*.cube)")),
            percent(k::Amount, tr("Strength"), 0, 100, 100),
        }));
    add(effect(fxid::Vibrance, tr("Vibrance"), g::Color,
        tr("Saturates muted colors more than colors that are already strong; skin is spared"),
        {
            percent(k::Vibrance, tr("Vibrance"), -100, 100, 30),
            percent(k::Saturation, tr("Saturation"), -100, 100, 0),
        }));
    add(effect(fxid::ChannelMixer, tr("Channel Mixer"), g::Color,
        tr("Builds each output channel from a mix of the red, green and blue input"),
        {
            mix(k::MixRR, tr("Red from Red"), 100),
            mix(k::MixRG, tr("Red from Green"), 0),
            mix(k::MixRB, tr("Red from Blue"), 0),
            mix(k::MixGR, tr("Green from Red"), 0),
            mix(k::MixGG, tr("Green from Green"), 100),
            mix(k::MixGB, tr("Green from Blue"), 0),
            mix(k::MixBR, tr("Blue from Red"), 0),
            mix(k::MixBG, tr("Blue from Green"), 0),
            mix(k::MixBB, tr("Blue from Blue"), 100),
            boolean(k::Monochrome, tr("Monochrome (Red Row)"), false),
        }));

    // ---- Blur ----
    add(effect(fxid::UnsharpMask, tr("Unsharp Mask"), g::Blur,
        tr("Sharpens edges; the threshold leaves flat areas and noise alone"),
        {
            percent(k::Amount, tr("Amount"), 0, 500, 100),
            number(k::Radius, tr("Radius"), 0.5, 50, 2, 0.1, kPx),
            percent(k::Threshold, tr("Threshold"), 0, 50, 0),
        }));
    add(effect(fxid::TiltShift, tr("Tilt-Shift"), g::Blur,
        tr("A sharp band with blur growing away from it, like a miniature"),
        {
            number(k::Radius, tr("Blur"), 0, 100, 12, 0.1, kPx),
            center(k::CenterY, tr("Focus Position")),
            number(k::FocusWidth, tr("Focus Width"), 0, 5000, 120, 1, kPx),
            number(k::Transition, tr("Transition"), 1, 5000, 200, 1, kPx),
            number(k::Angle, tr("Angle"), -90, 90, 0, 0.5, kDegrees),
        }));
    add(effect(fxid::LensBlur, tr("Lens Blur"), g::Blur,
        tr("Out-of-focus blur with round bokeh; bright spots become discs"),
        {
            number(k::Radius, tr("Radius"), 0, 100, 10, 0.1, kPx),
            percent(k::Highlights, tr("Bokeh Highlights"), 0, 100, 40),
            percent(k::Threshold, tr("Highlight Threshold"), 0, 100, 75),
        }));
    add(effect(fxid::NoiseReduction, tr("Noise Reduction"), g::Blur,
        tr("Smooths noise in flat areas and keeps edges (spatial, per frame)"),
        {
            percent(k::Strength, tr("Strength"), 0, 100, 40),
            number(k::Radius, tr("Radius"), 1, 4, 2, 0.1, kPx),
        }));

    // ---- Light ----
    add(effect(fxid::LightRays, tr("Light Rays"), g::Light,
        tr("Bright areas cast rays away from a light centre"),
        {
            center(k::CenterX, tr("Center X")),
            center(k::CenterY, tr("Center Y")),
            percent(k::Length, tr("Length"), 0, 100, 40),
            percent(k::Threshold, tr("Threshold"), 0, 100, 60),
            percent(k::Intensity, tr("Intensity"), 0, 400, 100),
            color(k::Color, tr("Color"), QColor(255, 244, 214)),
        }));
    add(effect(fxid::Bloom, tr("Bloom"), g::Light,
        tr("Wide, layered soft light around the highlights"),
        {
            percent(k::Threshold, tr("Threshold"), 0, 100, 60),
            number(k::Radius, tr("Radius"), 1, 200, 30, 0.5, kPx),
            percent(k::Intensity, tr("Intensity"), 0, 400, 80),
            percent(k::Saturation, tr("Saturation"), 0, 200, 100),
        }));
    add(effect(fxid::Halation, tr("Halation"), g::Light,
        tr("The reddish halo film shows around bright edges"),
        {
            percent(k::Threshold, tr("Threshold"), 0, 100, 65),
            number(k::Radius, tr("Radius"), 1, 100, 8, 0.5, kPx),
            percent(k::Amount, tr("Amount"), 0, 400, 100),
            color(k::Color, tr("Color"), QColor(255, 72, 24)),
        }));
    add(effect(fxid::LensFlare, tr("Lens Flare"), g::Light,
        tr("A drawn lens flare: glow, streaks, a halo ring and ghosts along the lens axis"),
        {
            number(k::CenterX, tr("Light X"), -10000, 10000, -400, 1, kPx),
            number(k::CenterY, tr("Light Y"), -10000, 10000, -220, 1, kPx),
            percent(k::Intensity, tr("Intensity"), 0, 400, 100),
            number(k::Size, tr("Size"), 10, 2000, 160, 1, kPx),
            integer(k::Streaks, tr("Streaks"), 0, 12, 6),
            percent(k::Ghosts, tr("Ghosts"), 0, 200, 100),
            color(k::Color, tr("Color"), QColor(255, 220, 170)),
        }));

    // ---- Stylize ----
    add(effect(fxid::Pixelate, tr("Pixelate"), g::Stylize,
        tr("Hexagon, dot or diamond cells filled with the picture's color"),
        {
            choice(k::Shape, tr("Shape"), {c::Hexagons, c::Circles, c::Diamonds},
                {tr("Hexagons"), tr("Circle Dots"), tr("Diamonds")}, c::Hexagons),
            number(k::Size, tr("Cell Size"), 2, 500, 24, 0.5, kPx),
            color(k::BackgroundColor, tr("Background"), QColor(Qt::black)),
        }));
    add(effect(fxid::Halftone, tr("Halftone"), g::Stylize,
        tr("Print-like dots whose size follows the brightness"),
        {
            choice(k::Mode, tr("Color"), {c::Mono, c::Colored},
                {tr("Black on White"), tr("Picture Colors")}, c::Mono),
            number(k::Size, tr("Dot Spacing"), 2, 200, 8, 0.5, kPx),
            number(k::Angle, tr("Angle"), -90, 90, 45, 0.5, kDegrees),
            percent(k::Amount, tr("Amount"), 0, 100, 100),
        }));
    add(effect(fxid::Scanlines, tr("Scanlines"), g::Stylize,
        tr("Dark horizontal lines like an old monitor, optionally rolling"),
        {
            number(k::Spacing, tr("Spacing"), 2, 200, 4, 0.5, kPx),
            percent(k::Thickness, tr("Thickness"), 5, 95, 50),
            percent(k::Amount, tr("Darkness"), 0, 100, 50),
            number(k::Speed, tr("Roll Speed"), -500, 500, 0, 1, QStringLiteral("px/s")),
            number(k::Angle, tr("Angle"), -90, 90, 0, 0.5, kDegrees),
        }));
    add(effect(fxid::PencilSketch, tr("Pencil Sketch"), g::Stylize,
        tr("Turns the picture into pencil lines on paper"),
        {
            number(k::Radius, tr("Line Width"), 0.5, 30, 4, 0.1, kPx),
            percent(k::Strength, tr("Strength"), 0, 400, 100),
            percent(k::ColorAmount, tr("Color"), 0, 100, 0),
            percent(k::Amount, tr("Amount"), 0, 100, 100),
        }));
    add(effect(fxid::Abstraction, tr("Abstraction"), g::Stylize,
        tr("Painterly look: smooth flat color areas with few tones and dark outlines"),
        {
            number(k::Radius, tr("Smoothness"), 1, 20, 4, 0.1, kPx),
            integer(k::Levels, tr("Tones"), 2, 32, 8),
            percent(k::Edges, tr("Outlines"), 0, 100, 40),
        }));
    add(effect(fxid::Prism, tr("Prism"), g::Stylize,
        tr("Splits the picture into a rainbow along one direction"),
        {
            number(k::Amount, tr("Spread"), 0, 200, 12, 0.1, kPx),
            number(k::Angle, tr("Angle"), -180, 180, 0, 0.5, kDegrees),
        }));
    add(effect(fxid::Duotone, tr("Duotone"), g::Stylize,
        tr("Maps the shadows to one color and the highlights to another"),
        {
            color(k::ShadowColor, tr("Shadows"), QColor(0x1e, 0x1b, 0x4b)),
            color(k::HighlightColor, tr("Highlights"), QColor(0xff, 0xc8, 0x57)),
            percent(k::Amount, tr("Amount"), 0, 100, 100),
        }));

    // ---- Distort ----
    add(effect(fxid::Twirl, tr("Twirl"), g::Distort,
        tr("Twists the picture around a centre, most at the centre"),
        {
            number(k::Angle, tr("Angle"), -3600, 3600, 180, 1, kDegrees),
            number(k::Radius, tr("Radius"), 1, 10000, 300, 1, kPx),
            center(k::CenterX, tr("Center X")),
            center(k::CenterY, tr("Center Y")),
        }));
    add(effect(fxid::BulgePinch, tr("Bulge / Pinch"), g::Distort,
        tr("Magnifies (bulge, positive) or squeezes (pinch, negative) a round area"),
        {
            percent(k::Amount, tr("Amount"), -100, 100, 50),
            number(k::Radius, tr("Radius"), 1, 10000, 300, 1, kPx),
            center(k::CenterX, tr("Center X")),
            center(k::CenterY, tr("Center Y")),
        }));
    add(effect(fxid::Kaleidoscope, tr("Kaleidoscope"), g::Distort,
        tr("Repeats one mirrored wedge of the picture around a centre"),
        {
            integer(k::Segments, tr("Segments"), 2, 32, 6),
            number(k::Angle, tr("Angle"), -180, 180, 0, 0.5, kDegrees),
            center(k::CenterX, tr("Center X")),
            center(k::CenterY, tr("Center Y")),
        }));
    add(effect(fxid::MirrorSplit, tr("Mirror Split"), g::Distort,
        tr("Mirrors one half (or quarter) of the picture onto the other"),
        {
            choice(k::Mode, tr("Keep"),
                {c::KeepLeft, c::KeepRight, c::KeepTop, c::KeepBottom, c::Quad},
                {tr("Left Half"), tr("Right Half"), tr("Top Half"), tr("Bottom Half"),
                    tr("Top Left Quarter")},
                c::KeepLeft),
            center(k::CenterX, tr("Mirror X")),
            center(k::CenterY, tr("Mirror Y")),
        }));
    add(effect(fxid::CornerPin, tr("Corner Pin"), g::Distort,
        tr("Moves the four corners freely (perspective): offsets from the clip's corners"),
        {
            center(k::TopLeftX, tr("Top Left X")),
            center(k::TopLeftY, tr("Top Left Y")),
            center(k::TopRightX, tr("Top Right X")),
            center(k::TopRightY, tr("Top Right Y")),
            center(k::BottomRightX, tr("Bottom Right X")),
            center(k::BottomRightY, tr("Bottom Right Y")),
            center(k::BottomLeftX, tr("Bottom Left X")),
            center(k::BottomLeftY, tr("Bottom Left Y")),
        }));
    add(effect(fxid::CameraShake, tr("Camera Shake"), g::Distort,
        tr("Hand-held or impact shake: smooth random movement, the same on every play"),
        {
            number(k::Amplitude, tr("Amplitude"), 0, 500, 12, 0.5, kPx),
            number(k::Frequency, tr("Frequency"), 0.1, 30, 4, 0.05, kHz),
            number(k::Rotation, tr("Rotation"), 0, 45, 1, 0.1, kDegrees),
            percent(k::Zoom, tr("Zoom"), 100, 200, 105),
            boolean(k::MotionBlur, tr("Motion Blur"), true),
            integer(k::Seed, tr("Random Seed"), 0, 9999, 1),
        }));

    // ---- Time ----
    add(effect(fxid::Echo, tr("Echo"), g::Time,
        tr("Blends earlier frames of the clip over it: trails and ghosting"),
        {
            integer(k::Echoes, tr("Echoes"), 1, 4, 3),
            integer(k::Delay, tr("Delay"), 1, 30, 2, QStringLiteral("fr")),
            percent(k::Decay, tr("Decay"), 0, 100, 50),
        }));
    add(effect(fxid::Strobe, tr("Strobe"), g::Time,
        tr("Holds every Nth frame: a choppy, stop-motion look"),
        {integer(k::Hold, tr("Hold Frames"), 1, 60, 3, QStringLiteral("fr"))}));
    add(effect(fxid::Flicker, tr("Flicker"), g::Time,
        tr("Brightness that flickers like an old projector or a failing lamp"),
        {
            percent(k::Amount, tr("Amount"), 0, 100, 30),
            number(k::Frequency, tr("Frequency"), 0.1, 30, 8, 0.05, kHz),
            percent(k::Randomness, tr("Randomness"), 0, 100, 70),
            integer(k::Seed, tr("Random Seed"), 0, 9999, 1),
        }));

    // ---- Transform ----
    add(effect(fxid::Tile, tr("Tile"), g::Transform,
        tr("Repeats the picture in smaller copies, optionally mirrored at the edges"),
        {
            percent(k::Zoom, tr("Tile Size"), 5, 100, 50),
            center(k::OffsetX, tr("Offset X")),
            center(k::OffsetY, tr("Offset Y")),
            boolean(k::Mirror, tr("Mirror Edges"), false),
        }));
}

} // namespace kk
