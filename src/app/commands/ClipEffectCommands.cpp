// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: effect.add, effect.stack, effect.set, effect.remove, effect.enable (the video and
// audio effects of a clip). The effect ids are those of effect.list; adding goes through
// effectactions::applyEffect(), the function behind the Effects library, and parameters are
// written through the Inspector's ParamAccess with the descriptor's ranges.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "app/commands/ParamSupport.h"
#include "core/EditOps.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/effects/EffectActions.h"
#include "ui/timeline/TimelineEditUtil.h"

namespace kk::commands {

namespace ops = editops;
using timelineedit::runEdit;

namespace {

const ParamSpec kClipParam {QStringLiteral("clip"), ParamType::Integer,
    QStringLiteral("clip id (see clip.list)"), true, {}, {}};
const ParamSpec kEffectParam {QStringLiteral("effect"), ParamType::Integer,
    QStringLiteral("effect instance id (see effect.stack)"), true, {}, {}};

const TimelineData& dataOf(CommandContext& ctx)
{
    return ctx.project->timeline()->data();
}

QJsonObject effectToJson(const ClipData& clip, const EffectInstance& fx)
{
    QJsonObject params;
    QJsonArray animated;
    const EffectDescriptor* d = EffectRegistry::instance().find(fx.type);
    if (d) {
        for (const ParamDescriptor& p : d->params) {
            const ParamRef ref {
                clip.id, ParamScope::Effect, fx.id, p.key};
            params.insert(p.key, paramValueJson(clip, ref, p, 0));
            if (p.isScalar() && paramaccess::animated(clip, ref)
                && paramaccess::animated(clip, ref)->isAnimated())
                animated.append(p.key);
        }
    }
    return {{QStringLiteral("id"), qint64(fx.id)}, {QStringLiteral("type"), fx.type},
        {QStringLiteral("name"), d ? d->name : fx.type}, {QStringLiteral("enabled"), fx.enabled},
        {QStringLiteral("parameters"), params}, {QStringLiteral("keyframed"), animated}};
}

/// The clip, refusing a locked track; the effect instance too when @p effectId is not null.
std::optional<CommandResult> findTarget(CommandContext& ctx, quint64 clipId, quint64 effectId,
    FoundClip& found, const EffectInstance** fx)
{
    const TimelineData& d = dataOf(ctx);
    if (auto bad = findClipOrFail(d, clipId, found))
        return bad;
    if (d.track(found.track)->locked)
        return fail(
            "track_locked", QStringLiteral("track %1 is locked").arg(trackName(found.track)));
    if (effectId != 0) {
        const EffectInstance* instance = paramaccess::effect(*found.clip, effectId);
        if (!instance)
            return fail("unknown_effect",
                QStringLiteral("clip %1 has no effect instance %2 (see effect.stack)")
                    .arg(clipId)
                    .arg(effectId));
        if (fx)
            *fx = instance;
    }
    return std::nullopt;
}

CommandResult runAdd(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const quint64 clipId = quint64(args.integer(QStringLiteral("clip")));
    const QString type = args.string(QStringLiteral("type"));
    FoundClip found;
    if (auto bad = findTarget(ctx, clipId, 0, found, nullptr))
        return *bad;
    const EffectDescriptor* d = EffectRegistry::instance().find(type);
    if (!d
        || (d->category != EffectCategory::VideoEffect
            && d->category != EffectCategory::AudioEffect))
        return fail("unknown_effect",
            QStringLiteral("\"%1\" is not a video or audio effect (see effect.list)").arg(type));
    if (!effectactions::effectFitsTrack(d->category, found.track.type))
        return fail("bad_value",
            QStringLiteral("%1 is %2 effect and clip %3 is on %4 track %5")
                .arg(type,
                    d->category == EffectCategory::AudioEffect ? QStringLiteral("an audio")
                                                               : QStringLiteral("a video"))
                .arg(clipId)
                .arg(found.track.type == TrackType::Audio ? QStringLiteral("the audio")
                                                          : QStringLiteral("the video"))
                .arg(trackName(found.track)));
    QVector<ParamSetting> settings;
    if (auto bad = checkSettings(*d, settingsOf(args), settings))
        return *bad;

    // The Effects library's own function appends the instance; the settings follow in the same
    // edit (one undo step in the window).
    project.beginEdit(QStringLiteral("Add %1").arg(d->name));
    effectactions::applyEffect(project, {clipId}, type);
    quint64 id = 0;
    runEdit(&project, QStringLiteral("Add %1").arg(d->name), [&](TimelineData& data) {
        ClipData* clip = ops::clipById(data, clipId);
        if (!clip || clip->effects.isEmpty())
            return false;
        id = clip->effects.last().id;
        applySettings(*clip, {clipId, ParamScope::Effect, id, {}}, settings);
        return true;
    });
    project.endEdit();
    if (id == 0)
        return fail("edit_refused", QStringLiteral("the effect was not added"));
    const ClipData& clip = *ops::clipById(dataOf(ctx), clipId);
    return CommandResult::success(
        {{QStringLiteral("effect"), effectToJson(clip, clip.effects.last())}},
        QStringLiteral("added %1 to clip %2 as effect %3").arg(type).arg(clipId).arg(id));
}

CommandResult runStack(CommandContext& ctx, const CommandArgs& args)
{
    const quint64 clipId = quint64(args.integer(QStringLiteral("clip")));
    FoundClip found;
    if (auto bad = findClipOrFail(dataOf(ctx), clipId, found))
        return *bad;
    QJsonArray list;
    QString text
        = QStringLiteral("%1 effect(s) on clip %2").arg(found.clip->effects.size()).arg(clipId);
    for (const EffectInstance& fx : found.clip->effects) {
        list.append(effectToJson(*found.clip, fx));
        text += QStringLiteral("\n  %1  %2%3")
                    .arg(fx.id, -6)
                    .arg(fx.type, fx.enabled ? QString() : QStringLiteral("  (off)"));
    }
    return CommandResult::success({{QStringLiteral("effects"), list}}, text);
}

CommandResult runSet(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const quint64 clipId = quint64(args.integer(QStringLiteral("clip")));
    const quint64 effectId = quint64(args.integer(QStringLiteral("effect")));
    FoundClip found;
    const EffectInstance* fx = nullptr;
    if (auto bad = findTarget(ctx, clipId, effectId, found, &fx))
        return *bad;
    const EffectDescriptor* d = EffectRegistry::instance().find(fx->type);
    if (!d)
        return fail("unknown_effect", QStringLiteral("effect type %1 is not known").arg(fx->type));
    const QStringList entries = settingsOf(args);
    if (entries.isEmpty())
        return fail("missing_parameter",
            QStringLiteral("nothing to set: give key and value, or set key=value"));
    QVector<ParamSetting> settings;
    if (auto bad = checkSettings(*d, entries, settings))
        return *bad;
    runEdit(&project, QStringLiteral("Set %1").arg(d->name), [&](TimelineData& data) {
        applySettings(*ops::clipById(data, clipId),
            {clipId, ParamScope::Effect, effectId, {}}, settings);
        return true;
    });
    const ClipData& clip = *ops::clipById(dataOf(ctx), clipId);
    return CommandResult::success(
        {{QStringLiteral("effect"), effectToJson(clip, *paramaccess::effect(clip, effectId))}},
        QStringLiteral("updated effect %1 of clip %2").arg(effectId).arg(clipId));
}

CommandResult runRemove(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const quint64 clipId = quint64(args.integer(QStringLiteral("clip")));
    const quint64 effectId = quint64(args.integer(QStringLiteral("effect")));
    FoundClip found;
    if (auto bad = findTarget(ctx, clipId, effectId, found, nullptr))
        return *bad;
    runEdit(&project, QStringLiteral("Remove Effect"), [&](TimelineData& data) {
        ClipData* clip = ops::clipById(data, clipId);
        for (qsizetype i = 0; i < clip->effects.size(); ++i) {
            if (clip->effects[i].id == effectId) {
                clip->effects.removeAt(i);
                return true;
            }
        }
        return false;
    });
    return CommandResult::success({{QStringLiteral("removed"), qint64(effectId)}},
        QStringLiteral("removed effect %1 from clip %2").arg(effectId).arg(clipId));
}

CommandResult runEnable(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const quint64 clipId = quint64(args.integer(QStringLiteral("clip")));
    const quint64 effectId = quint64(args.integer(QStringLiteral("effect")));
    const bool enabled = args.boolean(QStringLiteral("enabled"), true);
    FoundClip found;
    if (auto bad = findTarget(ctx, clipId, effectId, found, nullptr))
        return *bad;
    runEdit(&project, enabled ? QStringLiteral("Enable Effect") : QStringLiteral("Disable Effect"),
        [&](TimelineData& data) {
            EffectInstance* fx = paramaccess::effect(*ops::clipById(data, clipId), effectId);
            if (fx->enabled == enabled)
                return false;
            fx->enabled = enabled;
            return true;
        });
    return CommandResult::success(
        {{QStringLiteral("effect"), qint64(effectId)}, {QStringLiteral("enabled"), enabled}},
        QStringLiteral("effect %1 of clip %2 is %3")
            .arg(effectId)
            .arg(clipId)
            .arg(enabled ? QStringLiteral("on") : QStringLiteral("off")));
}

} // namespace

void registerClipEffectCommands(CommandRegistry& registry)
{
    registry.add({{QStringLiteral("effect.add"),
                      QStringLiteral("Add a video or audio effect to a clip (video effects on "
                                     "video clips, audio effects on audio clips), with optional "
                                     "parameters. Ids and parameters: effect.list."),
                      {kClipParam,
                          {QStringLiteral("type"), ParamType::String,
                              QStringLiteral("effect id (see effect.list)"), true, {}, {}},
                          setListParam("effect")},
                      ProjectUse::Modify, true},
        runAdd});
    registry.add({{QStringLiteral("effect.stack"),
                      QStringLiteral("List the effects on a clip in the order they apply, with "
                                     "instance id, state and parameter values."),
                      {kClipParam}, ProjectUse::Read, true},
        runStack});
    registry.add(
        {{QStringLiteral("effect.set"),
             QStringLiteral("Set parameters of an effect on a clip, by key. Values are checked "
                            "against the range, choices and type of the effect's descriptor "
                            "(effect.list --id). Give key and value, and/or a set list."),
             {kClipParam, kEffectParam,
                 {QStringLiteral("key"), ParamType::String, QStringLiteral("parameter key"), false,
                     {}, {}},
                 {QStringLiteral("value"), ParamType::String, QStringLiteral("its new value"),
                     false, {}, {}},
                 setListParam("effect")},
             ProjectUse::Modify, true},
            runSet});
    registry.add({{QStringLiteral("effect.remove"), QStringLiteral("Remove an effect from a clip."),
                      {kClipParam, kEffectParam}, ProjectUse::Modify, true},
        runRemove});
    registry.add({{QStringLiteral("effect.enable"),
                      QStringLiteral("Switch an effect of a clip on or off (default on)."),
                      {kClipParam, kEffectParam,
                          {QStringLiteral("enabled"), ParamType::Boolean,
                              QStringLiteral("true to switch on, false to switch off"), false,
                              QStringLiteral("true"), {}}},
                      ProjectUse::Modify, true},
        runEnable});
}

} // namespace kk::commands
