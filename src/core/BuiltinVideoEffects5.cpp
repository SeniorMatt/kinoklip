// SPDX-License-Identifier: GPL-3.0-or-later
// The fifth batch of video effects (1.2, docs/behavior-spec.md section 60): the effects that
// read other frames of the clip (Motion Trails, Smear, Frame Replacer, Motion Blur, Deflicker,
// Color Stabilizer, Automatic Dirt Removal, Dust Buster) and Patch Replacer. Which frames an
// effect reads: src/render/TimeEffects.cpp. Pixel code: src/render (GlTemporalEffects.cpp,
// GlRepairEffects.cpp).
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
const QString kFrames = QStringLiteral("fr");

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

/// A whole-number setting that decides which frames are read: not keyframable.
ParamDescriptor fixedInt(
    const QString& key, const QString& label, int min, int max, int def, const QString& unit = {})
{
    ParamDescriptor p = integer(key, label, min, max, def, unit);
    p.animatable = false;
    return p;
}

ParamDescriptor px(const QString& key, const QString& label, double min, double max, double def)
{
    return number(key, label, min, max, def, 1.0, kPx);
}

} // namespace

void EffectRegistry::registerVideoEffects5()
{
    namespace k = fxkeys;
    namespace c = fxchoice;
    namespace g = effectgroups;

    // ---- Temporal ----
    add(effect(fxid::MotionTrails, tr("Motion Trails"), g::Temporal,
        tr("Leaves a fading trail of the clip's earlier frames behind everything that moves"),
        {
            fixedInt(k::Trails, tr("Trails"), 1, 5, 4),
            fixedInt(k::Delay, tr("Frame Spacing"), 1, 4, 1, kFrames),
            percent(k::Decay, tr("Fade"), 0, 100, 35),
            percent(k::Opacity, tr("Trail Opacity"), 0, 100, 100),
            choice(k::TrailMode, tr("Mode"), {c::Lighten, c::Average},
                {tr("Lighten"), tr("Average")}, c::Lighten),
        }));
    add(effect(fxid::Smear, tr("Smear"), g::Temporal,
        tr("Averages the frame with the frames around it: a soft smear along every movement"),
        {
            fixedInt(k::Length, tr("Length"), 1, 2, 2, kFrames),
            choice(k::SmearDirection, tr("Direction"), {c::Both, c::Behind, c::Ahead},
                {tr("Both"), tr("Behind"), tr("Ahead")}, c::Both),
            percent(k::Decay, tr("Falloff"), 0, 100, 30),
            percent(k::Amount, tr("Amount"), 0, 100, 100),
        }));
    add(effect(fxid::TemporalMotionBlur, tr("Motion Blur"), g::Temporal,
        tr("Blurs along the motion measured between neighbouring frames, like a camera shutter"),
        {
            number(k::ShutterAngle, tr("Shutter Angle"), 0, 720, 180, 1.0, QStringLiteral("°")),
            fixedInt(k::Samples, tr("Quality"), 4, 32, 12),
        }));

    // ---- Revival ----
    add(effect(fxid::FrameReplacer, tr("Frame Replacer"), g::Revival,
        tr("Replaces a damaged frame with the frame before it, the frame after it or a mix of "
           "both"),
        {
            choice(k::FrameSource, tr("Replace With"), {c::Previous, c::Next, c::Mix},
                {tr("Previous Frame"), tr("Next Frame"), tr("Mix of Both")}, c::Previous),
            percent(k::Amount, tr("Replace"), 0, 100, 100),
        }));
    add(effect(fxid::Deflicker, tr("Deflicker"), g::Revival,
        tr("Evens out brightness that jumps from frame to frame by pulling each frame toward the "
           "average of the frames around it"),
        {
            fixedInt(k::FrameWindow, tr("Time Window"), 1, 2, 2, kFrames),
            percent(k::Strength, tr("Strength"), 0, 100, 100),
            percent(k::MaxCorrection, tr("Limit"), 0, 100, 50),
            boolean(k::PerChannel, tr("Correct Colour Too"), false),
        }));
    add(effect(fxid::DirtRemoval, tr("Automatic Dirt Removal"), g::Revival,
        tr("Removes dust and hairs that show in one frame only, using the frames around it"),
        {
            fixedInt(k::FrameWindow, tr("Time Window"), 1, 2, 1, kFrames),
            percent(k::Threshold, tr("Dirt Contrast"), 1, 100, 15),
            percent(k::MotionThreshold, tr("Motion Tolerance"), 0, 100, 8),
            px(k::Dilate, tr("Grow"), 0, 8, 1),
            percent(k::Amount, tr("Strength"), 0, 100, 100),
        }));
    add(effect(fxid::DustBuster, tr("Dust Buster"), g::Revival,
        tr("Removes dust and scratches that show in one frame only, inside a region you place"),
        {
            choice(k::Shape, tr("Region Shape"),
                {QStringLiteral("rectangle"), QStringLiteral("ellipse")},
                {tr("Rectangle"), tr("Ellipse")}, QStringLiteral("rectangle")),
            number(k::CenterX, tr("Center X"), -4000, 4000, 0, 1.0, kPx),
            number(k::CenterY, tr("Center Y"), -4000, 4000, 0, 1.0, kPx),
            px(k::Width, tr("Width"), 1, 8000, 400),
            px(k::Height, tr("Height"), 1, 8000, 300),
            px(k::Feather, tr("Feather"), 0, 500, 20),
            choice(k::RegionMode, tr("Repair"), {c::DetectedDirt, c::WholeRegion},
                {tr("Detected Dirt"), tr("Whole Region")}, c::DetectedDirt),
            fixedInt(k::FrameWindow, tr("Time Window"), 1, 2, 1, kFrames),
            percent(k::Threshold, tr("Dirt Contrast"), 1, 100, 15),
            percent(k::MotionThreshold, tr("Motion Tolerance"), 0, 100, 8),
            px(k::Dilate, tr("Grow"), 0, 8, 2),
            percent(k::Amount, tr("Strength"), 0, 100, 100),
        }));
    add(effect(fxid::PatchReplacer, tr("Patch Replacer"), g::Revival,
        tr("Covers a blemish with a patch copied from another place in the same picture"),
        {
            choice(k::Shape, tr("Patch Shape"),
                {QStringLiteral("rectangle"), QStringLiteral("ellipse")},
                {tr("Rectangle"), tr("Ellipse")}, QStringLiteral("ellipse")),
            number(k::SourceX, tr("Source X"), -4000, 4000, -250, 1.0, kPx),
            number(k::SourceY, tr("Source Y"), -4000, 4000, 0, 1.0, kPx),
            number(k::TargetX, tr("Target X"), -4000, 4000, 0, 1.0, kPx),
            number(k::TargetY, tr("Target Y"), -4000, 4000, 0, 1.0, kPx),
            px(k::Width, tr("Width"), 1, 8000, 160),
            px(k::Height, tr("Height"), 1, 8000, 160),
            px(k::Feather, tr("Feather"), 0, 500, 12),
            percent(k::Opacity, tr("Blend"), 0, 100, 100),
        }));

    // ---- Color ----
    add(effect(fxid::ColorStabilizer, tr("Color Stabilizer"), g::Color,
        tr("Holds exposure and white balance steady by matching every frame to a reference frame"),
        {
            fixedInt(k::ReferenceFrame, tr("Reference Frame"), 0, 999999, 0, kFrames),
            choice(k::Match, tr("Match"), {c::Both, c::Exposure, c::WhiteBalance},
                {tr("Exposure and White Balance"), tr("Exposure"), tr("White Balance")}, c::Both),
            percent(k::Strength, tr("Strength"), 0, 100, 100),
            percent(k::MaxCorrection, tr("Limit"), 0, 300, 100),
            percent(k::RegionX, tr("Region X"), -50, 50, 0),
            percent(k::RegionY, tr("Region Y"), -50, 50, 0),
            percent(k::RegionWidth, tr("Region Width"), 5, 100, 100),
            percent(k::RegionHeight, tr("Region Height"), 5, 100, 100),
        }));
}

} // namespace kk
