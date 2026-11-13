// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: track.list, track.add, track.set (mute, lock, enable, solo, auto-select, sync-lock, name). The
// same fields the track header buttons of the timeline switch; addTrack() is the timeline's.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/EditOps.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "ui/timeline/TimelineEditUtil.h"

namespace kk::commands {

namespace ops = editops;

namespace {

constexpr int kMaxTracks = 99; ///< tracks per type, as for timeline.new

QJsonObject trackJson(const TrackData& t, TrackRef ref)
{
    return {{QStringLiteral("track"), trackName(ref)},
        {QStringLiteral("type"),
            t.type == TrackType::Video       ? QStringLiteral("video")
                : t.type == TrackType::Audio ? QStringLiteral("audio")
                                             : QStringLiteral("subtitle")},
        {QStringLiteral("name"), t.name}, {QStringLiteral("enabled"), t.enabled},
        {QStringLiteral("locked"), t.locked}, {QStringLiteral("muted"), t.muted},
        {QStringLiteral("solo"), t.solo}, {QStringLiteral("autoSelect"), t.autoSelect},
        {QStringLiteral("syncLock"), t.syncLock},
        {QStringLiteral("clips"), int(t.clips.size())}};
}

CommandResult runList(CommandContext& ctx, const CommandArgs&)
{
    const TimelineData& d = ctx.project->timeline()->data();
    QJsonArray list;
    QString text;
    for (TrackType type : {TrackType::Video, TrackType::Audio}) {
        for (int i = 0; i < int(d.tracks(type).size()); ++i) {
            const TrackData& t = d.tracks(type)[size_t(i)];
            const TrackRef ref {type, i};
            list.append(trackJson(t, ref));
            text += QStringLiteral("%1  %2 clip(s)%3%4%5\n")
                        .arg(trackName(ref), -4)
                        .arg(t.clips.size())
                        .arg(t.locked ? QStringLiteral("  locked") : QString())
                        .arg(t.muted ? QStringLiteral("  muted") : QString())
                        .arg(t.enabled ? QString() : QStringLiteral("  disabled"));
        }
    }
    return CommandResult::success({{QStringLiteral("tracks"), list}}, text.trimmed());
}

CommandResult runAdd(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TrackType type = args.string(QStringLiteral("type")) == QLatin1String("audio")
        ? TrackType::Audio
        : TrackType::Video;
    if (int(project.timeline()->data().tracks(type).size()) >= kMaxTracks)
        return fail("out_of_range",
            QStringLiteral("a timeline has at most %1 tracks of a kind").arg(kMaxTracks));
    int index = -1;
    timelineedit::runEdit(&project,
        type == TrackType::Video ? QStringLiteral("Add Video Track")
                                 : QStringLiteral("Add Audio Track"),
        [&](TimelineData& data) {
            index = ops::addTrack(data, type);
            return index >= 0;
        });
    if (index < 0)
        return fail("edit_refused", QStringLiteral("the track was not added"));
    const TimelineData& d = project.timeline()->data();
    const TrackRef ref {type, index};
    return CommandResult::success({{QStringLiteral("track"), trackJson(*d.track(ref), ref)}},
        QStringLiteral("added track %1").arg(trackName(ref)));
}

CommandResult runSet(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const TimelineData& d = project.timeline()->data();
    const auto ref = parseTrack(d, args.string(QStringLiteral("track")));
    if (!ref)
        return fail("unknown_track",
            QStringLiteral("no track \"%1\" (V1 ... V%2, A1 ... A%3; see track.list)")
                .arg(args.string(QStringLiteral("track")))
                .arg(d.videoTracks.size())
                .arg(d.audioTracks.size()));
    struct Flag
    {
        const char* option;
        bool TrackData::*field;
    };
    static const Flag flags[] = {{"enabled", &TrackData::enabled}, {"locked", &TrackData::locked},
        {"muted", &TrackData::muted}, {"solo", &TrackData::solo},
        {"auto-select", &TrackData::autoSelect}, {"sync-lock", &TrackData::syncLock}};
    bool any = args.has(QStringLiteral("name"));
    for (const Flag& f : flags)
        any |= args.has(QString::fromLatin1(f.option));
    if (!any)
        return fail(
            "missing_parameter", QStringLiteral("nothing to set: give at least one option"));
    timelineedit::runEdit(&project, QStringLiteral("Set Track"), [&](TimelineData& data) {
        TrackData& t = *data.track(*ref);
        for (const Flag& f : flags) {
            if (args.has(QString::fromLatin1(f.option)))
                t.*(f.field) = args.boolean(QString::fromLatin1(f.option));
        }
        if (args.has(QStringLiteral("name")))
            t.name = args.string(QStringLiteral("name"));
        return true;
    });
    return CommandResult::success(
        {{QStringLiteral("track"), trackJson(*project.timeline()->data().track(*ref), *ref)}},
        QStringLiteral("updated track %1").arg(trackName(*ref)));
}

} // namespace

void registerTrackCommands(CommandRegistry& registry)
{
    registry.add({{QStringLiteral("track.list"),
                      QStringLiteral("List the video and audio tracks of a timeline with their "
                                     "state (enabled, locked, muted, solo) and clip count."),
                      {}, ProjectUse::Read, true},
        runList});
    registry.add({{QStringLiteral("track.add"),
                      QStringLiteral("Add an empty video or audio track on top of its kind."),
                      {{QStringLiteral("type"), ParamType::String, QStringLiteral("track kind"),
                          true, {}, {QStringLiteral("video"), QStringLiteral("audio")}}},
                      ProjectUse::Modify, true},
        runAdd});
    const auto flag = [](const char* name, const char* description) {
        return ParamSpec {QString::fromLatin1(name), ParamType::Boolean,
            QString::fromLatin1(description), false, {}, {}};
    };
    registry.add({{QStringLiteral("track.set"),
                      QStringLiteral("Change a track: switch it on or off, lock it, mute or solo "
                                     "it, take part in edits without a target, or rename it."),
                      {{QStringLiteral("track"), ParamType::String,
                           QStringLiteral("the track: V1, V2, A1, ..."), true, {}, {}},
                          flag("enabled", "true: the track is visible in the output"),
                          flag("locked", "true: no edit changes the track"),
                          flag("muted", "true: the track is silent"),
                          flag("solo", "true: only soloed tracks are heard"),
                          flag("sync-lock", "true: the track ripples with edits on other tracks (sync lock)"),
                          flag("auto-select", "true: edits without a target include this track"),
                          {QStringLiteral("name"), ParamType::String,
                              QStringLiteral("track name (empty: V1, A2, ...)"), false, {}, {}}},
                      ProjectUse::Modify, true},
        runSet});
}

} // namespace kk::commands
