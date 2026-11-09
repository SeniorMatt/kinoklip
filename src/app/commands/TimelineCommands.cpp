// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: timeline.list, timeline.new, timeline.select.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/Project.h"

namespace kk::commands {

namespace {

constexpr int kMaxTracks = 99; ///< tracks per type in a new timeline

CommandResult runList(CommandContext& ctx, const CommandArgs&)
{
    const Project& project = *ctx.project;
    QJsonArray list;
    QString text;
    for (quint64 uid : project.timelineUids()) {
        const auto data = project.timelineData(uid);
        if (!data)
            continue;
        const QJsonObject o = timelineToJson(*data, uid == project.activeTimelineUid());
        list.append(o);
        text += QStringLiteral("%1%2 %3: %4 clips, %5\n")
                    .arg(o[QStringLiteral("active")].toBool() ? QStringLiteral("*")
                                                              : QStringLiteral(" "))
                    .arg(uid)
                    .arg(data->name)
                    .arg(o[QStringLiteral("clips")].toInt())
                    .arg(o[QStringLiteral("durationTimecode")].toString());
    }
    return CommandResult::success({{QStringLiteral("timelines"), list}}, text.trimmed());
}

CommandResult runNew(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    const int video = int(args.integer(QStringLiteral("video-tracks"), 1));
    const int audio = int(args.integer(QStringLiteral("audio-tracks"), 1));
    if (video < 1 || video > kMaxTracks || audio < 0 || audio > kMaxTracks)
        return fail("bad_value",
            QStringLiteral("track counts: 1 - %1 video, 0 - %1 audio").arg(kMaxTracks));
    const QString name = args.has(QStringLiteral("name"))
        ? args.string(QStringLiteral("name"))
        : project.uniqueTimelineName(QStringLiteral("Timeline"));
    if (name.trimmed().isEmpty())
        return fail("bad_value", QStringLiteral("the name is empty"));
    const quint64 uid = project.createTimeline(name, {}, video, audio);
    return CommandResult::success(
        {{QStringLiteral("id"), qint64(uid)}, {QStringLiteral("name"), name}},
        QStringLiteral("created timeline %1 \"%2\"").arg(uid).arg(name));
}

CommandResult runSelect(CommandContext& ctx, const CommandArgs&)
{
    const quint64 uid = ctx.project->activeTimelineUid(); // already switched by the registry
    const auto data = ctx.project->timelineData(uid);
    return CommandResult::success({{QStringLiteral("id"), qint64(uid)},
                                      {QStringLiteral("name"), data ? data->name : QString()}},
        QStringLiteral("active timeline: %1 \"%2\"").arg(uid).arg(data ? data->name : QString()));
}

} // namespace

void registerTimelineCommands(CommandRegistry& registry)
{
    registry.add(
        {{QStringLiteral("timeline.list"), QStringLiteral("List the timelines of the project."), {},
             ProjectUse::Read, false},
            runList});
    registry.add(
        {{QStringLiteral("timeline.new"),
             QStringLiteral("Create an empty timeline (it does not become active)."),
             {{QStringLiteral("name"), ParamType::String,
                  QStringLiteral("timeline name (default: Timeline N)"), false, {}, {}},
                 {QStringLiteral("video-tracks"), ParamType::Integer,
                     QStringLiteral("number of video tracks"), false, QStringLiteral("1"), {}},
                 {QStringLiteral("audio-tracks"), ParamType::Integer,
                     QStringLiteral("number of audio tracks"), false, QStringLiteral("1"), {}}},
             ProjectUse::Modify, false},
            runNew});
    registry.add({{QStringLiteral("timeline.select"),
                      QStringLiteral(
                          "Make a timeline the project's active timeline (saved in the project)."),
                      {}, ProjectUse::Modify, true, true},
        runSelect});
}

} // namespace kk::commands
