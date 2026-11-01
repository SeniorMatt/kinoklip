// SPDX-License-Identifier: GPL-3.0-or-later
// EffectActions implementation.
#include "ui/effects/EffectActions.h"

#include "core/EditOps.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/AppContext.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace kk::effectactions {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::EffectActions", text);
}

constexpr double kGeneratorSeconds = 5.0;

/// Type of the track holding @p clipId, if the clip exists.
std::optional<TrackType> trackTypeOf(const TimelineData& data, quint64 clipId)
{
    const auto location = editops::findClip(data, clipId);
    if (!location)
        return std::nullopt;
    return location->track.type;
}

bool rangeFree(const TrackData& track, int64_t start, int64_t end)
{
    for (const ClipData& clip : track.clips) {
        if (clip.start < end && clip.end() > start)
            return false;
    }
    return true;
}

} // namespace

int64_t defaultTransitionFrames(Rational fps)
{
    return std::max<int64_t>(1, std::llround(fps.toDouble()));
}

int64_t defaultGeneratorFrames(Rational fps)
{
    return std::max<int64_t>(1, std::llround(fps.toDouble() * kGeneratorSeconds));
}

bool effectFitsTrack(EffectCategory category, TrackType type)
{
    return (category == EffectCategory::VideoEffect && type == TrackType::Video)
        || (category == EffectCategory::AudioEffect && type == TrackType::Audio);
}

int applyEffect(Project& project, const QVector<quint64>& clipIds, const QString& descriptorId)
{
    const EffectRegistry& registry = EffectRegistry::instance();
    const EffectDescriptor* desc = registry.find(descriptorId);
    if (!desc)
        return 0;
    // Check first so a no-op never opens an (empty) undo step.
    const TimelineData& current = project.timeline()->data();
    QVector<quint64> targets;
    for (quint64 id : clipIds) {
        const auto type = trackTypeOf(current, id);
        if (type && effectFitsTrack(desc->category, *type))
            targets.append(id);
    }
    if (targets.isEmpty())
        return 0;
    project.beginEdit(tr("Add %1").arg(desc->name));
    TimelineData& data = project.timeline()->mutableData();
    for (quint64 id : std::as_const(targets)) {
        if (ClipData* clip = editops::clipById(data, id))
            clip->effects.append(registry.createEffect(descriptorId, data.allocateId()));
    }
    project.endEdit();
    return int(targets.size());
}

bool applyTransition(Project& project, quint64 clipId, const QString& descriptorId, bool atHead,
    int64_t durationFrames)
{
    const EffectRegistry& registry = EffectRegistry::instance();
    const EffectDescriptor* desc = registry.find(descriptorId);
    const ClipData* existing = editops::clipById(project.timeline()->data(), clipId);
    if (!desc || desc->category != EffectCategory::Transition || !existing)
        return false;
    const auto& slot = atHead ? existing->transitionIn : existing->transitionOut;
    int64_t frames = durationFrames;
    if (frames < 1)
        frames = slot ? slot->duration : defaultTransitionFrames(project.timeline()->data().fps);
    frames = std::clamp<int64_t>(frames, 1, std::max<int64_t>(1, existing->duration));

    project.beginEdit(tr("Add %1").arg(desc->name));
    ClipData* clip = editops::clipById(project.timeline()->mutableData(), clipId);
    (atHead ? clip->transitionIn : clip->transitionOut)
        = registry.createTransition(descriptorId, frames);
    project.endEdit();
    return true;
}

bool removeTransition(Project& project, quint64 clipId, bool atHead)
{
    const ClipData* existing = editops::clipById(project.timeline()->data(), clipId);
    if (!existing || !(atHead ? existing->transitionIn : existing->transitionOut))
        return false;
    project.beginEdit(tr("Remove Transition"));
    ClipData* clip = editops::clipById(project.timeline()->mutableData(), clipId);
    (atHead ? clip->transitionIn : clip->transitionOut).reset();
    project.endEdit();
    return true;
}

quint64 addGeneratorClip(
    Project& project, const QString& descriptorId, int64_t frame, int64_t durationFrames)
{
    const EffectRegistry& registry = EffectRegistry::instance();
    const EffectDescriptor* desc = registry.find(descriptorId);
    if (!desc
        || (desc->category != EffectCategory::Title && desc->category != EffectCategory::Generator))
        return 0;
    const TimelineData& current = project.timeline()->data();
    const int64_t length
        = durationFrames > 0 ? durationFrames : defaultGeneratorFrames(current.fps);
    const int64_t start = std::max<int64_t>(0, frame);
    const int64_t end = start + length;

    // "Place on top": one track above the highest video track that is busy in [start, end).
    int target = 0;
    for (int i = int(current.videoTracks.size()) - 1; i >= 0; --i) {
        if (!rangeFree(current.videoTracks[size_t(i)], start, end)) {
            target = i + 1;
            break;
        }
    }
    while (target < int(current.videoTracks.size()) && current.videoTracks[size_t(target)].locked)
        ++target;

    ClipData clip;
    clip.start = start;
    clip.duration = length;
    clip.name = desc->name;
    if (desc->category == EffectCategory::Title) {
        clip.kind = ClipKind::Title;
        clip.title = registry.defaultParams(descriptorId);
        clip.name = clip.title.staticValue(keys::Text, desc->name).toString();
    } else {
        clip.kind = ClipKind::Solid;
        const ParamDescriptor* color = desc->param(QStringLiteral("color"));
        clip.solidColor = color ? color->defStatic.value<QColor>() : QColor(Qt::black);
    }

    project.beginEdit(tr("Add %1").arg(desc->name));
    TimelineData& data = project.timeline()->mutableData();
    if (target >= int(data.videoTracks.size()))
        target = editops::addTrack(data, TrackType::Video);
    const quint64 id = editops::overwriteClip(data, TrackRef {TrackType::Video, target}, clip);
    project.endEdit();
    return id;
}

bool activate(AppContext& ctx, const QString& descriptorId)
{
    if (!ctx.project)
        return false;
    Project& project = *ctx.project;
    const EffectDescriptor* desc = EffectRegistry::instance().find(descriptorId);
    if (!desc)
        return false;
    const int64_t playhead = project.timeline()->playhead();
    switch (desc->category) {
    case EffectCategory::VideoEffect:
    case EffectCategory::AudioEffect:
        return ctx.selection && applyEffect(project, ctx.selection->clips(), descriptorId) > 0;
    case EffectCategory::Transition: {
        const quint64 clipId = ctx.selection ? ctx.selection->currentClip() : 0;
        const ClipData* clip = editops::clipById(project.timeline()->data(), clipId);
        if (!clip)
            return false;
        // The end of the clip nearer to the playhead gets the transition.
        const bool atHead = std::abs(playhead - clip->start) <= std::abs(clip->end() - playhead);
        return applyTransition(project, clipId, descriptorId, atHead);
    }
    case EffectCategory::Title:
    case EffectCategory::Generator: {
        const quint64 id = addGeneratorClip(project, descriptorId, playhead);
        if (id && ctx.selection)
            ctx.selection->selectClip(id);
        return id != 0;
    }
    case EffectCategory::ClipProperties:
        return false;
    }
    return false;
}

} // namespace kk::effectactions
