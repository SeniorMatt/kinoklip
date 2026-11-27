// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: media.import, media.list, media.relink.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/MediaMetadataCsv.h"
#include "core/MediaPool.h"
#include "export/EdlExport.h"
#include "media/CloneTool.h"
#include "core/Project.h"
#include "ui/common/TimecodeFormat.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace kk::commands {

namespace {

QJsonObject mediaToJson(const Project& project, const MediaItem& item)
{
    const Rational fps = project.settings().fps;
    const media::MediaInfo& info = item.info;
    static const char* const kinds[] = {"file", "timeline", "compound", "multicam", "subtitle"};
    QJsonObject o {{QStringLiteral("id"), item.id}, {QStringLiteral("name"), item.name},
        {QStringLiteral("kind"), QString::fromLatin1(kinds[int(item.kind)])},
        {QStringLiteral("path"), item.path},
        {QStringLiteral("offline"), project.mediaPool()->isOffline(item.id)},
        {QStringLiteral("hasVideo"), info.hasVideo}, {QStringLiteral("hasAudio"), info.hasAudio},
        {QStringLiteral("durationUs"), qint64(info.durationUs)},
        {QStringLiteral("duration"), timecode::format(usToFrames(info.durationUs, fps), fps)}};
    if (info.hasAudio)
        o.insert(QStringLiteral("audioTracks"), info.audioTrackCount());
    if (info.hasVideo) {
        o.insert(QStringLiteral("width"), info.displayWidth);
        o.insert(QStringLiteral("height"), info.displayHeight);
        o.insert(QStringLiteral("fps"), info.frameRate.toDouble());
    }
    return o;
}

CommandResult runImport(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    QStringList paths;
    for (const QString& file : args.strings(QStringLiteral("file")))
        paths.append(QFileInfo(file).absoluteFilePath());
    const ImportResult result = project.importMedia(paths);
    if (result.importedIds.isEmpty())
        return fail("import_failed",
            result.warnings.isEmpty() ? QStringLiteral("nothing was imported")
                                      : result.warnings.join(QStringLiteral("; ")));
    QJsonArray imported;
    for (const QString& id : result.importedIds) {
        if (const MediaItem* item = project.mediaPool()->item(id))
            imported.append(mediaToJson(project, *item));
    }
    return CommandResult::success(
        {{QStringLiteral("imported"), imported},
            {QStringLiteral("warnings"), QJsonArray::fromStringList(result.warnings)}},
        QStringLiteral("imported %1 file(s)%2")
            .arg(result.importedIds.size())
            .arg(result.warnings.isEmpty()
                    ? QString()
                    : QStringLiteral("; skipped: ") + result.warnings.join(QStringLiteral("; "))));
}

CommandResult runList(CommandContext& ctx, const CommandArgs&)
{
    const Project& project = *ctx.project;
    QJsonArray items;
    QString text;
    for (const MediaItem& item : project.mediaPool()->items()) {
        const QJsonObject o = mediaToJson(project, item);
        items.append(o);
        text += QStringLiteral("%1  %2  %3%4\n")
                    .arg(item.id, item.name, o[QStringLiteral("duration")].toString(),
                        o[QStringLiteral("offline")].toBool() ? QStringLiteral("  OFFLINE")
                                                              : QString());
    }
    return CommandResult::success({{QStringLiteral("media"), items}},
        text.isEmpty() ? QStringLiteral("no media") : text.trimmed());
}

CommandResult runRelink(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    MediaPool* pool = project.mediaPool();
    const bool single = args.has(QStringLiteral("id"));
    if (single == args.has(QStringLiteral("folder")))
        return fail("bad_value", QStringLiteral("give either --folder, or --id with --path"));
    int relinked = 0;
    if (single) {
        const QString id = args.string(QStringLiteral("id"));
        const QString path = args.string(QStringLiteral("path"));
        const MediaItem* item = pool->item(id);
        if (!item || item->kind != MediaItemKind::File)
            return fail("unknown_media", QStringLiteral("no media file with id \"%1\"").arg(id));
        if (path.isEmpty() || !QFileInfo(path).isFile())
            return fail("file_not_found", QStringLiteral("%1 is not a file").arg(path));
        project.beginEdit(QStringLiteral("Relink Media"));
        relinked = pool->relinkItem(id, QFileInfo(path).absoluteFilePath()) ? 1 : 0;
        project.endEdit();
    } else {
        const QString folder = args.string(QStringLiteral("folder"));
        if (!QFileInfo(folder).isDir())
            return fail("file_not_found", QStringLiteral("%1 is not a folder").arg(folder));
        project.beginEdit(QStringLiteral("Relink Media"));
        relinked = pool->relinkFromFolder(QDir(folder).absolutePath());
        project.endEdit();
    }
    pool->recheckOffline();
    return CommandResult::success(
        {{QStringLiteral("relinked"), relinked}, {QStringLiteral("offline"), pool->offlineCount()}},
        QStringLiteral("relinked %1 item(s), %2 still offline")
            .arg(relinked)
            .arg(pool->offlineCount()));
}

CommandResult runExportMetadata(CommandContext& ctx, const CommandArgs& args)
{
    const QString path = QFileInfo(args.string(QStringLiteral("file"))).absoluteFilePath();
    const QString text = mediametadata::toCsv(*ctx.project->mediaPool());
    QString error;
    if (!edl::writeTextFile(path, text, &error))
        return fail("write_failed", error);
    return CommandResult::success({{QStringLiteral("file"), path}},
        QStringLiteral("wrote the metadata of the media pool to %1").arg(path));
}

CommandResult runImportMetadata(CommandContext& ctx, const CommandArgs& args)
{
    const QString path = QFileInfo(args.string(QStringLiteral("file"))).absoluteFilePath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail("file_not_found", QStringLiteral("cannot read %1").arg(path));
    Project& project = *ctx.project;
    project.beginEdit(QStringLiteral("Import Metadata"));
    const mediametadata::ImportReport report
        = mediametadata::applyCsv(*project.mediaPool(), QString::fromUtf8(file.readAll()));
    if (!report.ok()) {
        project.cancelEdit();
        return fail("bad_csv", report.error);
    }
    project.endEdit();
    return CommandResult::success(
        {{QStringLiteral("rows"), report.rows}, {QStringLiteral("updated"), report.updated},
            {QStringLiteral("unchanged"), report.unchanged},
            {QStringLiteral("unmatched"), QJsonArray::fromStringList(report.unmatched)},
            {QStringLiteral("ambiguous"), QJsonArray::fromStringList(report.ambiguous)}},
        QStringLiteral("updated %1 clip(s), %2 row(s) matched no clip")
            .arg(report.updated)
            .arg(report.unmatched.size()));
}

CommandResult runClone(CommandContext&, const CommandArgs& args)
{
    clone::Job job;
    for (const QString& s : args.strings(QStringLiteral("source")))
        job.sources << QFileInfo(s).absoluteFilePath();
    for (const QString& d : args.strings(QStringLiteral("destination")))
        job.destinations << QFileInfo(d).absoluteFilePath();
    if (args.has(QStringLiteral("checksum"))
        && !clone::parseChecksum(args.string(QStringLiteral("checksum")), job.checksum))
        return fail("bad_checksum", QStringLiteral("checksum must be md5, sha1, sha256 or crc32"));
    job.verify = !args.boolean(QStringLiteral("no-verify"));
    const clone::Result result = clone::run(job);
    if (!result.error.isEmpty())
        return fail("clone_failed", result.error);
    QJsonArray files;
    for (const clone::FileResult& f : result.results)
        files.append(QJsonObject {{QStringLiteral("file"), f.relative},
            {QStringLiteral("size"), f.size}, {QStringLiteral("checksum"), f.checksum}});
    const QJsonObject data {{QStringLiteral("files"), result.files},
        {QStringLiteral("bytes"), result.bytes}, {QStringLiteral("verified"), result.verified},
        {QStringLiteral("failures"), QJsonArray::fromStringList(result.failures)},
        {QStringLiteral("checksum"), clone::checksumName(job.checksum)},
        {QStringLiteral("list"), files}};
    if (!result.ok)
        return fail("clone_failed",
            QStringLiteral("%1 problem(s): %2")
                .arg(result.failures.size())
                .arg(result.failures.join(QStringLiteral("; "))));
    return CommandResult::success(data,
        QStringLiteral("cloned %1 file(s) to %2 destination(s), %3 copies verified")
            .arg(result.files)
            .arg(job.destinations.size())
            .arg(result.verified));
}

} // namespace

