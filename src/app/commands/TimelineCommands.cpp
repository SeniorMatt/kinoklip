// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: timeline.list, timeline.new, timeline.select, timeline.convert-rate.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/FrameRateConvert.h"
#include "core/Project.h"

#include <QFileInfo>

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

CommandResult runConvertRate(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    if (ctx.live)
        return fail("not_allowed", QStringLiteral("use it on a project file, not the open window"));
    // A converted copy is always a new file: the original is never touched.
    const QString output = args.string(QStringLiteral("output"));
    if (output.isEmpty())
        return fail("bad_value", QStringLiteral("--output <new project file> is required"));
    const QFileInfo out(output);
    if (out.exists())
        return fail("file_exists", QStringLiteral("%1 exists; choose a new file name").arg(output));
    if (out.absoluteFilePath() == QFileInfo(ctx.projectPath).absoluteFilePath())
        return fail("bad_value", QStringLiteral("--output must differ from --project"));
    const double wanted = args.number(QStringLiteral("fps"));
    if (!(wanted >= 1.0 && wanted <= 1000.0))
        return fail("bad_value", QStringLiteral("fps must be between 1 and 1000"));
    const Rational fps = rationalFromFps(wanted);
    const quint64 uid = project.activeTimelineUid(); // already switched by --timeline
    const auto before = project.timelineData(uid);
    if (!before)
        return fail("unknown_timeline", QStringLiteral("no such timeline"));
    if (before->fps == fps)
        return fail("bad_value", QStringLiteral("the timeline already runs at that frame rate"));
    const Rational oldFps = before->fps;
    if (!project.convertTimelineFrameRate(
            uid, fps, args.boolean(QStringLiteral("with-compounds"), true)))
        return fail("failed", QStringLiteral("could not convert the timeline"));
    const bool projectMoved = project.settings().fps == fps;
    return CommandResult::success(
        {{QStringLiteral("id"), qint64(uid)}, {QStringLiteral("fromFps"), oldFps.toDouble()},
            {QStringLiteral("fps"), fps.toDouble()},
            {QStringLiteral("projectFps"), project.settings().fps.toDouble()}},
        QStringLiteral("timeline %1 \"%2\": %3 fps -> %4 fps, everything kept at the same time%5; "
                       "saved to %6")
            .arg(uid)
            .arg(before->name)
            .arg(oldFps.toDouble(), 0, 'g', 6)
            .arg(fps.toDouble(), 0, 'g', 6)
            .arg(projectMoved ? QStringLiteral(" (project frame rate changed too)") : QString())
            .arg(output));
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
                      {}, ProjectUse::Modify, true, true, false},
        runSelect});
    registry.add({{QStringLiteral("timeline.convert-rate"),
                      QStringLiteral("Change the frame rate of a timeline that has clips and save "
                                     "the result as a NEW project file (--output, required, must "
                                     "not exist; the original is never changed). Clips, "
                                     "keyframes, markers, fades and transitions stay at the same "
                                     "moment in time. A timeline that follows the project "
                                     "settings takes the project along."),
                      {{QStringLiteral("fps"), ParamType::Number,
                           QStringLiteral("new frame rate, e.g. 60, 29.97, 59.94"), true, {}, {}},
                          {QStringLiteral("with-compounds"), ParamType::Boolean,
                              QStringLiteral("also convert the compound and multicam clips it "
                                             "shows that run at its old rate"),
                              false, QStringLiteral("true"), {}}},
                      ProjectUse::Modify, true, false, false},
        runConvertRate});
}

} // namespace kk::commands
