// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: keyframe.set, keyframe.list, keyframe.delete. A keyframe belongs to one animatable
// number of a clip: one of its own (opacity, posX, volumeDb, ...) or of one of its effects (give
// the effect instance id). Times are timeline frames; the model keeps them clip-local, like the
// Keyframes panel. Edits use ParamAccess and KeyframeOps, the code behind the Inspector.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "app/commands/ParamSupport.h"
#include "core/Animation.h"
#include "core/EditOps.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/common/TimecodeFormat.h"
#include "ui/timeline/TimelineEditUtil.h"

namespace kk::commands {

namespace ops = editops;

namespace {

const QStringList kInterps {QStringLiteral("linear"), QStringLiteral("easeIn"),
    QStringLiteral("easeOut"), QStringLiteral("easeInOut"), QStringLiteral("smooth"),
    QStringLiteral("hold")};

const ParamSpec kClip {QStringLiteral("clip"), ParamType::Integer,
    QStringLiteral("clip id (see clip.list)"), true, {}, {}};
const ParamSpec kParam {QStringLiteral("param"), ParamType::String,
    QStringLiteral("parameter key, e.g. opacity, posX, posY, zoomX, rotation, cropL, volumeDb, "
                   "pan, or a key of the effect"),
    true, {}, {}};
const ParamSpec kEffect {QStringLiteral("effect"), ParamType::Integer,
    QStringLiteral("effect instance id (see effect.stack) when the parameter is an effect's"),
    false, {}, {}};

const ParamSpec kLayer {QStringLiteral("layer"), ParamType::Integer,
    QStringLiteral("title layer id (see title.layer_list) when the parameter is a layer's; 0 is "
                   "the first layer"),
    false, {}, {}};

const TimelineData& dataOf(CommandContext& ctx)
{
    return ctx.project->timeline()->data();
}

/// The key path of a resolved parameter, as KeyframeOps addresses it.
QString pathOf(const ResolvedParam& p)
{
    switch (p.ref.scope) {
    case ParamScope::Effect:
        return keyframeops::effectPath(p.ref.effectId, p.ref.key);
    case ParamScope::Audio:
        return keyframeops::memberPath(QStringLiteral("audio"), p.ref.key);
    case ParamScope::Title:
        return keyframeops::memberPath(QStringLiteral("title"), p.ref.key);
    case ParamScope::TitleLayer:
        return keyframeops::layerPath(p.ref.effectId, p.ref.key);
    default:
        return keyframeops::memberPath(QStringLiteral("transform"), p.ref.key);
    }
}

/// Finds the clip and the animatable parameter @p param (of @p effect); the key time @p at as a
/// clip-local frame when the command has one.
std::optional<CommandResult> locate(CommandContext& ctx, const CommandArgs& args, bool needTime,
    FoundClip& found, ResolvedParam& param, int64_t& local)
{
    const TimelineData& d = dataOf(ctx);
    if (auto bad = findClipOrFail(d, quint64(args.integer(QStringLiteral("clip"))), found))
        return bad;
    if (d.track(found.track)->locked)
        return fail(
            "track_locked", QStringLiteral("track %1 is locked").arg(trackName(found.track)));
    const QString layerName = QStringLiteral("layer");
    if (auto bad = args.has(layerName)
            ? resolveLayerParam(*found.clip, quint64(args.integer(layerName)),
                  args.string(QStringLiteral("param")), param)
            : resolveParam(*found.clip, quint64(args.integer(QStringLiteral("effect"))),
                  args.string(QStringLiteral("param")), param))
        return bad;
    if (!param.descriptor->isScalar() || !param.descriptor->animatable)
        return fail("not_animatable",
            QStringLiteral("\"%1\" is not a number that can be keyframed").arg(param.ref.key));
    if (needTime) {
        std::optional<int64_t> at;
        if (auto bad = readTime(args, QStringLiteral("at"), d.fps, at))
            return bad;
        local = *at - found.clip->start;
        if (local < 0 || local >= found.clip->duration)
            return fail("out_of_range",
                QStringLiteral("the time must be inside clip %1 (frames %2 to %3)")
                    .arg(found.clip->id)
                    .arg(found.clip->start)
                    .arg(found.clip->end() - 1));
    }
    return std::nullopt;
}

QJsonObject keysJson(const ClipData& clip, const AnimatedValue& value, Rational fps)
{
    QJsonArray keys;
    for (const Keyframe& k : value.keys)
        keys.append(QJsonObject {{QStringLiteral("at"), qint64(clip.start + k.frame)},
            {QStringLiteral("timecode"), timecode::format(clip.start + k.frame, fps)},
            {QStringLiteral("value"), k.value},
            {QStringLiteral("interp"), interpToString(keyInterp(k))}});
    return {{QStringLiteral("keys"), keys}};
}

CommandResult runSet(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    FoundClip found;
    ResolvedParam param;
    int64_t local = 0;
    if (auto bad = locate(ctx, args, true, found, param, local))
        return *bad;
    ParamSetting value;
    if (auto bad = checkParamValue(*param.descriptor, args.string(QStringLiteral("value")), value))
        return *bad;
    const bool hasInterp = args.has(QStringLiteral("interp"));
    const Interp interp = interpFromString(args.string(QStringLiteral("interp")));
    const quint64 id = found.clip->id;
    timelineedit::runEdit(&project, QStringLiteral("Set Keyframe"), [&](TimelineData& data) {
        ClipData& clip = *ops::clipById(data, id);
        // A key at the time first (with the current value, as the diamond button does), then the
        // value: setScalar() writes a key once the parameter is animated.
        if (!paramaccess::animated(clip, param.ref)
            || !paramaccess::animated(clip, param.ref)->hasKeyAt(local))
            paramaccess::toggleKey(clip, param.ref, *param.descriptor, local);
        paramaccess::setScalar(clip, param.ref, *param.descriptor, local, value.number);
        if (hasInterp)
            paramaccess::setKeyInterp(clip, param.ref, local, interp);
        return true;
    });
    const ClipData& clip = *ops::clipById(dataOf(ctx), id);
    return CommandResult::success(
        {{QStringLiteral("param"), param.ref.key}, {QStringLiteral("clip"), qint64(id)},
            {QStringLiteral("at"), qint64(clip.start + local)},
            {QStringLiteral("value"), value.number}},
        QStringLiteral("keyframe %1 = %2 at frame %3 of clip %4")
            .arg(param.ref.key)
            .arg(value.number)
            .arg(clip.start + local)
            .arg(id));
}

CommandResult runList(CommandContext& ctx, const CommandArgs& args)
{
    const TimelineData& d = dataOf(ctx);
    FoundClip found;
    if (auto bad = findClipOrFail(d, quint64(args.integer(QStringLiteral("clip"))), found))
        return *bad;
    const ClipData& clip = *found.clip;
    const QString only = args.string(QStringLiteral("param"));
    const quint64 onlyEffect = quint64(args.integer(QStringLiteral("effect")));
    QJsonArray list;
    int total = 0;
    for (const keyframeops::AnimatedParam& p : keyframeops::animatedParams(clip)) {
        const AnimatedValue* value = keyframeops::findValue(clip, p.path);
        if (!value || !value->isAnimated())
            continue;
        const QStringList parts = p.path.split(QLatin1Char('/'));
        const QString key = parts.last();
        const quint64 effect
            = parts.value(0) == QLatin1String("effect") ? parts.value(1).toULongLong() : 0;
        const bool inLayer = parts.value(0) == QLatin1String("layer");
        const quint64 layer = inLayer ? parts.value(1).toULongLong() : 0;
        if ((!only.isEmpty() && key != only) || (onlyEffect != 0 && effect != onlyEffect))
            continue;
        if (args.has(QStringLiteral("layer"))) {
            const quint64 wanted = quint64(args.integer(QStringLiteral("layer")));
            const bool match = wanted == 0 ? parts.value(0) == QLatin1String("title")
                                           : inLayer && layer == wanted;
            if (!match)
                continue;
        }
        QJsonObject o = keysJson(clip, *value, d.fps);
        o.insert(QStringLiteral("param"), key);
        o.insert(QStringLiteral("label"), p.label);
        o.insert(QStringLiteral("group"), p.groupName);
        if (effect != 0)
            o.insert(QStringLiteral("effect"), qint64(effect));
        if (inLayer)
            o.insert(QStringLiteral("layer"), qint64(layer));
        total += int(value->keys.size());
        list.append(o);
    }
    QString text = QStringLiteral("%1 keyframed parameter(s), %2 key(s) on clip %3")
                       .arg(list.size())
                       .arg(total)
                       .arg(clip.id);
    for (const QJsonValue& v : std::as_const(list))
        text += QStringLiteral("\n  %1: %2 key(s)")
                    .arg(v.toObject()[QStringLiteral("param")].toString())
                    .arg(v.toObject()[QStringLiteral("keys")].toArray().size());
    return CommandResult::success({{QStringLiteral("parameters"), list}}, text);
}

CommandResult runDelete(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    FoundClip found;
    ResolvedParam param;
    int64_t local = 0;
    if (auto bad = locate(ctx, args, true, found, param, local))
        return *bad;
    const AnimatedValue* value = paramaccess::animated(*found.clip, param.ref);
    if (!value || !value->hasKeyAt(local))
        return fail("no_keyframe",
            QStringLiteral("%1 has no keyframe at frame %2 of clip %3 (keyframe.list)")
                .arg(param.ref.key)
                .arg(found.clip->start + local)
                .arg(found.clip->id));
    const quint64 id = found.clip->id;
    const KeyframeRef ref {id, pathOf(param), local};
    int removed = 0;
    timelineedit::runEdit(&project, QStringLiteral("Delete Keyframe"), [&](TimelineData& data) {
        removed = keyframeops::deleteKeys(data, {ref});
        return removed > 0;
    });
    if (removed == 0)
        return fail("edit_refused", QStringLiteral("the keyframe was not removed"));
    return CommandResult::success({{QStringLiteral("removed"), removed}},
        QStringLiteral("deleted the %1 keyframe at frame %2 of clip %3")
            .arg(param.ref.key)
            .arg(found.clip->start + local)
            .arg(id));
}

} // namespace

void registerKeyframeCommands(CommandRegistry& registry)
{
    const ParamSpec at {QStringLiteral("at"), ParamType::Time,
        QStringLiteral("timeline time of the keyframe; inside the clip"), true, {}, {}};
    registry.add(
        {{QStringLiteral("keyframe.set"),
             QStringLiteral("Set a keyframe: the value of a number (opacity, position, "
                            "volume, an effect's parameter) at a time of a clip; the "
                            "value changes between keyframes. Adds the key or changes "
                            "the one at that time."),
             {kClip, kParam, kEffect, kLayer, at,
                 {QStringLiteral("value"), ParamType::String,
                     QStringLiteral("the value, inside the range of the parameter"), true, {}, {}},
                 {QStringLiteral("interp"), ParamType::String,
                     QStringLiteral("how the value moves from this key to the next"), false,
                     QStringLiteral("linear"), kInterps}},
             ProjectUse::Modify, true},
            runSet});
    registry.add({{QStringLiteral("keyframe.list"),
                      QStringLiteral("List the keyframes of a clip by parameter: time, value, "
                                     "interpolation. Optionally only one parameter."),
                      {kClip,
                          {QStringLiteral("param"), ParamType::String,
                              QStringLiteral("only this parameter key"), false, {}, {}},
                          kEffect, kLayer},
                      ProjectUse::Read, true},
        runList});
    registry.add({{QStringLiteral("keyframe.delete"),
                      QStringLiteral("Delete the keyframe of a parameter at a time. The "
                                     "parameter keeps the value of the last key removed."),
                      {kClip, kParam, kEffect, kLayer, at}, ProjectUse::Modify, true},
        runDelete});
}

} // namespace kk::commands
