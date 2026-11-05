// SPDX-License-Identifier: GPL-3.0-or-later
// EffectActions implementation.
#include "ui/effects/EffectActions.h"

#include "core/AudioTransitions.h"
#include "core/EditBehaviour.h"
#include "core/EditOps.h"
#include "core/Generators.h"
#include "core/Mask.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/TitlePresets.h"
#include "ui/common/AppContext.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace kk::effectactions {

namespace {

constexpr double kRegionBlurRadius = 40.0; ///< px: enough to hide a face at 1080p
constexpr double kRegionBlurFeather = 8.0; ///< px: a soft edge, as most privacy blurs have

QString tr(const char* text)
{
    return QCoreApplication::translate("kk::EffectActions", text);
}

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
    // Preferences > Editing > "Generator duration" (EditBehaviour::generatorSeconds).
    return EditBehaviour::frames(EditBehaviour::fromSettings().generatorSeconds, fps);
}

std::optional<ClipData> makeGeneratorClip(
    const QString& descriptorId, Rational fps, QSizeF frameSize)
{
    const EffectRegistry& registry = EffectRegistry::instance();
    const EffectDescriptor* desc = registry.find(descriptorId);
    if (!desc)
        return std::nullopt;
    ClipData clip;
    clip.name = desc->name;
    clip.duration = defaultGeneratorFrames(fps);
    switch (desc->category) {
    case EffectCategory::Title:
        clip.kind = ClipKind::Title;
        clip.title = registry.defaultParams(descriptorId);
        if (descriptorId == titles::Text) {
            clip.name = clip.title.staticValue(keys::Text, desc->name).toString();
        } else {
            // A preset is remembered (the Inspector names it and resets to its defaults); the
            // clip is named after it, as Resolve names a Left Lower Third clip.
            clip.generator = descriptorId;
            clip.name = desc->name;
        }
        break;
    case EffectCategory::Effects:
        clip.kind = ClipKind::Adjustment; // Adjustment Clip, Region Blur
        if (descriptorId == generators::RegionBlur) {
            // Blur + rectangle mask: what Resolve users build from an adjustment clip by hand
            // (spec 49.8). Ids are assigned when the clip is placed.
            EffectInstance blur = registry.createEffect(QStringLiteral("gaussian_blur"), 0);
            blur.params.setBase(QStringLiteral("radius"), kRegionBlurRadius);
            clip.effects.append(std::move(blur));
            MaskData box = makeMask(MaskShape::Rectangle, 0, frameSize / 3.0);
            box.params.setBase(maskkeys::FeatherOut, kRegionBlurFeather);
            clip.masks.append(std::move(box));
        }
        break;
    case EffectCategory::Generator:
        if (descriptorId == generators::SolidColor) {
            clip.kind = ClipKind::Solid;
            const ParamDescriptor* color = desc->param(QStringLiteral("color"));
            // Fixed: the solid's colour is clip content (user data), not scheme chrome.
            clip.solidColor = color ? color->defStatic.value<QColor>() : QColor(Qt::black);
        } else {
            clip.kind = ClipKind::Generator;
            clip.generator = descriptorId;
            clip.title = registry.defaultParams(descriptorId);
        }
        break;
    default:
        return std::nullopt;
    }
    return clip;
}

bool effectFitsTrack(EffectCategory category, TrackType type)
{
    return (category == EffectCategory::VideoEffect && type == TrackType::Video)
        || (category == EffectCategory::AudioEffect && type == TrackType::Audio);
}

bool transitionFitsTrack(EffectCategory category, TrackType type)
{
    return (category == EffectCategory::Transition && type == TrackType::Video)
        || (category == EffectCategory::AudioTransition && type == TrackType::Audio);
}

QString defaultTransitionId(TrackType type)
{
    return type == TrackType::Audio ? audiotransitions::defaultId()
                                    : QStringLiteral("cross_dissolve");
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
    if (!desc || !existing)
        return false;
    const auto trackType = trackTypeOf(project.timeline()->data(), clipId);
    if (!trackType || !transitionFitsTrack(desc->category, *trackType))
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
        || (desc->category != EffectCategory::Title && desc->category != EffectCategory::Generator
            && desc->category != EffectCategory::Effects))
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

    std::optional<ClipData> made
        = makeGeneratorClip(descriptorId, current.fps, QSizeF(current.width, current.height));
    if (!made)
        return 0;
    ClipData clip = std::move(*made);
    clip.start = start;
    clip.duration = length;

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
    case EffectCategory::Transition:
    case EffectCategory::AudioTransition: {
        // The current clip, else the most recent selected clip on a track the transition fits
        // (an audio transition with a linked video + audio pair selected goes onto the audio).
        const TimelineData& data = project.timeline()->data();
        quint64 clipId = 0;
        const QVector<quint64> selected
            = ctx.selection ? ctx.selection->clips() : QVector<quint64> {};
        for (auto it = selected.crbegin(); it != selected.crend() && !clipId; ++it) {
            const auto type = trackTypeOf(data, *it);
            if (type && transitionFitsTrack(desc->category, *type))
                clipId = *it;
        }
        const ClipData* clip = editops::clipById(data, clipId);
        if (!clip)
            return false;
        // The end of the clip nearer to the playhead gets the transition.
        const bool atHead = std::abs(playhead - clip->start) <= std::abs(clip->end() - playhead);
        return applyTransition(project, clipId, descriptorId, atHead);
    }
    case EffectCategory::Title:
    case EffectCategory::Generator:
    case EffectCategory::Effects: {
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
