// SPDX-License-Identifier: GPL-3.0-or-later
// The v4 video effects catalogue: keyers (chroma, luma, HSL), colour, blurs, light, stylize,
// distort and transform effects. Pixel code: src/render (GlKeyers.cpp, GlColorEffects.cpp,
// GlBlurEffects.cpp, GlStylizeEffects.cpp, GlDistortEffects.cpp; CPU reference EffectMath.h).
// Behaviour: docs/behavior-spec.md section 40.
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

/// Default key colour of the chroma key (a typical green screen) — also the green preset.
const QColor kDefaultKeyColor(0x00, 0xb1, 0x40);

/// Descriptor of @p id in library folder @p group.
EffectDescriptor effect(const char* id, const QString& name, const char* group,
    const QString& description, QVector<ParamDescriptor> params)
{
    EffectDescriptor d = descriptor(
        QLatin1String(id), name, EffectCategory::VideoEffect, description, std::move(params));
    d.group = QLatin1String(group);
    return d;
}

/// The keyers' view mode: the result, the matte as greyscale, or the matte's status.
ParamDescriptor viewMode()
{
    return choice(fxkeys::View, tr("View"), {fxchoice::Final, fxchoice::Matte, fxchoice::Status},
        {tr("Final Result"), tr("Matte"), tr("Status")}, fxchoice::Final);
}

/// A centre offset from the middle of the clip (timeline pixels).
ParamDescriptor center(const QString& key, const QString& label)
{
    return number(key, label, -10000, 10000, 0, 1, kPx);
}

} // namespace

