// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: media.import, media.list, media.relink.
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/MediaPool.h"
#include "core/Project.h"
#include "ui/common/TimecodeFormat.h"

#include <QDir>
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

} // namespace

void registerMediaCommands(CommandRegistry& registry)
{
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
