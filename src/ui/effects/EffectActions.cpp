// SPDX-License-Identifier: GPL-3.0-or-later
// EffectActions implementation.
#include "ui/effects/EffectActions.h"

#include "core/AudioTransitions.h"
#include "core/CompOps.h"
#include "core/EditBehaviour.h"
#include "core/EditOps.h"
#include "core/Generators.h"
#include "core/Mask.h"
#include "core/ParamKeys.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/TitlePresets.h"
#include "core/VideoEffectKeys.h"
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

/// Sets effect parameter @p key of @p effect to @p value.
void setNumber(EffectInstance& effect, const QString& key, double value)
{
    effect.params.setBase(key, value);
}

/// Adds effect @p type (default parameters) to @p clip and returns it for setting parameters.
EffectInstance& addEffect(ClipData& clip, const char* type)
{
    clip.effects.append(EffectRegistry::instance().createEffect(QLatin1String(type), 0));
    return clip.effects.last();
}

/// Fills the adjustment clip @p clip with the stack of Effects > Colored Border / Binoculars /
/// Night Vision (spec 56.10) and the seven template looks (spec 65). Ids are assigned when the
/// clip is placed.
void fillTemplate(ClipData& clip, const QString& id, QSizeF frame)
{
    namespace k = fxkeys;
    if (id == generators::ColoredBorderClip) {
        EffectInstance& border = addEffect(clip, fxid::ColoredBorder);
        setNumber(border, k::Width, frame.height() / 45.0);
        setNumber(border, k::Radius, frame.height() / 36.0);
    } else if (id == generators::BinocularsClip) {
        // Everything outside the two lenses goes (nearly) black: a rectangle over the whole
        // frame, minus two overlapping circles, softly edged.
        EffectInstance& dark = addEffect(clip, fxid::Levels);
        setNumber(dark, k::OutWhite, 8.0); // 8 of 255: all but black
        const double lens = frame.height() * 0.8;
        const double feather = frame.height() / 60.0;
        MaskData all = makeMask(MaskShape::Rectangle, 0, frame * 1.2);
        clip.masks.append(std::move(all));
        for (const double side : {-1.0, 1.0}) {
            MaskData circle = makeMask(MaskShape::Ellipse, 0, QSizeF(lens, lens));
            circle.mode = MaskMode::Subtract;
            circle.params.setBase(maskkeys::PosX, side * lens * 0.375);
            circle.params.setBase(maskkeys::FeatherOut, feather);
            clip.masks.append(std::move(circle));
        }
    } else if (id == generators::CctvClip) {
        addEffect(clip, fxid::LookCctv);
    } else if (id == generators::DigitalGlitchClip) {
        addEffect(clip, fxid::LookDigitalGlitch);
    } else if (id == generators::DroneOverlayClip) {
        addEffect(clip, fxid::LookDroneOverlay);
    } else if (id == generators::DslrClip) {
        addEffect(clip, fxid::LookDslr);
    } else if (id == generators::DveClip) {
        addEffect(clip, fxid::LookDve);
    } else if (id == generators::VideoCallClip) {
        addEffect(clip, fxid::LookVideoCall);
    } else if (id == generators::VideoCameraClip) {
        addEffect(clip, fxid::LookVideoCamera);
    } else {
        EffectInstance& tint = addEffect(clip, fxid::Duotone);
        tint.params.setStatic(k::ShadowColor, QColor(0x00, 0x14, 0x05));
        tint.params.setStatic(k::HighlightColor, QColor(0x8c, 0xff, 0x96));
        EffectInstance& grain = addEffect(clip, fxid::FilmGrain);
        setNumber(grain, k::Amount, 35.0);
        EffectInstance& lines = addEffect(clip, fxid::Scanlines);
        setNumber(lines, k::Amount, 25.0);
        EffectInstance& vignette = addEffect(clip, "vignette");
        setNumber(vignette, k::Amount, 70.0);
    }
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
        if (descriptorId == generators::ColoredBorderClip
            || descriptorId == generators::BinocularsClip
            || descriptorId == generators::NightVisionClip || descriptorId == generators::CctvClip
            || descriptorId == generators::DigitalGlitchClip
            || descriptorId == generators::DroneOverlayClip
            || descriptorId == generators::DslrClip || descriptorId == generators::DveClip
            || descriptorId == generators::VideoCallClip
            || descriptorId == generators::VideoCameraClip)
            fillTemplate(clip, descriptorId, frameSize);
        if (descriptorId == generators::CompositionClip) {
            // A transparent solid with an empty composition: nothing shows until the user
            // adds generator or media nodes on the Compositing page.
            clip.kind = ClipKind::Solid;
            clip.solidColor = QColor(0, 0, 0, 0);
            clip.composition = compops::defaultComposition();
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

    // The tail of a clip touching its right neighbour is that neighbour's head: placeTransition
    // stores the cut's one transition there (a transitionOut on the left clip would not show).
    project.beginEdit(tr("Add %1").arg(desc->name));
    const bool placed = editops::placeTransition(project.timeline()->mutableData(), clipId,
        atHead, registry.createTransition(descriptorId, frames));
    if (!placed) {
        project.cancelEdit();
        return false;
    }
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

quint64 addGeneratorClip(Project& project, const QString& descriptorId, int64_t frame,
    int64_t durationFrames, int videoTrack, const std::function<void(ClipData&)>& prepare)
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
    if (videoTrack >= 0) {
        if (videoTrack >= int(current.videoTracks.size())
            || current.videoTracks[size_t(videoTrack)].locked)
            return 0;
        target = videoTrack;
    }

    std::optional<ClipData> made
        = makeGeneratorClip(descriptorId, current.fps, QSizeF(current.width, current.height));
    if (!made)
        return 0;
    ClipData clip = std::move(*made);
    clip.start = start;
    clip.duration = length;
    if (prepare)
        prepare(clip);

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
        return ctx.selection && applyEffect(project, ctx.selection->clips(), descriptorId) > 0;
    case EffectCategory::AudioEffect: {
        // The audio part of a selected linked video clip gets the effect too.
        if (!ctx.selection)
            return false;
        QVector<quint64> targets = ctx.selection->clips();
        for (quint64 id : ctx.selection->clips()) {
            for (quint64 linked : editops::linkedClips(project.timeline()->data(), id)) {
                if (!targets.contains(linked))
                    targets.append(linked);
            }
        }
        return applyEffect(project, targets, descriptorId) > 0;
    }
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
