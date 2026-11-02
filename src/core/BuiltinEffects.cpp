// SPDX-License-Identifier: GPL-3.0-or-later
// The built-in v0.1 catalogue: clip property groups, video effects, transitions, generators.
// To add an effect: append a descriptor here, then implement it in src/render keyed by its id.
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/ParamKeys.h"

#include <QCoreApplication>

namespace kk {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::BuiltinEffects", text);
}

// ---- Descriptor builders (keep the catalogue below readable) ----

ParamDescriptor number(const QString& key, const QString& label, double min, double max, double def,
    double step, const QString& unit = {})
{
    ParamDescriptor p;
    p.key = key;
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
    const QString& key, const QString& label, int min, int max, int def, const QString& unit = {})
{
    ParamDescriptor p = number(key, label, min, max, def, 1.0, unit);
    p.type = ParamType::Int;
    return p;
}

ParamDescriptor boolean(const QString& key, const QString& label, bool def)
{
    ParamDescriptor p;
    p.key = key;
    p.label = label;
    p.type = ParamType::Bool;
    p.defStatic = def;
    p.animatable = false;
    return p;
}

ParamDescriptor color(const QString& key, const QString& label, const QColor& def)
{
    ParamDescriptor p;
    p.key = key;
    p.label = label;
    p.type = ParamType::Color;
    p.defStatic = def;
    p.animatable = false;
    return p;
}

ParamDescriptor choice(const QString& key, const QString& label, const QStringList& values,
    const QStringList& labels, const QString& def)
{
    ParamDescriptor p;
    p.key = key;
    p.label = label;
    p.type = ParamType::Choice;
    p.choices = values;
    p.choiceLabels = labels;
    p.defStatic = def;
    p.animatable = false;
    return p;
}

ParamDescriptor text(const QString& key, const QString& label, const QString& def)
{
    ParamDescriptor p;
    p.key = key;
    p.label = label;
    p.type = ParamType::String;
    p.defStatic = def;
    p.animatable = false;
    return p;
}

