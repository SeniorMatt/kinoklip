// SPDX-License-Identifier: GPL-3.0-or-later
// The built-in v0.1 catalogue: clip property groups, video effects, transitions, generators.
// To add an effect: append a descriptor here, then implement it in src/render keyed by its id.
#include "core/EffectRegistry.h"
#include "core/ParamKeys.h"

#include <QCoreApplication>

namespace cw {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("cw::BuiltinEffects", text);
}

// ---- Descriptor builders (keep the catalogue below readable) ----

ParamDescriptor number(const char* key, const QString& label, double min, double max, double def,
    double step, const QString& unit = {})
{
    ParamDescriptor p;
    p.key = QLatin1String(key);
    p.label = label;
    p.type = ParamType::Double;
    p.min = min;
    p.max = max;
    p.def = def;
    p.step = step;
    p.unit = unit;
    return p;
}

ParamDescriptor integer(
    const char* key, const QString& label, int min, int max, int def, const QString& unit = {})
{
    ParamDescriptor p = number(key, label, min, max, def, 1.0, unit);
    p.type = ParamType::Int;
    return p;
}

ParamDescriptor boolean(const char* key, const QString& label, bool def)
{
    ParamDescriptor p;
    p.key = QLatin1String(key);
    p.label = label;
    p.type = ParamType::Bool;
    p.defStatic = def;
    p.animatable = false;
    return p;
}

ParamDescriptor color(const char* key, const QString& label, const QColor& def)
{
    ParamDescriptor p;
    p.key = QLatin1String(key);
    p.label = label;
    p.type = ParamType::Color;
    p.defStatic = def;
    p.animatable = false;
    return p;
}

ParamDescriptor choice(const char* key, const QString& label, const QStringList& values,
    const QStringList& labels, const QString& def)
{
    ParamDescriptor p;
    p.key = QLatin1String(key);
    p.label = label;
    p.type = ParamType::Choice;
    p.choices = values;
    p.choiceLabels = labels;
    p.defStatic = def;
    p.animatable = false;
    return p;
}

ParamDescriptor text(const char* key, const QString& label, const QString& def)
{
    ParamDescriptor p;
    p.key = QLatin1String(key);
    p.label = label;
    p.type = ParamType::String;
    p.defStatic = def;
    p.animatable = false;
    return p;
}

EffectDescriptor descriptor(const char* id, const QString& name, EffectCategory category,
    const QString& description, QVector<ParamDescriptor> params, const QString& storage = {})
{
    EffectDescriptor d;
    d.id = QLatin1String(id);
    d.name = name;
    d.category = category;
    d.description = description;
    d.storage = storage;
    d.params = std::move(params);
    return d;
}

// Common unit suffixes.
const QString kPx = QStringLiteral("px");
const QString kPercent = QStringLiteral("%");
const QString kDegrees = QStringLiteral("°");

QStringList directions()
{
    return {QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("up"),
        QStringLiteral("down")};
}

QStringList directionLabels()
{
    return {tr("Left"), tr("Right"), tr("Up"), tr("Down")};
}

} // namespace

