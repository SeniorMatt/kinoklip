// SPDX-License-Identifier: GPL-3.0-or-later
// The tracking effects of the 1.2 gap list (docs/behavior-spec.md section 70): Match Move, Surface
// Tracker and Warper. Maths: core/MatchMove.h, src/render (GlMeshEffects.cpp); the tracking
// itself: compositing/tracking/EffectTrackJob.h.
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

EffectDescriptor effect(const char* id, const QString& name, const QString& description,
    const char* group, QVector<ParamDescriptor> params)
{
    EffectDescriptor d = descriptor(
        QLatin1String(id), name, EffectCategory::VideoEffect, description, std::move(params));
    d.group = QLatin1String(group);
    return d;
}

/// A number that is an input of the tracker (changing it needs a new track), not keyframable.
ParamDescriptor fixedNumber(const QString& key, const QString& label, double min, double max,
    double def, double step, const QString& unit)
{
    ParamDescriptor p = number(key, label, min, max, def, step, unit);
    p.animatable = false;
    return p;
}

ParamDescriptor trackData()
{
    ParamDescriptor p = text(fxkeys::TrackData, QStringLiteral("Track"), QString());
    p.hidden = true;
    return p;
}

} // namespace

void EffectRegistry::registerVideoEffects7()
{
    namespace k = fxkeys;
    namespace c = fxchoice;
    namespace g = effectgroups;

    add(effect(fxid::MatchMove, tr("Match Move"),
        tr("Follows a point or a region of the picture and moves the clip with it"), g::Transform,
        {
            choice(k::TrackSource, tr("Track"), {c::TrackClip, c::TrackBelow},
                {tr("This Clip"), tr("Footage Below")}, c::TrackClip),
            choice(k::TrackKind, tr("Tracking"), {c::KindPosition, c::KindPlanar},
                {tr("Position"), tr("Position, Scale and Rotation")}, c::KindPosition),
            choice(k::TrackDirection, tr("Direction"), {c::DirectionFollow, c::DirectionLock},
                {tr("Follow the Track"), tr("Hold the Track in Place")}, c::DirectionFollow),
            fixedNumber(k::RegionX, tr("Region X"), 0, 100, 50, 0.1, kPercent),
            fixedNumber(k::RegionY, tr("Region Y"), 0, 100, 50, 0.1, kPercent),
            fixedNumber(k::RegionSize, tr("Region Size"), 2, 80, 12, 0.1, kPercent),
            number(k::Strength, tr("Strength"), 0, 200, 100, 1, kPercent),
            number(k::OffsetX, tr("Offset X"), -4000, 4000, 0, 0.5, kPx),
            number(k::OffsetY, tr("Offset Y"), -4000, 4000, 0, 0.5, kPx),
            trackData(),
        }));
}

} // namespace kk
