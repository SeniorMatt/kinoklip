// SPDX-License-Identifier: GPL-3.0-or-later
// The third batch of video effects (1.2, docs/behavior-spec.md section 56): Colored Border,
// Despill, Alpha Matte Shrink and Grow, Transform, Dent, Fast Noise, Gamut Limiter, Contrast
// Pop, Chromatic Aberration Fix. Pixel code: src/render (GlExtraEffects.cpp, GlExtraColor.cpp;
// CPU reference EffectMathExtra.cpp).
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

EffectDescriptor effect(const char* id, const QString& name, const char* group,
    const QString& description, QVector<ParamDescriptor> params)
{
    EffectDescriptor d = descriptor(
        QLatin1String(id), name, EffectCategory::VideoEffect, description, std::move(params));
    d.group = QLatin1String(group);
    return d;
}

ParamDescriptor percent(
    const QString& key, const QString& label, double min, double max, double def)
{
    return number(key, label, min, max, def, 0.5, kPercent);
}

/// A whole-number parameter that cannot be keyframed (a seed, an octave count).
ParamDescriptor fixedInt(const QString& key, const QString& label, int min, int max, int def)
{
    ParamDescriptor p = integer(key, label, min, max, def);
    p.animatable = false;
    return p;
}

ParamDescriptor center(const QString& key, const QString& label)
{
    return number(key, label, -10000, 10000, 0, 1, kPx);
}

} // namespace