void EffectRegistry::registerBuiltins()
{
    using C = EffectCategory;

    // ---- Clip property groups (stored on ClipData, always present in the Inspector) ----
    add(descriptor(groups::Transform, tr("Transform"), C::ClipProperties,
        tr("Position, zoom, rotation, anchor point and opacity"),
        {
            number(keys::ZoomX, tr("Zoom X"), 0.01, 20.0, 1.0, 0.01),
            number(keys::ZoomY, tr("Zoom Y"), 0.01, 20.0, 1.0, 0.01),
            boolean(keys::ZoomLinked, tr("Link Zoom"), true),
            number(keys::PosX, tr("Position X"), -10000, 10000, 0, 1, kPx),
            number(keys::PosY, tr("Position Y"), -10000, 10000, 0, 1, kPx),
            number(keys::Rotation, tr("Rotation"), -3600, 3600, 0, 0.1, kDegrees),
            number(keys::AnchorX, tr("Anchor X"), -10000, 10000, 0, 1, kPx),
            number(keys::AnchorY, tr("Anchor Y"), -10000, 10000, 0, 1, kPx),
            boolean(keys::FlipH, tr("Flip Horizontal"), false),
            boolean(keys::FlipV, tr("Flip Vertical"), false),
            number(keys::Opacity, tr("Opacity"), 0, 100, 100, 1, kPercent),
        },
        QStringLiteral("transform")));
    add(descriptor(groups::Cropping, tr("Cropping"), C::ClipProperties,
        tr("Cut pixels away from the edges of the source"),
        {
            number(keys::CropL, tr("Crop Left"), 0, 10000, 0, 1, kPx),
            number(keys::CropR, tr("Crop Right"), 0, 10000, 0, 1, kPx),
            number(keys::CropT, tr("Crop Top"), 0, 10000, 0, 1, kPx),
            number(keys::CropB, tr("Crop Bottom"), 0, 10000, 0, 1, kPx),
        },
        QStringLiteral("transform")));
    add(descriptor(groups::Audio, tr("Audio"), C::ClipProperties, tr("Clip volume and pan"),
        {
            number(keys::VolumeDb, tr("Volume"), -96, 24, 0, 0.1, QStringLiteral("dB")),
            number(keys::Pan, tr("Pan"), -1, 1, 0, 0.01),
        },
        QStringLiteral("audio")));

    // ---- Video effects (ClipData::effects) ----
    add(descriptor("gaussian_blur", tr("Gaussian Blur"), C::VideoEffect,
        tr("Soft blur of the whole image"), {number("radius", tr("Radius"), 0, 100, 5, 0.1, kPx)}));
    add(descriptor("brightness_contrast", tr("Brightness / Contrast"), C::VideoEffect,
        tr("Simple brightness and contrast adjustment"),
        {
            number("brightness", tr("Brightness"), -100, 100, 0, 1),
            number("contrast", tr("Contrast"), -100, 100, 0, 1),
        }));
    add(descriptor("saturation", tr("Saturation"), C::VideoEffect,
        tr("Make colours more or less intense (0 = greyscale)"),
        {number("saturation", tr("Saturation"), 0, 200, 100, 1, kPercent)}));
    add(descriptor("sharpen", tr("Sharpen"), C::VideoEffect, tr("Unsharp-mask sharpening"),
        {
            number("amount", tr("Amount"), 0, 100, 50, 1, kPercent),
            number("radius", tr("Radius"), 0.5, 10, 1, 0.1, kPx),
        }));
    add(descriptor("mirror", tr("Mirror / Flip"), C::VideoEffect,
        tr("Flip the image horizontally and/or vertically"),
        {
            boolean("horizontal", tr("Horizontal"), true),
            boolean("vertical", tr("Vertical"), false),
        }));
    add(descriptor("vignette", tr("Vignette"), C::VideoEffect, tr("Darken the image corners"),
        {
            number("amount", tr("Amount"), 0, 100, 50, 1, kPercent),
            number("size", tr("Size"), 0, 100, 50, 1, kPercent),
            number("softness", tr("Softness"), 0, 100, 50, 1, kPercent),
        }));
    add(descriptor("mosaic", tr("Mosaic"), C::VideoEffect, tr("Pixelate the image into blocks"),
        {integer("blockSize", tr("Block Size"), 2, 200, 20, kPx)}));
    add(descriptor("drop_shadow", tr("Drop Shadow"), C::VideoEffect,
        tr("Shadow behind the clip's alpha (titles, scaled-down overlays)"),
        {
            number("distance", tr("Distance"), 0, 200, 10, 1, kPx),
            number("angle", tr("Angle"), -180, 180, 135, 1, kDegrees),
            number("blur", tr("Blur"), 0, 100, 8, 0.1, kPx),
            number("opacity", tr("Opacity"), 0, 100, 60, 1, kPercent),
            color("color", tr("Colour"), QColor(Qt::black)),
        }));

    // ---- Transitions (ClipData::transitionIn / transitionOut) ----
    add(descriptor("cross_dissolve", tr("Cross Dissolve"), C::Transition,
        tr("Blend from one clip into the next"), {}));
    add(descriptor("dip_to_color", tr("Dip to Colour"), C::Transition,
        tr("Fade through a solid colour"), {color("color", tr("Colour"), QColor(Qt::black))}));
    add(descriptor("wipe", tr("Wipe"), C::Transition, tr("Reveal the next clip with a moving edge"),
        {
            choice("direction", tr("Direction"), directions(), directionLabels(),
                QStringLiteral("left")),
            number("softness", tr("Softness"), 0, 100, 10, 1, kPercent),
        }));
    add(descriptor("push", tr("Push"), C::Transition,
        tr("The next clip pushes the current one out"),
        {choice("direction", tr("Direction"), directions(), directionLabels(),
            QStringLiteral("left"))}));

    // ---- Generators & titles (clip kinds of their own) ----
    add(descriptor("solid_color", tr("Solid Colour"), C::Generator, tr("A full-frame solid colour"),
        {color("color", tr("Colour"), QColor(0x30, 0x30, 0x34))}, QStringLiteral("solidColor")));
    add(descriptor("text_title", tr("Text"), C::Title, tr("A simple text title"),
        {
            text(keys::Text, tr("Text"), tr("Title")),
            text(keys::Font, tr("Font"), QStringLiteral("Sans Serif")),
            number(keys::Size, tr("Size"), 1, 500, 72, 1, kPx),
            boolean(keys::Bold, tr("Bold"), false),
            boolean(keys::Italic, tr("Italic"), false),
            choice(keys::Align, tr("Alignment"),
                {QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right")},
                {tr("Left"), tr("Centre"), tr("Right")}, QStringLiteral("center")),
            color(keys::Color, tr("Colour"), QColor(Qt::white)),
            number(keys::Tracking, tr("Tracking"), -50, 200, 0, 1, kPercent),
            number(keys::LineSpacing, tr("Line Spacing"), 50, 300, 100, 1, kPercent),
            color(keys::OutlineColor, tr("Outline Colour"), QColor(Qt::black)),
            number(keys::OutlineWidth, tr("Outline Width"), 0, 50, 0, 0.5, kPx),
            boolean(keys::BgEnabled, tr("Background"), false),
            color(keys::BgColor, tr("Background Colour"), QColor(Qt::black)),
            number(keys::BgOpacity, tr("Background Opacity"), 0, 100, 60, 1, kPercent),
            boolean(keys::Shadow, tr("Drop Shadow"), false),
        },
        QStringLiteral("title")));
}

} // namespace cw
