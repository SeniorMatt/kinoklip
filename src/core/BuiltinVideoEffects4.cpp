// SPDX-License-Identifier: GPL-3.0-or-later
// The fourth batch of video effects (1.2, docs/behavior-spec.md section 57): Sharpen Edges, Soften
// and Sharpen, Texture Pop, Detail Recovery, Deband, Dead Pixel Fixer, Beauty, Analog Damage,
// Film Damage, JPEG Damage, Watercolor, Stylize, Aperture Diffraction, Lens Reflections, Color
// Palette, 3D Keyer, Video Collage. Pixel code: src/render (GlDetailEffects.cpp,
// GlDamageEffects.cpp, GlPaintEffects.cpp, GlOpticsEffects.cpp, GlUtilityEffects.cpp).
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

ParamDescriptor fixedInt(const QString& key, const QString& label, int min, int max, int def)
{
    ParamDescriptor p = integer(key, label, min, max, def);
    p.animatable = false;
    return p;
}

ParamDescriptor seed()
{
    return fixedInt(fxkeys::Seed, tr("Seed"), 0, 99999, 1);
}

ParamDescriptor keyView()
{
    namespace c = fxchoice;
    return choice(fxkeys::View, tr("View"), {c::Final, c::Matte, c::Status},
        {tr("Final"), tr("Matte"), tr("Status")}, c::Final);
}

} // namespace