EffectDescriptor descriptor(const QString& id, const QString& name, EffectCategory category,
    const QString& description, QVector<ParamDescriptor> params, const QString& storage = {})
{
    EffectDescriptor d;
    d.id = id;
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

    // Mask parameters (ClipData::masks[i]; the inspector shows one group per mask, the
    // keyframe editors one "Masks" group). Width / height apply to rectangles and ellipses.
    add(descriptor(kMaskDescriptorId, tr("Masks"), C::ClipProperties,
        tr("Shape masks cut the clip's picture before it is composited"),
        {
            choice(maskkeys::Mode, tr("Mode"),
                {QStringLiteral("add"), QStringLiteral("subtract"), QStringLiteral("intersect"),
                    QStringLiteral("difference")},
                {tr("Add"), tr("Subtract"), tr("Intersect"), tr("Difference")},
                QStringLiteral("add")),
            boolean(maskkeys::Invert, tr("Invert"), false),
            number(maskkeys::Opacity, tr("Opacity"), 0, 100, 100, 1, kPercent),
            number(maskkeys::FeatherIn, tr("Feather Inside"), 0, 2000, 0, 0.5, kPx),
            number(maskkeys::FeatherOut, tr("Feather Outside"), 0, 2000, 0, 0.5, kPx),
            number(maskkeys::Expand, tr("Expand"), -2000, 2000, 0, 0.5, kPx),
            number(maskkeys::PosX, tr("Position X"), -10000, 10000, 0, 1, kPx),
            number(maskkeys::PosY, tr("Position Y"), -10000, 10000, 0, 1, kPx),
            number(maskkeys::Width, tr("Width"), 1, 20000, 960, 1, kPx),
            number(maskkeys::Height, tr("Height"), 1, 20000, 540, 1, kPx),
            number(maskkeys::ScaleX, tr("Scale X"), 0.01, 20.0, 1.0, 0.01),
            number(maskkeys::ScaleY, tr("Scale Y"), 0.01, 20.0, 1.0, 0.01),
            number(maskkeys::Rotation, tr("Rotation"), -3600, 3600, 0, 0.1, kDegrees),
        },
        QStringLiteral("mask")));

    // ---- Video effects (ClipData::effects) ----
    add(descriptor("gaussian_blur", tr("Gaussian Blur"), C::VideoEffect,
        tr("Soft blur of the whole image"),
        {number(QStringLiteral("radius"), tr("Radius"), 0, 100, 5, 0.1, kPx)}));
    add(descriptor("brightness_contrast", tr("Brightness / Contrast"), C::VideoEffect,
        tr("Simple brightness and contrast adjustment"),
        {
            number(QStringLiteral("brightness"), tr("Brightness"), -100, 100, 0, 1),
            number(QStringLiteral("contrast"), tr("Contrast"), -100, 100, 0, 1),
        }));
    add(descriptor("saturation", tr("Saturation"), C::VideoEffect,
        tr("Make colors more or less intense (0 = greyscale)"),
        {number(QStringLiteral("saturation"), tr("Saturation"), 0, 200, 100, 1, kPercent)}));
    add(descriptor("sharpen", tr("Sharpen"), C::VideoEffect, tr("Unsharp-mask sharpening"),
        {
            number(QStringLiteral("amount"), tr("Amount"), 0, 100, 50, 1, kPercent),
            number(QStringLiteral("radius"), tr("Radius"), 0.5, 10, 1, 0.1, kPx),
        }));
    add(descriptor("mirror", tr("Mirror / Flip"), C::VideoEffect,
        tr("Flip the image horizontally and/or vertically"),
        {
            boolean(QStringLiteral("horizontal"), tr("Horizontal"), true),
            boolean(QStringLiteral("vertical"), tr("Vertical"), false),
        }));
    add(descriptor("vignette", tr("Vignette"), C::VideoEffect, tr("Darken the image corners"),
        {
            number(QStringLiteral("amount"), tr("Amount"), 0, 100, 50, 1, kPercent),
            number(QStringLiteral("size"), tr("Size"), 0, 100, 50, 1, kPercent),
            number(QStringLiteral("softness"), tr("Softness"), 0, 100, 50, 1, kPercent),
        }));
    add(descriptor("mosaic", tr("Mosaic"), C::VideoEffect, tr("Pixelate the image into blocks"),
        {integer(QStringLiteral("blockSize"), tr("Block Size"), 2, 200, 20, kPx)}));
    add(descriptor("drop_shadow", tr("Drop Shadow"), C::VideoEffect,
        tr("Shadow behind the clip's alpha (titles, scaled-down overlays)"),
        {
            number(QStringLiteral("distance"), tr("Distance"), 0, 200, 10, 1, kPx),
            number(QStringLiteral("angle"), tr("Angle"), -180, 180, 135, 1, kDegrees),
            number(QStringLiteral("blur"), tr("Blur"), 0, 100, 8, 0.1, kPx),
            number(QStringLiteral("opacity"), tr("Opacity"), 0, 100, 60, 1, kPercent),
            color(QStringLiteral("color"), tr("Color"), QColor(Qt::black)),
        }));

    // ---- Transitions (ClipData::transitionIn / transitionOut) ----
    add(descriptor("cross_dissolve", tr("Cross Dissolve"), C::Transition,
        tr("Blend from one clip into the next"), {}));
    add(descriptor("dip_to_color", tr("Dip to Color"), C::Transition,
        tr("Fade through a solid color"),
        {color(QStringLiteral("color"), tr("Color"), QColor(Qt::black))}));
    add(descriptor("wipe", tr("Wipe"), C::Transition, tr("Reveal the next clip with a moving edge"),
        {
            choice(QStringLiteral("direction"), tr("Direction"), directions(), directionLabels(),
                QStringLiteral("left")),
            number(QStringLiteral("softness"), tr("Softness"), 0, 100, 10, 1, kPercent),
        }));
    add(descriptor("push", tr("Push"), C::Transition,
        tr("The next clip pushes the current one out"),
        {choice(QStringLiteral("direction"), tr("Direction"), directions(), directionLabels(),
            QStringLiteral("left"))}));

    // ---- Generators & titles (clip kinds of their own) ----
    add(descriptor("solid_color", tr("Solid Color"), C::Generator, tr("A full-frame solid color"),
        {color(QStringLiteral("color"), tr("Color"), QColor(0x30, 0x30, 0x34))},
        QStringLiteral("solidColor")));
    registerGenerators();
    add(descriptor("text_title", tr("Text"), C::Title, tr("A simple text title"),
        {
            text(keys::Text, tr("Text"), tr("Title")),
            text(keys::Font, tr("Font"), QStringLiteral("Sans Serif")),
            number(keys::Size, tr("Size"), 1, 500, 72, 1, kPx),
            boolean(keys::Bold, tr("Bold"), false),
            boolean(keys::Italic, tr("Italic"), false),
            choice(keys::Align, tr("Alignment"),
                {QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right")},
                {tr("Left"), tr("Center"), tr("Right")}, QStringLiteral("center")),
            color(keys::Color, tr("Color"), QColor(Qt::white)),
            number(keys::Tracking, tr("Tracking"), -50, 200, 0, 1, kPercent),
            number(keys::LineSpacing, tr("Line Spacing"), 50, 300, 100, 1, kPercent),
            color(keys::OutlineColor, tr("Outline Color"), QColor(Qt::black)),
            number(keys::OutlineWidth, tr("Outline Width"), 0, 50, 0, 0.5, kPx),
            boolean(keys::BgEnabled, tr("Background"), false),
            color(keys::BgColor, tr("Background Color"), QColor(Qt::black)),
            number(keys::BgOpacity, tr("Background Opacity"), 0, 100, 60, 1, kPercent),
            boolean(keys::Shadow, tr("Drop Shadow"), false),
        },
        QStringLiteral("title")));

    // ---- Effects (library section "Effects") ----
    add(descriptor(generators::AdjustmentClip, tr("Adjustment Clip"), C::Effects,
        tr("Applies its effects, transform and masks to every track below it"), {}, {}));
}

void EffectRegistry::registerGenerators()
{
    using C = EffectCategory;
    namespace g = generators;
    const QString storage = QStringLiteral("title"); // generator parameters: ClipData::title
    auto fixed = [](ParamDescriptor p) {
        p.animatable = false;
        return p;
    };
    add(descriptor(g::FourColorGradient, tr("4 Color Gradient"), C::Generator,
        tr("Blends four corner colors across the frame"),
        {color(g::Color1, tr("Top Left"), QColor(0xd9, 0x4f, 0x4f)),
            color(g::Color2, tr("Top Right"), QColor(0xe0, 0xc0, 0x4a)),
            color(g::Color3, tr("Bottom Left"), QColor(0x3f, 0x6f, 0xd0)),
            color(g::Color4, tr("Bottom Right"), QColor(0x4f, 0xb0, 0x70))},
        storage));
    add(descriptor(g::LinearGradient, tr("Linear Gradient"), C::Generator,
        tr("A straight blend between two colors"),
        {color(g::Color1, tr("Start Color"), QColor(Qt::black)),
            color(g::Color2, tr("End Color"), QColor(Qt::white)),
            number(g::Angle, tr("Angle"), -360, 360, 0, 1, kDegrees),
            number(g::CenterX, tr("Center X"), -4000, 4000, 0, 1, kPx),
            number(g::CenterY, tr("Center Y"), -4000, 4000, 0, 1, kPx),
            number(g::Length, tr("Length"), 1, 8000, 1920, 1, kPx)},
        storage));
    add(descriptor(g::RadialGradient, tr("Radial Gradient"), C::Generator,
        tr("A circular blend from an inner to an outer color"),
        {color(g::Color1, tr("Inner Color"), QColor(Qt::white)),
            color(g::Color2, tr("Outer Color"), QColor(Qt::black)),
            number(g::CenterX, tr("Center X"), -4000, 4000, 0, 1, kPx),
            number(g::CenterY, tr("Center Y"), -4000, 4000, 0, 1, kPx),
            number(g::Radius, tr("Radius"), 1, 8000, 800, 1, kPx)},
        storage));
    add(descriptor(g::ColorBars, tr("Color Bars"), C::Generator,
        tr("Test pattern: SMPTE-style or full-field color bars"),
        {choice(g::Style, tr("Style"), {QStringLiteral("smpte"), QStringLiteral("full")},
            {tr("SMPTE (75%)"), tr("Full Field (100%)")}, QStringLiteral("smpte"))},
        storage));
    add(descriptor(g::GreyScale, tr("Grey Scale"), C::Generator,
        tr("A black to white ramp in steps"),
        {fixed(integer(g::Steps, tr("Steps"), 0, 64, 11)),
            choice(g::Orientation, tr("Orientation"),
                {QStringLiteral("horizontal"), QStringLiteral("vertical")},
                {tr("Horizontal"), tr("Vertical")}, QStringLiteral("horizontal"))},
        storage));
    add(descriptor(g::Noise, tr("Noise"), C::Generator, tr("Random grain, still or animated"),
        {boolean(g::Monochrome, tr("Monochrome"), true),
            number(g::Brightness, tr("Brightness"), 0, 100, 50, 1, kPercent),
            number(g::Amount, tr("Amount"), 0, 100, 100, 1, kPercent),
            number(g::Grain, tr("Grain Size"), 1, 100, 1, 0.5, kPx),
            fixed(integer(g::Seed, tr("Seed"), 0, 99999, 1)),
            boolean(g::Animated, tr("Animated"), true)},
        storage));
    add(descriptor(g::Countdown, tr("Countdown"), C::Generator,
        tr("A leader-style countdown with a sweeping hand"),
        {fixed(integer(g::StartSeconds, tr("Start At"), 0, 99, 0, QStringLiteral("s"))),
            color(keys::Color, tr("Number Color"), QColor(Qt::white)),
            color(g::Accent, tr("Sweep Color"), QColor(0x80, 0x80, 0x88)),
            color(g::Background, tr("Background"), QColor(0x30, 0x30, 0x34)),
            text(keys::Font, tr("Font"), QStringLiteral("Sans Serif"))},
        storage));
    add(descriptor(g::Timecode, tr("Timecode"), C::Generator,
        tr("Burns the timeline or clip timecode into the picture"),
        {choice(g::Source, tr("Source"), {QStringLiteral("timeline"), QStringLiteral("clip")},
             {tr("Timeline"), tr("Clip")}, QStringLiteral("timeline")),
            fixed(integer(g::StartFrame, tr("Start Frame"), 0, 9999999, 0)),
            text(g::Prefix, tr("Prefix"), QString()),
            text(keys::Font, tr("Font"), QStringLiteral("Monospace")),
            number(keys::Size, tr("Size"), 8, 400, 64, 1, kPx),
            color(keys::Color, tr("Color"), QColor(Qt::white)),
            boolean(keys::BgEnabled, tr("Background"), true),
            color(keys::BgColor, tr("Background Color"), QColor(Qt::black)),
            number(keys::BgOpacity, tr("Background Opacity"), 0, 100, 70, 1, kPercent),
            number(g::OffsetY, tr("Vertical Position"), -50, 50, 39, 0.5, kPercent)},
        storage));
}

} // namespace kk
