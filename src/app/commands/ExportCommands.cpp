// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: export.otio, export.edl, import.otio, render.
#include "app/TimelineExport.h"
#include "app/TimelineImport.h"
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "export/EdlExport.h"
#include "export/ExportPresets.h"
#include "export/RenderCommand.h"

#include <QFileInfo>

namespace kk::commands {

namespace {

const ParamSpec kFileParam {
    QStringLiteral("file"), ParamType::Path, QStringLiteral("file to write"), true, {}, {}};

CommandResult exportText(
    CommandContext& ctx, const CommandArgs& args, bool otio, const QString& label)
{
    const TimelineData& data = ctx.project->timeline()->data();
    const QString path = QFileInfo(args.string(QStringLiteral("file"))).absoluteFilePath();
    const MediaPool* pool = ctx.project->mediaPool();
    const QString text = otio ? timelineOtioText(data, pool) : timelineEdlText(data, pool);
    QString error;
    if (!edl::writeTextFile(path, text, &error))
        return fail("write_failed", error);
    return CommandResult::success(
        {{QStringLiteral("file"), path}, {QStringLiteral("bytes"), text.toUtf8().size()}},
        QStringLiteral("wrote %1 (%2)").arg(path, label));
}

CommandResult runExportOtio(CommandContext& ctx, const CommandArgs& args)
{
    return exportText(ctx, args, true, QStringLiteral("OpenTimelineIO"));
}

CommandResult runExportEdl(CommandContext& ctx, const CommandArgs& args)
{
    return exportText(ctx, args, false, QStringLiteral("EDL"));
}

CommandResult runImportOtio(CommandContext& ctx, const CommandArgs& args)
{
    const QString path = QFileInfo(args.string(QStringLiteral("file"))).absoluteFilePath();
    if (!QFileInfo(path).isFile())
        return fail("file_not_found", QStringLiteral("%1 does not exist").arg(path));
    const TimelineImportReport report = importOtioFile(*ctx.project, path);
    if (report.uid == 0)
        return fail("import_failed", report.error);
    return CommandResult::success(
        {{QStringLiteral("timeline"), qint64(report.uid)},
            {QStringLiteral("clips"), report.clipCount},
            {QStringLiteral("mediaAdded"), report.mediaAdded},
            {QStringLiteral("offline"), QJsonArray::fromStringList(report.offline)},
            {QStringLiteral("skipped"), QJsonArray::fromStringList(report.skipped)}},
        QStringLiteral("imported timeline %1: %2 clips, %3 file(s) offline")
            .arg(report.uid)
            .arg(report.clipCount)
            .arg(report.offline.size()));
}

CommandResult runRender(CommandContext& ctx, const CommandArgs& args)
{
    const QString output = QFileInfo(args.string(QStringLiteral("output"))).absoluteFilePath();
    // An image sequence pattern ("name_%05d.png") is not a file name that can be resolved.
    const QString target = args.string(QStringLiteral("output")).contains(QLatin1Char('%'))
        ? args.string(QStringLiteral("output"))
        : output;
    if (ctx.project->timeline()->data().duration() == 0)
        return fail("empty_timeline", QStringLiteral("the timeline has no clips to render"));
    const RenderOutcome outcome = renderProject(
        *ctx.project, target, args.string(QStringLiteral("preset")), [&](const ExportProgress& p) {
            if (ctx.progress)
                ctx.progress(p.fraction(),
                    QStringLiteral("%1/%2 frames, %3 fps")
                        .arg(p.framesDone)
                        .arg(p.totalFrames)
                        .arg(p.fps, 0, 'f', 1));
        });
    if (!outcome.ok)
        return fail("render_failed", outcome.error);
    const ExportResult& r = outcome.result;
    return CommandResult::success(
        {{QStringLiteral("output"), r.outputPath},
            {QStringLiteral("files"), QJsonArray::fromStringList(r.files)},
            {QStringLiteral("frames"), qint64(r.frames)}, {QStringLiteral("seconds"), r.seconds},
            {QStringLiteral("fps"), r.averageFps()},
            {QStringLiteral("videoEncoder"), r.videoEncoder}},
        QStringLiteral("rendered %1 frames in %2 s -> %3")
            .arg(r.frames)
            .arg(r.seconds, 0, 'f', 2)
            .arg(r.outputPath));
}

} // namespace

void registerExportCommands(CommandRegistry& registry)
{
    registry.add({{QStringLiteral("export.otio"),
                      QStringLiteral("Write a timeline as an OpenTimelineIO (.otio) file."),
                      {kFileParam}, ProjectUse::Read, true},
        runExportOtio});
    registry.add({{QStringLiteral("export.edl"),
                      QStringLiteral("Write a timeline as a CMX 3600 EDL (.edl) file."),
                      {kFileParam}, ProjectUse::Read, true},
        runExportEdl});
    registry.add(
        {{QStringLiteral("import.otio"),
             QStringLiteral("Import an OpenTimelineIO (.otio) file as a new timeline; media that "
                            "exists is added to the pool, missing media is offline."),
             {{QStringLiteral("file"), ParamType::Path, QStringLiteral(".otio file to read"), true,
                 {}, {}}},
             ProjectUse::Modify, false},
            runImportOtio});
    QStringList presets;
    for (const ExportPreset& p : ExportPresets::builtinPresets())
        presets.append(p.id);
    registry.add(
        {{QStringLiteral("render"),
             QStringLiteral("Render a timeline to a file with a Deliver preset (progress on "
                            "stderr). Presets: ")
                 + presets.join(QStringLiteral(", ")) + QStringLiteral("."),
             {{QStringLiteral("output"), ParamType::Path,
                  QStringLiteral("file to write (name_%05d.png for an image sequence)"), true, {},
                  {}},
                 {QStringLiteral("preset"), ParamType::String,
                     QStringLiteral("Deliver preset id (default: chosen by the file extension)"),
                     false, {}, {}}},
             ProjectUse::Read, true, false, true, true},
            runRender});
}

} // namespace kk::commands