void EffectRegistry::registerVideoEffects()
{
    namespace k = fxkeys;
    namespace g = effectgroups;

    // ---- Key ----
    ParamDescriptor keyColor = color(k::KeyColor, tr("Key Color"), kDefaultKeyColor);
    keyColor.viewerPicker = true;
    add(effect(fxid::ChromaKey, tr("Chroma Key"), g::Key,
        tr("Green / blue screen: makes the key color transparent"),
        {
            choice(k::Screen, tr("Screen"), {fxchoice::Green, fxchoice::Blue, fxchoice::Custom},
                {tr("Green Screen"), tr("Blue Screen"), tr("Custom (Key Color)")}, fxchoice::Green),
            keyColor,
            number(k::Tolerance, tr("Tolerance"), 0, 100, 30, 0.5, kPercent),
            number(k::Softness, tr("Softness"), 0, 100, 20, 0.5, kPercent),
            number(k::ClipBlack, tr("Clip Black"), 0, 100, 0, 0.5, kPercent),
            number(k::ClipWhite, tr("Clip White"), 0, 100, 100, 0.5, kPercent),
            number(k::ShrinkGrow, tr("Shrink / Grow"), -20, 20, 0, 0.1, kPx),
            number(k::EdgeBlur, tr("Edge Blur"), 0, 20, 0, 0.1, kPx),
            number(k::SpillAmount, tr("Spill Removal"), 0, 100, 50, 1, kPercent),
            choice(k::SpillMethod, tr("Spill Method"), {fxchoice::Average, fxchoice::Maximum},
                {tr("Average"), tr("Maximum")}, fxchoice::Average),
            boolean(k::Invert, tr("Invert"), false),
            viewMode(),
        }));
    add(effect(fxid::LumaKey, tr("Luma Key"), g::Key,
        tr("Keeps the brightness range between Low and High, the rest becomes transparent"),
        {
            number(k::Low, tr("Low"), 0, 100, 20, 0.5, kPercent),
            number(k::High, tr("High"), 0, 100, 100, 0.5, kPercent),
            number(k::Softness, tr("Softness"), 0, 100, 10, 0.5, kPercent),
            boolean(k::Invert, tr("Invert"), false),
            viewMode(),
        }));
    add(effect(fxid::HslKey, tr("HSL Key"), g::Key,
        tr("Makes a hue, saturation and luminance range transparent"),
        {
            number(k::Hue, tr("Hue"), 0, 360, 120, 1, kDegrees),
            number(k::HueWidth, tr("Hue Width"), 0, 360, 60, 1, kDegrees),
            number(k::HueSoftness, tr("Hue Softness"), 0, 180, 20, 1, kDegrees),
            number(k::SatLow, tr("Saturation Low"), 0, 100, 20, 0.5, kPercent),
            number(k::SatHigh, tr("Saturation High"), 0, 100, 100, 0.5, kPercent),
            number(k::SatSoftness, tr("Saturation Softness"), 0, 100, 10, 0.5, kPercent),
            number(k::LumLow, tr("Luminance Low"), 0, 100, 0, 0.5, kPercent),
            number(k::LumHigh, tr("Luminance High"), 0, 100, 100, 0.5, kPercent),
            number(k::LumSoftness, tr("Luminance Softness"), 0, 100, 10, 0.5, kPercent),
            boolean(k::Invert, tr("Invert"), false),
            viewMode(),
        }));

    // ---- Color ----
    add(effect(fxid::Exposure, tr("Exposure"), g::Color, tr("Brightens or darkens in camera stops"),
        {
            number(k::Exposure, tr("Exposure"), -5, 5, 0, 0.01),
            number(k::Gamma, tr("Gamma"), 0.2, 5, 1, 0.01),
        }));
    add(effect(fxid::TemperatureTint, tr("Temperature / Tint"), g::Color,
        tr("Warmer / cooler (blue - orange) and green - magenta white balance"),
        {
            number(k::Temperature, tr("Temperature"), -100, 100, 0, 1),
            number(k::Tint, tr("Tint"), -100, 100, 0, 1),
        }));
    add(effect(fxid::Levels, tr("Levels"), g::Color,
        tr("Input and output black / white points and gamma"),
        {
            number(k::InBlack, tr("Input Black"), 0, 255, 0, 1),
            number(k::InWhite, tr("Input White"), 0, 255, 255, 1),
            number(k::Gamma, tr("Gamma"), 0.1, 10, 1, 0.01),
            number(k::OutBlack, tr("Output Black"), 0, 255, 0, 1),
            number(k::OutWhite, tr("Output White"), 0, 255, 255, 1),
        }));
    add(effect(fxid::Invert, tr("Invert"), g::Color, tr("Negative image"),
        {number(k::Amount, tr("Amount"), 0, 100, 100, 1, kPercent)}));
    add(effect(fxid::BlackWhite, tr("Black and White"), g::Color,
        tr("Greyscale, optionally tinted (sepia, cyanotype, ...)"),
        {
            number(k::Amount, tr("Amount"), 0, 100, 100, 1, kPercent),
            color(k::TintColor, tr("Tint Color"), QColor(0xc8, 0xa0, 0x70)),
            number(k::TintAmount, tr("Tint Amount"), 0, 100, 0, 1, kPercent),
        }));
    add(effect(fxid::Posterize, tr("Posterize"), g::Color,
        tr("Reduces every color channel to a few levels"),
        {integer(k::Levels, tr("Levels"), 2, 64, 6)}));
    add(effect(fxid::ColorBalance, tr("Color Balance"), g::Color,
        tr("Lift (shadows), gamma (midtones) and gain (highlights), overall and per channel"),
        {
            number(k::Lift, tr("Lift"), -100, 100, 0, 0.5),
            number(k::LiftR, tr("Lift Red"), -100, 100, 0, 0.5),
            number(k::LiftG, tr("Lift Green"), -100, 100, 0, 0.5),
            number(k::LiftB, tr("Lift Blue"), -100, 100, 0, 0.5),
            number(k::Gamma, tr("Gamma"), -100, 100, 0, 0.5),
            number(k::GammaR, tr("Gamma Red"), -100, 100, 0, 0.5),
            number(k::GammaG, tr("Gamma Green"), -100, 100, 0, 0.5),
            number(k::GammaB, tr("Gamma Blue"), -100, 100, 0, 0.5),
            number(k::Gain, tr("Gain"), 0, 400, 100, 0.5, kPercent),
            number(k::GainR, tr("Gain Red"), -100, 100, 0, 0.5, kPercent),
            number(k::GainG, tr("Gain Green"), -100, 100, 0, 0.5, kPercent),
            number(k::GainB, tr("Gain Blue"), -100, 100, 0, 0.5, kPercent),
        }));

    // ---- Blur ----
    add(effect(fxid::BoxBlur, tr("Box Blur"), g::Blur, tr("Even blur over a square area"),
        {number(k::Radius, tr("Radius"), 0, 100, 5, 0.1, kPx)}));
    add(effect(fxid::DirectionalBlur, tr("Directional Blur"), g::Blur,
        tr("Motion blur along one direction"),
        {
            number(k::Length, tr("Length"), 0, 500, 20, 0.5, kPx),
            number(k::Angle, tr("Angle"), -180, 180, 0, 0.5, kDegrees),
        }));
    add(effect(fxid::RadialBlur, tr("Radial Blur"), g::Blur, tr("Spin blur around a centre"),
        {
            number(k::Angle, tr("Angle"), 0, 180, 10, 0.1, kDegrees),
            center(k::CenterX, tr("Center X")),
            center(k::CenterY, tr("Center Y")),
        }));
    add(effect(fxid::ZoomBlur, tr("Zoom Blur"), g::Blur,
        tr("Blur streaking out of a centre, like a fast zoom"),
        {
            number(k::Amount, tr("Amount"), 0, 100, 20, 0.5, kPercent),
            center(k::CenterX, tr("Center X")),
            center(k::CenterY, tr("Center Y")),
        }));

    // ---- Light ----
    add(effect(fxid::Glow, tr("Glow"), g::Light, tr("Bright areas bleed soft light"),
        {
            number(k::Threshold, tr("Threshold"), 0, 100, 70, 0.5, kPercent),
            number(k::Radius, tr("Radius"), 0, 200, 20, 0.5, kPx),
            number(k::Intensity, tr("Intensity"), 0, 400, 100, 1, kPercent),
            color(k::Color, tr("Color"), QColor(Qt::white)),
        }));

    // ---- Stylize ----
    add(effect(fxid::FilmGrain, tr("Film Grain"), g::Stylize, tr("Adds film-like grain"),
        {
            number(k::Amount, tr("Amount"), 0, 100, 30, 1, kPercent),
            number(k::Size, tr("Grain Size"), 0.5, 10, 1.5, 0.1, kPx),
            boolean(k::Monochrome, tr("Monochrome"), true),
            boolean(k::Animated, tr("Animated"), true),
        }));
    add(effect(fxid::ChromaticAberration, tr("Chromatic Aberration"), g::Stylize,
        tr("Red and blue fringes towards the edges, like a cheap lens"),
        {number(k::Amount, tr("Amount"), -50, 50, 5, 0.1, kPx)}));
    add(effect(fxid::EdgeDetect, tr("Edge Detect"), g::Stylize,
        tr("Bright lines where the picture has edges"),
        {
            number(k::Strength, tr("Strength"), 0, 400, 100, 1, kPercent),
            number(k::Amount, tr("Amount"), 0, 100, 100, 1, kPercent),
            boolean(k::Invert, tr("Invert"), false),
        }));
    add(effect(fxid::Emboss, tr("Emboss"), g::Stylize, tr("Relief look lit from one side"),
        {
            number(k::Angle, tr("Light Angle"), -180, 180, 135, 1, kDegrees),
            number(k::Height, tr("Height"), 0, 20, 2, 0.1, kPx),
            number(k::Amount, tr("Amount"), 0, 100, 100, 1, kPercent),
        }));
    add(effect(fxid::Stroke, tr("Stroke"), g::Stylize,
        tr("A colored border around the clip's shape (titles, keyed or scaled-down clips)"),
        {
            number(k::Width, tr("Width"), 0, 100, 4, 0.5, kPx),
            color(k::Color, tr("Color"), QColor(Qt::white)),
            choice(k::Position, tr("Position"), {fxchoice::Outside, fxchoice::Inside},
                {tr("Outside"), tr("Inside")}, fxchoice::Outside),
            number(k::Opacity, tr("Opacity"), 0, 100, 100, 1, kPercent),
        }));

    // ---- Distort ----
    add(effect(fxid::LensDistortion, tr("Lens Distortion"), g::Distort,
        tr("Barrel (positive) or pincushion (negative) distortion"),
        {
            number(k::Distortion, tr("Distortion"), -100, 100, 30, 0.5),
            number(k::Zoom, tr("Zoom"), 50, 200, 100, 0.5, kPercent),
        }));
    add(effect(fxid::Ripple, tr("Ripple"), g::Distort, tr("Circular waves from a centre"),
        {
            number(k::Amplitude, tr("Amplitude"), 0, 200, 10, 0.5, kPx),
            number(k::Wavelength, tr("Wavelength"), 4, 1000, 60, 1, kPx),
            number(k::Speed, tr("Speed"), -10, 10, 1, 0.05, QStringLiteral("Hz")),
            number(k::Phase, tr("Phase"), -3600, 3600, 0, 1, kDegrees),
            center(k::CenterX, tr("Center X")),
            center(k::CenterY, tr("Center Y")),
        }));
    add(effect(fxid::Wave, tr("Wave"), g::Distort, tr("Straight waves across the picture"),
        {
            number(k::Amplitude, tr("Amplitude"), 0, 200, 10, 0.5, kPx),
            number(k::Wavelength, tr("Wavelength"), 4, 2000, 120, 1, kPx),
            number(k::Angle, tr("Direction"), -180, 180, 0, 1, kDegrees),
            number(k::Speed, tr("Speed"), -10, 10, 1, 0.05, QStringLiteral("Hz")),
            number(k::Phase, tr("Phase"), -3600, 3600, 0, 1, kDegrees),
        }));

    // ---- Transform ----
    add(effect(fxid::FeatheredCrop, tr("Feathered Crop"), g::Transform,
        tr("Crops the edges with a soft feather (percent of the clip size)"),
        {
            number(k::Left, tr("Left"), 0, 100, 0, 0.1, kPercent),
            number(k::Right, tr("Right"), 0, 100, 0, 0.1, kPercent),
            number(k::Top, tr("Top"), 0, 100, 0, 0.1, kPercent),
            number(k::Bottom, tr("Bottom"), 0, 100, 0, 0.1, kPercent),
            number(k::Feather, tr("Feather"), 0, 1000, 20, 0.5, kPx),
        }));
}

} // namespace kk
