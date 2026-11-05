// SPDX-License-Identifier: GPL-3.0-or-later
// The built-in v0.1 catalogue: clip property groups, video effects, transitions, generators.
// To add an effect: append a descriptor here, then implement it in src/render keyed by its id.
#include "core/AudioTransitions.h"
#include "core/DynamicZoom.h"
#include "core/EffectBuilders.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/NodeRegistry.h"
#include "core/ParamKeys.h"
#include "core/Subtitles.h"
#include "core/TitlePresets.h"

#include <QCoreApplication>

#include <utility>

namespace kk {

using namespace builders;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::BuiltinEffects", text);
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

/// A vertical offset stored +down and shown +up, as Resolve's Inspector shows it (spec 24.16).
ParamDescriptor shownUp(ParamDescriptor p)
{
    p.displayNegated = true;
    return p;
}

} // namespace

void EffectRegistry::registerBuiltins()
{
    using C = EffectCategory;

    // ---- Clip property groups (stored on ClipData, always present in the Inspector) ----
    add(descriptor(groups::Transform, tr("Transform"), C::ClipProperties,
        tr("Position, zoom, rotation, anchor point and flips"),
        {
            number(keys::ZoomX, tr("Zoom X"), 0.01, 20.0, 1.0, 0.01),
            number(keys::ZoomY, tr("Zoom Y"), 0.01, 20.0, 1.0, 0.01),
            boolean(keys::ZoomLinked, tr("Link Zoom"), true),
            number(keys::PosX, tr("Position X"), -10000, 10000, 0, 1, kPx),
            shownUp(number(keys::PosY, tr("Position Y"), -10000, 10000, 0, 1, kPx)),
            number(keys::Rotation, tr("Rotation"), -3600, 3600, 0, 0.1, kDegrees),
            number(keys::AnchorX, tr("Anchor X"), -10000, 10000, 0, 1, kPx),
            shownUp(number(keys::AnchorY, tr("Anchor Y"), -10000, 10000, 0, 1, kPx)),
            boolean(keys::FlipH, tr("Flip Horizontal"), false),
            boolean(keys::FlipV, tr("Flip Vertical"), false),
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
    add(descriptor(groups::DynamicZoom, tr("Dynamic Zoom"), C::ClipProperties,
        tr("Move from a start framing to an end framing over the clip (pan and zoom)"),
        {
            choice(keys::DynamicZoomEase, tr("Dynamic Zoom Ease"), dynamiczoom::easeIds(),
                dynamiczoom::easeLabels(), dynamiczoom::easeIds().front()),
        },
        QStringLiteral("transform")));
    // Resolve's Composite group: how the clip combines with the tracks below, and its
    // opacity (stored with the transform, key unchanged, so older projects load the same).
    {
        QStringList labels;
        for (const QString& id : blendmodes::compositeOrder())
            labels.append(blendmodes::displayName(id));
        add(descriptor(groups::Composite, tr("Composite"), C::ClipProperties,
            tr("Composite mode and opacity over the tracks below"),
            {
                choice(keys::CompositeMode, tr("Composite Mode"), blendmodes::compositeOrder(),
                    labels, blendmodes::Normal),
                number(keys::Opacity, tr("Opacity"), 0, 100, 100, 1, kPercent),
            },
            QStringLiteral("transform")));
    }
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

    // Library folders of the v0.1 video effects.
    static const std::pair<const char*, const char*> kFolders[] = {
        {"gaussian_blur", effectgroups::Blur},
        {"brightness_contrast", effectgroups::Color},
        {"saturation", effectgroups::Color},
        {"sharpen", effectgroups::Blur},
        {"mirror", effectgroups::Transform},
        {"vignette", effectgroups::Stylize},
        {"mosaic", effectgroups::Stylize},
        {"drop_shadow", effectgroups::Stylize},
    };
    for (const auto& [id, folder] : kFolders)
        m_descriptors[m_index.value(QLatin1String(id))].group = QLatin1String(folder);
    registerVideoEffects(); // keyers, colour, blurs, light, stylize, distort (v4)
    registerVideoEffects2(); // curves, LUTs, lens blur, light, looks, warps, time (v4)

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

    // ---- Audio transitions (gain curves in core/AudioTransitions.h) ----
    namespace at = audiotransitions;
    add(descriptor(at::kCrossFade0dB, tr("Cross Fade 0 dB"), C::AudioTransition,
        tr("Linear crossfade: the level dips at the middle"), {}));
    add(descriptor(at::kCrossFadePlus3dB, tr("Cross Fade +3 dB"), C::AudioTransition,
        tr("Equal-power crossfade: constant loudness for unrelated sounds (default)"), {}));
    add(descriptor(at::kCrossFadeMinus3dB, tr("Cross Fade -3 dB"), C::AudioTransition,
        tr("Between linear and equal power"), {}));
    add(descriptor(at::kExponential, tr("Exponential"), C::AudioTransition,
        tr("The incoming sound rises slowly, then fast"), {}));
    add(descriptor(at::kLogarithmic, tr("Logarithmic"), C::AudioTransition,
        tr("The incoming sound rises fast, then slowly"), {}));
    add(descriptor(at::kSCurve, tr("S-Curve"), C::AudioTransition,
        tr("Gentle at both ends, quickest at the middle"), {}));

    // ---- Generators & titles (clip kinds of their own) ----
    add(descriptor("solid_color", tr("Solid Color"), C::Generator, tr("A full-frame solid color"),
        {color(QStringLiteral("color"), tr("Color"), QColor(0x30, 0x30, 0x34))},
        QStringLiteral("solidColor")));
    registerGenerators();
    registerTitles();

    // ---- Subtitles (inspector groups of captions and subtitle tracks; not in the library) ----
    namespace st = subtitles;
    auto fixedNumber = [](ParamDescriptor p) {
        p.animatable = false;
        return p;
    };
    add(descriptor(st::CaptionGroup, tr("Caption"), C::ClipProperties, tr("Subtitle text"),
        {text(keys::Text, tr("Text"), QString()), boolean(st::OwnStyle, tr("Custom Style"), false)},
        QStringLiteral("title")));
    add(descriptor(st::StyleGroup, tr("Style"), C::ClipProperties, tr("How captions look"),
        {text(keys::Font, tr("Font"), QStringLiteral("Sans Serif")),
            fixedNumber(number(keys::Size, tr("Size"), 4, 400, st::kDefaultSize, 1, kPx)),
            boolean(keys::Bold, tr("Bold"), false), boolean(keys::Italic, tr("Italic"), false),
            color(keys::Color, tr("Color"), QColor(Qt::white)),
            color(keys::OutlineColor, tr("Outline Color"), QColor(Qt::black)),
            fixedNumber(number(keys::OutlineWidth, tr("Outline Width"), 0, 30,
                st::kDefaultOutlineWidth, 0.5, kPx)),
            boolean(keys::Shadow, tr("Drop Shadow"), true),
            boolean(keys::BgEnabled, tr("Background"), false),
            color(keys::BgColor, tr("Background Color"), QColor(Qt::black)),
            fixedNumber(number(keys::BgOpacity, tr("Background Opacity"), 0, 100,
                st::kDefaultBgOpacity, 1, kPercent)),
            choice(keys::Align, tr("Alignment"),
                {QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right")},
                {tr("Left"), tr("Center"), tr("Right")}, QStringLiteral("center")),
            choice(st::Position, tr("Position"),
                {QStringLiteral("bottom"), QStringLiteral("top"), QStringLiteral("middle")},
                {tr("Bottom"), tr("Top"), tr("Middle")}, QStringLiteral("bottom")),
            fixedNumber(number(
                st::MarginV, tr("Vertical Margin"), 0, 50, st::kDefaultMarginV, 0.5, kPercent)),
            fixedNumber(number(
                st::MarginH, tr("Horizontal Margin"), 0, 50, st::kDefaultMarginH, 0.5, kPercent)),
            fixedNumber(number(keys::LineSpacing, tr("Line Spacing"), 50, 300, 100, 1, kPercent))},
        QStringLiteral("title")));

    // ---- Effects (library section "Effects") ----
    add(descriptor(generators::AdjustmentClip, tr("Adjustment Clip"), C::Effects,
        tr("Applies its effects, transform and masks to every track below it"), {}, {}));
    add(descriptor(generators::RegionBlur, tr("Region Blur"), C::Effects,
        tr("An adjustment clip that blurs a rectangle of the picture below it: move and size "
           "its mask in the viewer"),
        {}, {}));
}

namespace {

/// What sets one title preset apart: its own defaults of the shared title parameters.
struct TitleLook
{
    QString text;
    double size = 72;
    QString align = QStringLiteral("center");
    QString anchor = QStringLiteral("center");
    double posX = 0.5;
    double posY = 0.5;
    bool background = false;
    QColor bgColor = QColor(Qt::black);
    double bgOpacity = 60;
    bool scroll = false;
    bool shadow = false;
};

/// The parameters every title has (Inspector > Title), with @p look's defaults. All the
/// presets share them, so a preset is only a starting point: any title can become any other.
QVector<ParamDescriptor> titleParams(const TitleLook& look)
{
    const QStringList sides {
        QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right")};
    const QStringList sideLabels {tr("Left"), tr("Center"), tr("Right")};
    return {
        text(keys::Text, tr("Text"), look.text),
        text(keys::Font, tr("Font"), QStringLiteral("Sans Serif")),
        number(keys::Size, tr("Size"), 1, 500, look.size, 1, kPx),
        boolean(keys::Bold, tr("Bold"), false),
        boolean(keys::Italic, tr("Italic"), false),
        choice(keys::Align, tr("Alignment"), sides, sideLabels, look.align),
        color(keys::Color, tr("Color"), QColor(Qt::white)),
        number(keys::Tracking, tr("Tracking"), -50, 200, 0, 1, kPercent),
        number(keys::LineSpacing, tr("Line Spacing"), 50, 300, 100, 1, kPercent),
        number(keys::TextPosX, tr("Position X"), -1, 2, look.posX, 0.001),
        number(keys::TextPosY, tr("Position Y"), -1, 2, look.posY, 0.001),
        choice(keys::Anchor, tr("Anchor"), sides, sideLabels, look.anchor),
        boolean(keys::Scroll, tr("Scroll"), look.scroll),
        color(keys::OutlineColor, tr("Outline Color"), QColor(Qt::black)),
        number(keys::OutlineWidth, tr("Outline Width"), 0, 50, 0, 0.5, kPx),
        boolean(keys::BgEnabled, tr("Background"), look.background),
        color(keys::BgColor, tr("Background Color"), look.bgColor),
        number(keys::BgOpacity, tr("Background Opacity"), 0, 100, look.bgOpacity, 1, kPercent),
        boolean(keys::Shadow, tr("Drop Shadow"), look.shadow),
    };
}

} // namespace

void EffectRegistry::registerTitles()
{
    using C = EffectCategory;
    const QString storage = QStringLiteral("title"); // ClipData::title
    // Our own simple designs: plain text, system font, an optional flat box.
    const QColor box(0x14, 0x16, 0x1c);
    TitleLook lowerThird;
    lowerThird.text = tr("Name Surname\nRole or place");
    lowerThird.size = 44;
    lowerThird.posY = 0.16;
    lowerThird.background = true;
    lowerThird.bgColor = box;
    lowerThird.bgOpacity = 75;

    TitleLook left = lowerThird;
    left.align = left.anchor = QStringLiteral("left");
    left.posX = 0.07;
    add(descriptor(titles::LeftLowerThird, tr("Left Lower Third"), C::Title,
        tr("Name and role in the lower left of the frame"), titleParams(left), storage));
    add(descriptor(titles::MiddleLowerThird, tr("Middle Lower Third"), C::Title,
        tr("Name and role centred low in the frame"), titleParams(lowerThird), storage));
    TitleLook right = lowerThird;
    right.align = right.anchor = QStringLiteral("right");
    right.posX = 0.93;
    add(descriptor(titles::RightLowerThird, tr("Right Lower Third"), C::Title,
        tr("Name and role in the lower right of the frame"), titleParams(right), storage));

    TitleLook scroll;
    scroll.text = tr("The End\n\nDirected by\nName\n\nEdited by\nName\n\nMusic by\nName\n\n"
                     "Thank you for watching");
    scroll.size = 48;
    scroll.scroll = true;
    add(descriptor(titles::Scroll, tr("Scroll"), C::Title,
        tr("Rolling credits: the text moves up through the frame over the clip's length"),
        titleParams(scroll), storage));

    TitleLook basic;
    basic.text = tr("Basic Title"); // as in Resolve (spec 14.5)
    add(descriptor(titles::Text, tr("Text"), C::Title, tr("A simple text title"),
        titleParams(basic), storage));

    TitleLook boxed;
    boxed.text = tr("Title");
    boxed.size = 64;
    boxed.background = true;
    boxed.bgColor = box;
    boxed.bgOpacity = 85;
    add(descriptor(titles::TextBox, tr("Text Box"), C::Title,
        tr("A text on a plain background box"), titleParams(boxed), storage));
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
