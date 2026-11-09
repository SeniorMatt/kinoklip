// SPDX-License-Identifier: GPL-3.0-or-later
// The video transitions of the Effects library folders: Dissolve (Additive, Blur, Non-Additive,
// Smooth Cut), Iris, Shape, Wipe, Motion. The older four (Cross Dissolve, Dip to Color, Wipe,
// Push) are registered in BuiltinEffects.cpp and only get their folder here. Pixel code:
// src/render/GlTransitions.cpp and the Gl*Transitions.cpp files next to it. Behaviour:
// docs/behavior-spec.md section 53.
#include "core/EffectBuilders.h"
#include "core/EffectRegistry.h"
#include "core/TransitionKeys.h"

#include <QCoreApplication>

namespace kk {

using namespace builders;
namespace k = tkeys;

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::BuiltinTransitions", text);
}

const QString kPx = QStringLiteral("px");
const QString kPercent = QStringLiteral("%");
const QString kDegrees = QStringLiteral("°");

using Params = QVector<ParamDescriptor>;

EffectDescriptor transition(const char* id, const QString& name, const char* group,
    const QString& description, Params params)
{
    EffectDescriptor d = descriptor(
        QLatin1String(id), name, EffectCategory::Transition, description, std::move(params));
    d.group = QLatin1String(group);
    return d;
}

/// Border colour, border width, feather and (unless @p reverse is false) reverse: shared by the
/// iris, shape and wipe transitions.
void appendEdgeSet(Params& params, bool reverse = true)
{
    params.append(color(k::Color, tr("Border Color"), QColor(Qt::white)));
    params.append(number(k::Border, tr("Border"), 0, 200, 0, 1, kPx));
    params.append(number(k::Feather, tr("Feather"), 0, 100, 0, 1, kPercent));
    if (reverse)
        params.append(boolean(k::Reverse, tr("Reverse"), false));
}

/// Parameters of a shape that grows from a centre: @p first, centre offset, rotation, edge set.
Params shapeParams(Params first, bool rotation)
{
    first.append(number(k::OffsetX, tr("Offset X"), -100, 100, 0, 1, kPercent));
    first.append(number(k::OffsetY, tr("Offset Y"), -100, 100, 0, 1, kPercent));
    if (rotation)
        first.append(number(k::Rotation, tr("Rotation"), -360, 360, 0, 1, kDegrees));
    appendEdgeSet(first);
    return first;
}

ParamDescriptor aspect(const QString& label, double def = 1.0)
{
    return number(k::Aspect, label, 0.2, 5, def, 0.05);
}

Params ratios()
{
    return {number(k::StartRatio, tr("Start Ratio"), 0, 100, 0, 1, kPercent),
        number(k::EndRatio, tr("End Ratio"), 0, 100, 100, 1, kPercent)};
}

ParamDescriptor presets(const QStringList& values, const QStringList& labels)
{
    return choice(k::Preset, tr("Preset"), values, labels, values.first());
}

Params withEdgeSet(Params params, bool reverse = true)
{
    appendEdgeSet(params, reverse);
    return params;
}

} // namespace