void registerMediaCommands(CommandRegistry& registry)
{
    registry.add({{QStringLiteral("media.clone"),
                      QStringLiteral("Clone Tool: copy folders or files (a camera card) to one or "
                                     "more destinations and verify every copy with a checksum; "
                                     "writes <name>.md5 (or .sha1 ...) next to each copy."),
                      {{QStringLiteral("source"), ParamType::StringList,
                           QStringLiteral("folder or file to copy (repeat the option for more)"),
                           true, {}, {}},
                          {QStringLiteral("destination"), ParamType::StringList,
                              QStringLiteral("folder to copy into (repeat for more copies)"), true,
                              {}, {}},
                          {QStringLiteral("checksum"), ParamType::String,
                              QStringLiteral("checksum type"), false, QStringLiteral("md5"),
                              {QStringLiteral("md5"), QStringLiteral("sha1"),
                                  QStringLiteral("sha256"), QStringLiteral("crc32")}},
                          {QStringLiteral("no-verify"), ParamType::Boolean,
                              QStringLiteral("skip reading the copies back"), false,
                              QStringLiteral("false"), {}}},
                      ProjectUse::None, false},
        runClone});
    registry.add({{QStringLiteral("media.export-metadata"),
                      QStringLiteral("Write the clip metadata of the media pool to a CSV file."),
                      {{QStringLiteral("file"), ParamType::Path, QStringLiteral("CSV file to write"),
                          true, {}, {}}},
                      ProjectUse::Read, false},
        runExportMetadata});
    registry.add({{QStringLiteral("media.import-metadata"),
                      QStringLiteral("Set clip metadata of the media pool from a CSV file (clips "
                                     "are found by File Name and Clip Directory; empty cells keep "
                                     "the value)."),
                      {{QStringLiteral("file"), ParamType::Path, QStringLiteral("CSV file to read"),
                          true, {}, {}}},
                      ProjectUse::Modify, false},
        runImportMetadata});
    registry.add(
        {{QStringLiteral("media.import"),
             QStringLiteral("Import media files into the project's media pool."),
             {{QStringLiteral("file"), ParamType::StringList,
                 QStringLiteral("file to import (repeat the option for more)"), true, {}, {}}},
             ProjectUse::Modify, false},
            runImport});
    registry.add(
        {{QStringLiteral("media.list"),
             QStringLiteral("List the media pool: id, name, path, duration, offline state."), {},
             ProjectUse::Read, false},
            runList});
    registry.add({{QStringLiteral("media.relink"),
                      QStringLiteral("Relink offline media: search a folder by file name, or point "
                                     "one item at a file."),
                      {{QStringLiteral("folder"), ParamType::Path,
                           QStringLiteral("folder to search (with its subfolders)"), false, {}, {}},
                          {QStringLiteral("id"), ParamType::String,
                              QStringLiteral("media id to relink to --path"), false, {}, {}},
                          {QStringLiteral("path"), ParamType::Path,
                              QStringLiteral("the file for --id"), false, {}, {}}},
                      ProjectUse::Modify, false},
        runRelink});
}

} // namespace kk::commands
