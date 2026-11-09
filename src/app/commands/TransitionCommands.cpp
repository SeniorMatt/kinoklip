// SPDX-License-Identifier: GPL-3.0-or-later
// Command: transition.add (a video or audio transition at the head or tail of a clip).
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/AudioTransitions.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/Timeline.h"
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
    const QString type = args.has(QStringLiteral("type"))
        ? args.string(QStringLiteral("type"))
        : (audio ? QString::fromLatin1(audiotransitions::kCrossFadePlus3dB)
                 : QStringLiteral("cross_dissolve"));
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
    transition.duration = duration.value_or(timecodeBase(d.fps)); // one second
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

} // namespace

void registerTransitionCommands(CommandRegistry& registry)
{
    registry.add(
        {{QStringLiteral("transition.add"),
             QStringLiteral("Add a transition at the head or tail of a clip (shortened to fit the "
                            "clips). Default: Cross Dissolve on video, Cross Fade +3 dB on audio."),
             {{QStringLiteral("clip"), ParamType::Integer,
                  QStringLiteral("clip id (see clip.list)"), true, {}, {}},
                 {QStringLiteral("edge"), ParamType::String,
                     QStringLiteral("which edge of the clip"), true, {},
                     {QStringLiteral("head"), QStringLiteral("tail")}},
                 {QStringLiteral("type"), ParamType::String,
                     QStringLiteral("effect id of the transition, e.g. cross_dissolve"), false, {},
                     {}},
                 {QStringLiteral("duration"), ParamType::Time,
                     QStringLiteral("length in frames or timecode"), false,
                     QStringLiteral("one second"), {}}},
             ProjectUse::Modify, true},
            runAdd});
}

} // namespace kk::commands