void EffectRegistry::registerTransitions()
{
    using namespace effectgroups;
    // The four older ones into their folders.
    static const struct
    {
        const char* id;
        const char* folder;
    } kFolders[] = {
        {"cross_dissolve", Dissolve}, {"dip_to_color", Dissolve}, {"wipe", Wipe}, {"push", Motion}};
    for (const auto& [id, folder] : kFolders)
        m_descriptors[m_index.value(QLatin1String(id))].group = QLatin1String(folder);

    // ---- Dissolve ----
    add(transition("additive_dissolve", tr("Additive Dissolve"), Dissolve,
        tr("Add the two pictures: brighter in the middle, back to normal at both ends"), ratios()));
    {
        Params params = ratios();
        params.append(number(k::BlurH, tr("Horizontal Strength"), 0, 30, 4, 0.5, kPercent));
        params.append(number(k::BlurV, tr("Vertical Strength"), 0, 30, 4, 0.5, kPercent));
        add(transition("blur_dissolve", tr("Blur Dissolve"), Dissolve,
            tr("Dissolve with a blur that peaks in the middle"), params));
    }
    add(transition("non_additive_dissolve", tr("Non-Additive Dissolve"), Dissolve,
        tr("Dissolve that keeps the brighter of the two pictures"), ratios()));
    add(transition("smooth_cut", tr("Smooth Cut"), Dissolve,
        tr("Hide a jump cut by morphing the pictures into each other along their motion (optical "
           "flow); use it on a short transition of 2 to 6 frames"),
        {}));

    // ---- Iris ----
    add(transition("arrow_iris", tr("Arrow Iris"), Iris, tr("An arrow grows from the centre"),
        shapeParams({presets({"up", "down", "left", "right"},
                         {tr("Up"), tr("Down"), tr("Left"), tr("Right")}),
                        aspect(tr("Aspect Ratio"))},
            true)));
    add(transition("cross_iris", tr("Cross Iris"), Iris, tr("A cross grows from the centre"),
        shapeParams({}, false)));
    add(transition("diamond_iris", tr("Diamond Iris"), Iris, tr("A diamond grows from the centre"),
        shapeParams({}, false)));
    add(transition("eye_iris", tr("Eye Iris"), Iris, tr("An eye opens from the centre"),
        shapeParams({}, false)));
    add(transition("hexagon_iris", tr("Hexagon Iris"), Iris, tr("A hexagon grows from the centre"),
        shapeParams(
            {presets({"hexagon", "hexagon_vertical"}, {tr("Hexagon"), tr("Hexagon Vertical")}),
                aspect(tr("Aspect Ratio"))},
            true)));
    add(transition("oval_iris", tr("Oval Iris"), Iris, tr("An oval grows from the centre"),
        shapeParams({aspect(tr("Oval Ratio"), 1.5)}, false)));
    add(transition("pentagon_iris", tr("Pentagon Iris"), Iris,
        tr("A pentagon grows from the centre"),
        shapeParams(
            {presets({"up", "down"}, {tr("Up"), tr("Down")}), aspect(tr("Aspect Ratio"))}, true)));
    add(transition("square_iris", tr("Square Iris"), Iris, tr("A square grows from the centre"),
        shapeParams(
            {presets({"flat", "point"}, {tr("Flat"), tr("Point")}), aspect(tr("Aspect Ratio"))},
            true)));
    add(transition("triangle_iris", tr("Triangle Iris"), Iris,
        tr("A triangle grows from the centre"),
        shapeParams({presets({"up", "bottom", "left", "right"},
                         {tr("Up"), tr("Bottom"), tr("Left"), tr("Right")}),
                        aspect(tr("Aspect Ratio"))},
            true)));

    // ---- Shape ----
    {
        const QStringList modes {"upper_left", "upper_right", "lower_left", "lower_right",
            "left_center", "top_center", "right_center", "bottom_center"};
        const QStringList labels {tr("Upper Left"), tr("Upper Right"), tr("Lower Left"),
            tr("Lower Right"), tr("Left Center"), tr("Top Center"), tr("Right Center"),
            tr("Bottom Center")};
        add(transition("box_shape", tr("Box"), Shape,
            tr("A box grows from a corner or the middle of an edge"),
            withEdgeSet({choice(k::Mode, tr("Mode"), modes, labels, modes.first())})));
    }
    add(transition("heart_shape", tr("Heart"), Shape, tr("A heart grows from the centre"),
        shapeParams({aspect(tr("Aspect Ratio"))}, true)));
    add(transition("star_shape", tr("Star"), Shape, tr("A star grows from the centre"),
        shapeParams({presets({"5", "4", "6"}, {tr("5-Point"), tr("4-Point"), tr("6-Point")}),
                        aspect(tr("Aspect Ratio"))},
            true)));
    add(transition("triangle_left_shape", tr("Triangle Left"), Shape,
        tr("A triangle grows from the right edge towards the left"), withEdgeSet({})));
    add(transition("triangle_right_shape", tr("Triangle Right"), Shape,
        tr("A triangle grows from the left edge towards the right"), withEdgeSet({})));

    // ---- Wipe ----
    add(transition("band_wipe", tr("Band Wipe"), Wipe, tr("Strips slide in from alternate sides"),
        withEdgeSet({presets({"horizontal", "vertical", "horizontal_bilinear", "vertical_bilinear"},
                         {tr("Horizontal"), tr("Vertical"), tr("Horizontal Bilinear"),
                             tr("Vertical Bilinear")}),
            number(k::Repeat, tr("Bands"), 2, 50, 8, 1)})));
    add(transition("center_wipe", tr("Center Wipe"), Wipe,
        tr("The next clip opens from a line through the centre"),
        withEdgeSet({number(k::Angle, tr("Angle"), -360, 360, 0, 1, kDegrees)})));
    add(transition("clock_wipe", tr("Clock Wipe"), Wipe,
        tr("A hand sweeps round the centre like a clock"),
        withEdgeSet({number(k::Angle, tr("Angle"), -360, 360, 0, 1, kDegrees),
                        boolean(k::Clockwise, tr("Clockwise"), true)},
            false)));
    add(transition("radial_wipe", tr("Radial Wipe"), Wipe,
        tr("A hand sweeps half a turn round the bottom centre"), withEdgeSet({})));
    add(transition("spiral_wipe", tr("Spiral Wipe"), Wipe,
        tr("The next clip spirals out from the centre"), withEdgeSet({})));
    add(transition("venetian_blind_wipe", tr("Venetian Blind Wipe"), Wipe,
        tr("Slats open one after another"),
        withEdgeSet({number(k::Repeat, tr("Repeat"), 1, 50, 8, 1),
            number(k::Angle, tr("Angle"), -360, 360, 90, 1, kDegrees)})));
    add(transition("x_wipe", tr("X Wipe"), Wipe, tr("The next clip opens along the two diagonals"),
        withEdgeSet({})));

    // ---- Motion ----
    const QStringList dirs {"left", "right", "up", "down"};
    const QStringList dirLabels {tr("Left"), tr("Right"), tr("Up"), tr("Down")};
    const ParamDescriptor blur = number(k::MotionBlur, tr("Motion Blur"), 0, 100, 0, 1, kPercent);
    add(transition("barn_door", tr("Barn Door"), Motion,
        tr("The picture opens like a barn door from the centre"),
        {presets({"vertical", "horizontal"}, {tr("Vertical"), tr("Horizontal")}),
            boolean(k::Reverse, tr("Reverse"), false), blur}));
    add(transition("slide", tr("Slide"), Motion, tr("One clip slides over or off the other"),
        {choice(k::Direction, tr("Direction"), dirs, dirLabels, QStringLiteral("left")),
            choice(k::Mode, tr("Mode"), {"in", "out"}, {tr("Slide In"), tr("Slide Out")},
                QStringLiteral("in")),
            blur}));
    add(transition("split", tr("Split"), Motion,
        tr("The picture splits and the halves move apart in opposite directions"),
        {presets({"horizontal", "vertical"}, {tr("Horizontal"), tr("Vertical")}),
            boolean(k::Reverse, tr("Reverse"), false), blur}));
}

} // namespace kk