void EffectRegistry::registerVideoEffects3()
{
    namespace k = fxkeys;
    namespace c = fxchoice;
    namespace g = effectgroups;

    // ---- Stylize ----
    add(effect(fxid::ColoredBorder, tr("Colored Border"), g::Stylize,
        tr("Paints a colored frame inside the edges of the picture"),
        {
            color(k::Color, tr("Color"), QColor(0xf2, 0xb1, 0x34)),
            number(k::Width, tr("Width"), 0, 1000, 24, 0.5, kPx),
            number(k::Radius, tr("Corner Radius"), 0, 1000, 0, 0.5, kPx),
            number(k::Feather, tr("Feather"), 0, 500, 0, 0.5, kPx),
            percent(k::Opacity, tr("Opacity"), 0, 100, 100),
        }));

    // ---- Color ----
    add(effect(fxid::Despill, tr("Despill"), g::Color,
        tr("Removes a red, green or blue color cast left by a screen"),
        {
            choice(k::Screen, tr("Cast"), {c::Green, c::Blue, c::Red},
                {tr("Green"), tr("Blue"), tr("Red")}, c::Green),
            percent(k::Amount, tr("Strength"), 0, 100, 100),
            choice(k::SpillMethod, tr("Method"), {c::Average, c::Maximum},
                {tr("Average"), tr("Maximum")}, c::Average),
        }));
    add(effect(fxid::GamutLimiter, tr("Gamut Limiter"), g::Color,
        tr("Pulls colors outside a smaller gamut back inside it, keeping their brightness"),
        {
            choice(k::SourceGamut, tr("Picture Gamut"), {c::Rec709, c::DciP3, c::Rec2020},
                {tr("Rec. 709"), tr("DCI-P3 D65"), tr("Rec. 2020")}, c::Rec2020),
            choice(k::TargetGamut, tr("Limit To"), {c::Rec709, c::DciP3, c::Rec2020},
                {tr("Rec. 709"), tr("DCI-P3 D65"), tr("Rec. 2020")}, c::Rec709),
            choice(k::Mode, tr("Method"), {c::Desaturate, c::Clip}, {tr("Desaturate"), tr("Clip")},
                c::Desaturate),
            number(k::Gamma, tr("Gamma"), 1.0, 3.0, 2.4, 0.05),
            percent(k::Amount, tr("Amount"), 0, 100, 100),
        }));
    add(effect(fxid::ContrastPop, tr("Contrast Pop"), g::Color,
        tr("Local contrast: adds punch to the detail of mid-sized shapes without changing the "
           "overall tones"),
        {
            percent(k::Amount, tr("Strength"), -100, 300, 60),
            number(k::Radius, tr("Radius"), 1, 500, 40, 0.5, kPx),
            percent(k::Saturation, tr("Saturation"), -100, 100, 0),
        }));
    add(effect(fxid::ChromaticAberrationFix, tr("Chromatic Aberration Fix"), g::Color,
        tr("Corrects color fringes at the edges of a lens image by scaling red and blue"),
        {
            number(k::RedShift, tr("Red Scale"), -2, 2, 0, 0.01, kPercent),
            number(k::BlueShift, tr("Blue Scale"), -2, 2, 0, 0.01, kPercent),
            percent(k::Defringe, tr("Fringe Removal"), 0, 100, 0),
            center(k::CenterX, tr("Center X")),
            center(k::CenterY, tr("Center Y")),
        }));

    add(effect(fxid::FalseColor, tr("False Color"), g::Color,
        tr("Paints the picture by brightness: exposure bands, a thermal scale or a rainbow"),
        {
            choice(k::Palette, tr("Palette"), {c::Exposure, c::Thermal, c::Rainbow},
                {tr("Exposure Bands"), tr("Thermal"), tr("Rainbow")}, c::Exposure),
            percent(k::Amount, tr("Amount"), 0, 100, 100),
        }));
    add(effect(fxid::Dehaze, tr("Dehaze"), g::Color,
        tr("Takes the fog or haze out of the picture, from the darkest colors of each area"),
        {
            percent(k::Amount, tr("Strength"), 0, 100, 60),
            number(k::Radius, tr("Area Size"), 1, 200, 15, 0.5, kPx),
            color(k::HazeColor, tr("Haze Color"), QColor(0xd8, 0xdc, 0xe2)),
            number(k::Softness, tr("Softness"), 0, 100, 8, 0.5, kPx),
        }));

    // ---- Key ----
    add(effect(fxid::AlphaMatteShrinkGrow, tr("Alpha Matte Shrink and Grow"), g::Key,
        tr("Shrinks or grows the transparency edge of the picture, with an optional blur"),
        {
            number(k::ShrinkGrow, tr("Shrink / Grow"), -100, 100, 0, 0.1, kPx),
            number(k::EdgeBlur, tr("Edge Blur"), 0, 100, 0, 0.1, kPx),
        }));

    // ---- Transform ----
    add(effect(fxid::Transform, tr("Transform"), g::Transform,
        tr("Moves, scales and rotates the picture, as an effect that can sit anywhere in the "
           "stack"),
        {
            center(k::OffsetX, tr("Position X")),
            center(k::OffsetY, tr("Position Y")),
            percent(k::Scale, tr("Scale"), 1, 1000, 100),
            number(k::Rotation, tr("Rotation"), -3600, 3600, 0, 0.5, kDegrees),
            center(k::CenterX, tr("Pivot X")),
            center(k::CenterY, tr("Pivot Y")),
            percent(k::Opacity, tr("Opacity"), 0, 100, 100),
        }));

    // ---- Distort ----
    add(effect(fxid::Dent, tr("Dent"), g::Distort,
        tr("Bows the picture out or in around a point, round, along one direction or square"),
        {
            choice(k::Mode, tr("Type"),
                {c::Bulge, c::Pinch, c::BowHorizontal, c::BowVertical, c::SquareBulge,
                    c::SquarePinch},
                {tr("Round Bulge"), tr("Round Pinch"), tr("Horizontal Bow"), tr("Vertical Bow"),
                    tr("Square Bulge"), tr("Square Pinch")},
                c::Bulge),
            percent(k::Amount, tr("Strength"), 0, 100, 50),
            number(k::Size, tr("Size"), 1, 10000, 400, 1, kPx),
            center(k::CenterX, tr("Center X")),
            center(k::CenterY, tr("Center Y")),
            choice(k::Edges, tr("Edges"), {c::Transparent, c::Repeat, c::Mirrored},
                {tr("Transparent"), tr("Repeat"), tr("Mirrored")}, c::Repeat),
        }));

    // ---- Texture ----
    add(effect(fxid::FastNoise, tr("Fast Noise"), g::Texture,
        tr("Cloudy noise laid over the picture, still or drifting"),
        {
            color(k::ShadowColor, tr("Color 1"), QColor(Qt::black)),
            color(k::HighlightColor, tr("Color 2"), QColor(Qt::white)),
            number(k::Scale, tr("Scale"), 4, 4000, 200, 1, kPx),
            fixedInt(k::Detail, tr("Detail"), 1, 8, 4),
            percent(k::Contrast, tr("Contrast"), 0, 500, 200),
            number(k::Evolution, tr("Evolution"), -10000, 10000, 0, 0.05),
            number(k::Speed, tr("Drift Speed"), -20, 20, 0, 0.05),
            fixedInt(k::Seed, tr("Seed"), 0, 99999, 1),
            choice(k::Mode, tr("Blend Mode"),
                {c::Normal, c::Add, c::Multiply, c::Screen, c::Overlay},
                {tr("Normal"), tr("Add"), tr("Multiply"), tr("Screen"), tr("Overlay")}, c::Overlay),
            percent(k::Amount, tr("Blend"), 0, 100, 50),
        }));
}

} // namespace kk