void EffectRegistry::registerVideoEffects4()
{
    namespace k = fxkeys;
    namespace c = fxchoice;
    namespace g = effectgroups;

    // ---- Sharpen ----
    add(effect(fxid::SharpenEdges, tr("Sharpen Edges"), g::Sharpen,
        tr("Sharpens only the real edges of the picture and leaves flat areas and fine texture "
           "alone"),
        {
            percent(k::Amount, tr("Strength"), 0, 500, 100),
            number(k::Radius, tr("Radius"), 0.5, 20, 1.5, 0.1, kPx),
            percent(k::Threshold, tr("Edge Threshold"), 0, 100, 10),
            percent(k::HaloLimit, tr("Halo Limit"), 0, 100, 25),
        }));
    add(effect(fxid::SoftenSharpen, tr("Soften and Sharpen"), g::Sharpen,
        tr("Softens fine texture such as skin pores while sharpening the larger edges"),
        {
            percent(k::Soften, tr("Soften"), 0, 100, 50),
            percent(k::Sharpen, tr("Sharpen"), 0, 300, 60),
            number(k::Radius, tr("Radius"), 0.5, 20, 2, 0.1, kPx),
            percent(k::Threshold, tr("Texture Limit"), 0, 100, 6),
        }));

    // ---- Texture ----
    add(effect(fxid::TexturePop, tr("Texture Pop"), g::Texture,
        tr("Makes the fine texture of surfaces stand out, without touching the large tones"),
        {
            percent(k::Amount, tr("Strength"), -100, 400, 80),
            number(k::Radius, tr("Texture Size"), 0.5, 12, 2, 0.1, kPx),
            percent(k::Highlights, tr("Highlight Protection"), 0, 100, 40),
            boolean(k::LumaOnly, tr("Brightness Only"), true),
        }));
    add(effect(fxid::DetailRecovery, tr("Detail Recovery"), g::Texture,
        tr("Brings back the detail hidden in deep shadows and bright highlights"),
        {
            percent(k::Shadows, tr("Shadows"), 0, 100, 40),
            percent(k::Highlights, tr("Highlights"), 0, 100, 40),
            percent(k::Detail, tr("Detail"), -100, 300, 40),
            number(k::Radius, tr("Radius"), 1, 200, 30, 0.5, kPx),
        }));
    add(effect(fxid::AnalogDamage, tr("Analog Damage"), g::Texture,
        tr("Worn videotape: line jitter, color bleed, noise, a tracking error band and scanlines"),
        {
            number(k::Jitter, tr("Line Jitter"), 0, 60, 6, 0.1, kPx),
            number(k::ColorBleed, tr("Color Bleed"), 0, 60, 8, 0.1, kPx),
            percent(k::Noise, tr("Noise"), 0, 100, 20),
            percent(k::Tracking, tr("Tracking Error"), 0, 100, 25),
            percent(k::ScanlineAmount, tr("Scanlines"), 0, 100, 20),
            seed(),
        }));
    add(effect(fxid::FilmDamage, tr("Film Damage"), g::Texture,
        tr("Old film: dirt, scratches, a soft dark vignette, gate weave and flicker"),
        {
            percent(k::Dirt, tr("Dirt"), 0, 100, 30),
            percent(k::Scratches, tr("Scratches"), 0, 100, 25),
            percent(k::Vignette, tr("Vignette"), 0, 100, 25),
            number(k::Blur, tr("Softness"), 0, 20, 0.8, 0.1, kPx),
            number(k::Weave, tr("Gate Weave"), 0, 20, 1.5, 0.1, kPx),
            percent(k::FlickerAmount, tr("Flicker"), 0, 50, 5),
            seed(),
        }));
    add(effect(fxid::JpegDamage, tr("JPEG Damage"), g::Texture,
        tr("Recompresses the picture as a badly compressed JPEG: blocks, banding and blotchy "
           "color"),
        {
            percent(k::Quality, tr("Quality"), 1, 100, 30),
            number(k::BlockSize, tr("Block Size"), 4, 32, 8, 1, kPx),
        }));

    // ---- Revival ----
    add(effect(fxid::Deband, tr("Deband"), g::Revival,
        tr("Smooths the visible steps in skies and gradients and dithers them away"),
        {
            number(k::Threshold, tr("Threshold"), 0, 100, 8, 0.5),
            number(k::Radius, tr("Radius"), 1, 64, 16, 0.5, kPx),
            percent(k::Dither, tr("Dither"), 0, 100, 40),
        }));
    add(effect(fxid::DeadPixelFixer, tr("Dead Pixel Fixer"), g::Revival,
        tr("Replaces stuck bright or dark pixels with the average of their surroundings"),
        {
            percent(k::Threshold, tr("Threshold"), 1, 100, 25),
            number(k::Size, tr("Defect Size"), 1, 8, 1, 0.5, kPx),
            choice(k::Detect, tr("Detect"), {c::Both, c::Bright, c::Dark},
                {tr("Bright and Dark"), tr("Bright Only"), tr("Dark Only")}, c::Both),
        }));

    // ---- Refine ----
    add(effect(fxid::Beauty, tr("Beauty"), g::Refine,
        tr("Smooths skin and other blotchy areas but keeps the edges; no face detection"),
        {
            percent(k::Smoothing, tr("Smoothing"), 0, 100, 50),
            number(k::Radius, tr("Smoothing Size"), 1, 50, 6, 0.5, kPx),
            percent(k::EdgePreserve, tr("Edge Sensitivity"), 1, 100, 20),
            percent(k::DetailRecover, tr("Detail Recover"), 0, 100, 20),
            boolean(k::SkinOnly, tr("Skin Tones Only"), false),
        }));

    // ---- Stylize ----
    add(effect(fxid::Watercolor, tr("Watercolor"), g::Stylize,
        tr("Paints the picture in flat washes of color with darker pooled edges and paper grain"),
        {
            number(k::Radius, tr("Brush Size"), 1, 20, 5, 0.5, kPx),
            percent(k::EdgeDarkening, tr("Edge Darkening"), 0, 100, 35),
            percent(k::Paper, tr("Paper Texture"), 0, 100, 30),
            percent(k::Saturation, tr("Saturation"), -100, 100, 15),
        }));
    add(effect(fxid::Stylize, tr("Stylize"), g::Stylize,
        tr("Turns the picture into a relief, an ink drawing or a flat cutout"),
        {
            choice(k::Style, tr("Style"), {c::Relief, c::Ink, c::Cutout},
                {tr("Relief"), tr("Ink Outline"), tr("Cutout")}, c::Relief),
            percent(k::Amount, tr("Strength"), 0, 100, 100),
            number(k::Size, tr("Detail Size"), 0.5, 20, 2, 0.1, kPx),
            number(k::Angle, tr("Light Angle"), -360, 360, 135, 1, kDegrees),
            fixedInt(k::Levels, tr("Levels"), 2, 16, 5),
        }));

    // ---- Light ----
    add(effect(fxid::ApertureDiffraction, tr("Aperture Diffraction"), g::Light,
        tr("Starburst spikes from the bright lights of the picture, like a stopped-down lens"),
        {
            fixedInt(k::Points, tr("Spikes"), 2, 16, 6),
            number(k::Length, tr("Length"), 5, 1000, 120, 1, kPx),
            percent(k::Threshold, tr("Threshold"), 0, 100, 80),
            percent(k::Intensity, tr("Intensity"), 0, 500, 100),
            number(k::Rotation, tr("Rotation"), -360, 360, 0, 0.5, kDegrees),
            color(k::Color, tr("Tint"), QColor(Qt::white)),
        }));
    add(effect(fxid::LensReflections, tr("Lens Reflections"), g::Light,
        tr("Faint colored reflections of the bright lights, mirrored through the lens center"),
        {
            fixedInt(k::Ghosts, tr("Reflections"), 1, 8, 4),
            percent(k::Threshold, tr("Threshold"), 0, 100, 70),
            percent(k::Intensity, tr("Intensity"), 0, 300, 80),
            percent(k::Distance, tr("Spread"), 10, 300, 100),
            number(k::Softness, tr("Softness"), 0, 100, 12, 0.5, kPx),
            color(k::Color, tr("Tint"), QColor(0xff, 0xd8, 0xb0)),
            number(k::CenterX, tr("Center X"), -10000, 10000, 0, 1, kPx),
            number(k::CenterY, tr("Center Y"), -10000, 10000, 0, 1, kPx),
        }));

    // ---- Color / Key / Transform ----
    add(effect(fxid::ColorPalette, tr("Color Palette"), g::Color,
        tr("Finds the dominant colors of the picture and shows them as a strip of swatches"),
        {
            fixedInt(k::Count, tr("Colors"), 2, 24, 8),
            choice(k::Position, tr("Position"), {c::PosBottom, c::PosTop, c::PosLeft, c::PosRight},
                {tr("Bottom"), tr("Top"), tr("Left"), tr("Right")}, c::PosBottom),
            percent(k::Size, tr("Strip Size"), 2, 50, 12),
            number(k::Gap, tr("Gap"), 0, 40, 2, 0.5, kPx),
            percent(k::Opacity, tr("Opacity"), 0, 100, 100),
        }));
    add(effect(fxid::Keyer3d, tr("3D Keyer"), g::Key,
        tr("Keys out up to three picked colors in color space, with a tolerance and a soft edge"),
        {
            color(k::KeyColor, tr("Color 1"), QColor(0, 177, 64)),
            boolean(k::UseColor2, tr("Use Color 2"), false),
            color(k::Color2, tr("Color 2"), QColor(0, 71, 187)),
            boolean(k::UseColor3, tr("Use Color 3"), false),
            color(k::Color3, tr("Color 3"), QColor(220, 40, 40)),
            percent(k::Tolerance, tr("Tolerance"), 0, 100, 25),
            percent(k::Softness, tr("Softness"), 0, 100, 15),
            percent(k::LumaWeight, tr("Brightness Weight"), 0, 100, 30),
            percent(k::ClipBlack, tr("Clip Black"), 0, 100, 0),
            percent(k::ClipWhite, tr("Clip White"), 0, 100, 100),
            boolean(k::Invert, tr("Invert"), false),
            keyView(),
        }));
    add(effect(fxid::VideoCollage, tr("Video Collage"), g::Transform,
        tr("Shows the picture again and again in a grid of tiles"),
        {
            fixedInt(k::Columns, tr("Columns"), 1, 12, 3),
            fixedInt(k::Rows, tr("Rows"), 1, 12, 3),
            number(k::Spacing, tr("Spacing"), 0, 400, 8, 0.5, kPx),
            number(k::Radius, tr("Corner Radius"), 0, 400, 0, 0.5, kPx),
            choice(k::Fit, tr("Tile Fit"), {c::Stretch, c::Fill, c::FitInside},
                {tr("Stretch"), tr("Fill"), tr("Fit")}, c::Stretch),
            percent(k::ShadowOpacity, tr("Shadow"), 0, 100, 30),
            color(k::BackgroundColor, tr("Background"), QColor(Qt::black)),
            percent(k::Opacity, tr("Background Opacity"), 0, 100, 0),
        }));
}

} // namespace kk
