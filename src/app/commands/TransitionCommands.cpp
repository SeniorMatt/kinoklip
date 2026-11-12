// SPDX-License-Identifier: GPL-3.0-or-later
// Command: transition.add (a video or audio transition at the head or tail of a clip).
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "app/commands/ParamSupport.h"
#include "core/AudioTransitions.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/effects/EffectActions.h"
#include "ui/timeline/TimelineEditUtil.h"

namespace kk::commands {

namespace ops = editops;

namespace {

CommandResult runAdd(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& d = project.timeline()->data();
    const quint64 id = quint64(args.integer(QStringLiteral("clip")));
    FoundClip found;
    if (auto bad = findClipOrFail(d, id, found))
        return *bad;
    const bool head = args.string(QStringLiteral("edge")) == QLatin1String("head");
    const bool audio = found.track.type == TrackType::Audio;
    const QString type = args.has(QStringLiteral("type")) ? args.string(QStringLiteral("type"))
                                                          : project.standardTransitionId(audio);
    const EffectDescriptor* descriptor = EffectRegistry::instance().find(type);
    const EffectCategory wanted
        = audio ? EffectCategory::AudioTransition : EffectCategory::Transition;
    if (!descriptor || descriptor->category != wanted)
        return fail("bad_value",
            QStringLiteral("\"%1\" is not a %2 transition")
                .arg(type, audio ? QStringLiteral("audio") : QStringLiteral("video")));
    std::optional<int64_t> duration;
    if (auto bad = readTime(args, QStringLiteral("duration"), d.fps, duration))
        return *bad;
    TransitionData transition;
    transition.type = type;
    transition.duration = duration.value_or(project.standardTransitionFrames());
    if (transition.duration < 1)
        return fail("out_of_range", QStringLiteral("the duration must be at least one frame"));
    if (d.track(found.track)->locked)
        return fail(
            "track_locked", QStringLiteral("track %1 is locked").arg(trackName(found.track)));

    const bool placed = timelineedit::runEdit(&project, QStringLiteral("Add Transition"),
        [&](TimelineData& data) { return ops::placeTransition(data, id, head, transition); });
    if (!placed)
        return fail("edit_refused", QStringLiteral("the transition does not fit at that edge"));
    return CommandResult::success({{QStringLiteral("clip"),
                                      clipToJson(project.timeline()->data(), found.track,
                                          *ops::clipById(project.timeline()->data(), id))}},
        QStringLiteral("added %1 at the %2 of clip %3")
            .arg(type, head ? QStringLiteral("head") : QStringLiteral("tail"))
            .arg(id));
}

/// Where the transition at the @p head or tail of clip @p id is stored: a tail that touches the
/// next clip is that clip's head transition (the cut has one transition, editops::placeTransition).
struct TransitionSlot
{
    quint64 clip = 0;
    bool head = true;
    const TransitionData* transition = nullptr;
};

TransitionSlot slotOf(const TimelineData& d, const ClipData& clip, bool head)
{
    if (const auto& own = head ? clip.transitionIn : clip.transitionOut)
        return {clip.id, head, &*own};
    if (!head) {
        const quint64 next = ops::cutNeighbour(d, clip.id, false);
        if (const ClipData* n = next ? ops::clipById(d, next) : nullptr; n && n->transitionIn)
            return {n->id, true, &*n->transitionIn};
    }
    return {clip.id, head, nullptr};
}

CommandResult transitionJson(const TimelineData& d, quint64 id, const QString& what)
{
    const auto loc = ops::findClip(d, id);
    return CommandResult::success(
        {{QStringLiteral("clip"), clipToJson(d, loc->track, *ops::clipById(d, id))}}, what);
}

CommandResult runSet(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& d = project.timeline()->data();
    const quint64 id = quint64(args.integer(QStringLiteral("clip")));
    FoundClip found;
    if (auto bad = findClipOrFail(d, id, found))
        return *bad;
    if (d.track(found.track)->locked)
        return fail(
            "track_locked", QStringLiteral("track %1 is locked").arg(trackName(found.track)));
    const bool head = args.string(QStringLiteral("edge")) == QLatin1String("head");
    const TransitionSlot slot = slotOf(d, *found.clip, head);
    if (!slot.transition)
        return fail("no_transition",
            QStringLiteral("clip %1 has no transition at its %2 (transition.add makes one)")
                .arg(id)
                .arg(head ? QStringLiteral("head") : QStringLiteral("tail")));
    std::optional<int64_t> duration;
    if (auto bad = readTime(args, QStringLiteral("duration"), d.fps, duration))
        return *bad;
    if (duration && *duration < 1)
        return fail("out_of_range", QStringLiteral("the duration must be at least one frame"));
    const QString alignment = args.string(QStringLiteral("alignment"));
    const EffectDescriptor* descriptor = EffectRegistry::instance().find(slot.transition->type);
    QVector<ParamSetting> settings;
    if (descriptor) {
        if (auto bad = checkSettings(*descriptor, settingsOf(args), settings))
            return *bad;
    } else if (!settingsOf(args).isEmpty()) {
        return fail("unknown_effect",
            QStringLiteral("transition type %1 is not known").arg(slot.transition->type));
    }
    if (!duration && alignment.isEmpty() && settings.isEmpty())
        return fail("missing_parameter",
            QStringLiteral("nothing to set: give duration, alignment, key and value or set"));

    int64_t applied = 0;
    const bool changed = timelineedit::runEdit(
        &project, QStringLiteral("Set Transition"), [&](TimelineData& data) {
            bool any = false;
            if (!alignment.isEmpty()) {
                const TransitionAlignment a = alignment == QLatin1String("start-on-cut")
                    ? TransitionAlignment::StartOnCut
                    : alignment == QLatin1String("end-on-cut") ? TransitionAlignment::EndOnCut
                                                               : TransitionAlignment::Centre;
                any |= ops::setTransitionAlignment(data, slot.clip, slot.head, a);
            }
            if (duration) {
                // Clamped to what fits (clip lengths, handles); the result says what was used.
                applied = ops::setTransitionDuration(data, slot.clip, slot.head, *duration);
                any |= applied > 0;
            }
            if (!settings.isEmpty()) {
                ClipData* clip = ops::clipById(data, slot.clip);
                applySettings(*clip,
                    {slot.clip,
                        slot.head ? ParamScope::TransitionIn
                                  : ParamScope::TransitionOut,
                        0, {}},
                    settings);
                any = true;
            }
            return any;
        });
    if (!changed && !(duration && applied > 0))
        return fail("edit_refused", QStringLiteral("nothing changed (already so, or no room)"));
    return transitionJson(project.timeline()->data(), id,
        QStringLiteral("updated the transition at the %1 of clip %2%3")
            .arg(head ? QStringLiteral("head") : QStringLiteral("tail"))
            .arg(id)
            .arg(duration && applied != *duration
                    ? QStringLiteral(" (length limited to %1 frames)").arg(applied)
                    : QString()));
}

CommandResult runRemove(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& d = project.timeline()->data();
    const quint64 id = quint64(args.integer(QStringLiteral("clip")));
    FoundClip found;
    if (auto bad = findClipOrFail(d, id, found))
        return *bad;
    if (d.track(found.track)->locked)
        return fail(
            "track_locked", QStringLiteral("track %1 is locked").arg(trackName(found.track)));
    const bool head = args.string(QStringLiteral("edge")) == QLatin1String("head");
    const TransitionSlot slot = slotOf(d, *found.clip, head);
    if (!slot.transition)
        return fail("no_transition",
            QStringLiteral("clip %1 has no transition at its %2")
                .arg(id)
                .arg(head ? QStringLiteral("head") : QStringLiteral("tail")));
    // The same removal as the context menu of the timeline.
    effectactions::removeTransition(project, slot.clip, slot.head);
    return transitionJson(project.timeline()->data(), id,
        QStringLiteral("removed the transition at the %1 of clip %2")
            .arg(head ? QStringLiteral("head") : QStringLiteral("tail"))
            .arg(id));
}

} // namespace

void registerTransitionCommands(CommandRegistry& registry)
{
    registry.add({{QStringLiteral("transition.add"),
                      QStringLiteral(
                          "Add a transition at the head or tail of a clip (shortened to fit the "
                          "clips). Default: the project's standard transition (Cross Dissolve on "
                          "video, Cross Fade +3 dB on audio unless changed in Project Settings)."),
                      {{QStringLiteral("clip"), ParamType::Integer,
                           QStringLiteral("clip id (see clip.list)"), true, {}, {}},
                          {QStringLiteral("edge"), ParamType::String,
                              QStringLiteral("which edge of the clip"), true, {},
                              {QStringLiteral("head"), QStringLiteral("tail")}},
                          {QStringLiteral("type"), ParamType::String,
                              QStringLiteral("effect id of the transition, e.g. cross_dissolve"),
                              false, {}, {}},
                          {QStringLiteral("duration"), ParamType::Time,
                              QStringLiteral("length in frames or timecode"), false,
                              QStringLiteral("the project's standard transition duration"), {}}},
                      ProjectUse::Modify, true},
        runAdd});
    const ParamSpec clip {QStringLiteral("clip"), ParamType::Integer,
        QStringLiteral("clip id (see clip.list)"), true, {}, {}};
    const ParamSpec edge {QStringLiteral("edge"), ParamType::String,
        QStringLiteral("which edge of the clip"), true, {},
        {QStringLiteral("head"), QStringLiteral("tail")}};
    registry.add({{QStringLiteral("transition.set"),
                      QStringLiteral("Change the transition at the head or tail of a clip: its "
                                     "length (limited to what fits), its alignment on the cut, "
                                     "and parameters by key (transition.list --id shows them)."),
                      {clip, edge,
                          {QStringLiteral("duration"), ParamType::Time,
                              QStringLiteral("new length in frames or timecode"), false, {}, {}},
                          {QStringLiteral("alignment"), ParamType::String,
                              QStringLiteral("where it sits on the cut"), false, {},
                              {QStringLiteral("centre"), QStringLiteral("start-on-cut"),
                                  QStringLiteral("end-on-cut")}},
                          {QStringLiteral("key"), ParamType::String,
                              QStringLiteral("parameter key"), false, {}, {}},
                          {QStringLiteral("value"), ParamType::String,
                              QStringLiteral("its new value"), false, {}, {}},
                          setListParam("transition")},
                      ProjectUse::Modify, true},
        runSet});
    registry.add({{QStringLiteral("transition.remove"),
                      QStringLiteral("Remove the transition at the head or tail of a clip."),
                      {clip, edge}, ProjectUse::Modify, true},
        runRemove});
}

} // namespace kk::commands
