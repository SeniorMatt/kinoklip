// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: version, project.new, project.info.
#include "app/AppInfo.h"
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"
#include "core/Project.h"
#include "core/Timeline.h"
#include "pack/ProjectArchive.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace kk::commands {

namespace {

constexpr int kMaxSize = 16384; ///< largest picture side accepted for a new project
constexpr double kMaxFps = 1000.0;

CommandResult runVersion(CommandContext&, const CommandArgs&)
{
    const QString version = appinfo::displayVersion();
    return CommandResult::success({{QStringLiteral("name"), QString::fromLatin1(appinfo::name)},
                                      {QStringLiteral("version"), version},
                                      {QStringLiteral("qt"), QString::fromLatin1(qVersion())}},
        QStringLiteral("%1 %2").arg(QString::fromLatin1(appinfo::name), version));
}

CommandResult runProjectNew(CommandContext& ctx, const CommandArgs& args)
{
    Project& project = *ctx.project;
    ProjectSettings settings = project.settings();
    const QString name = args.string(QStringLiteral("name"));
    settings.name = name.isEmpty() ? QFileInfo(ctx.projectPath).completeBaseName() : name;
    settings.width = int(args.integer(QStringLiteral("width"), settings.width));
    settings.height = int(args.integer(QStringLiteral("height"), settings.height));
    if (settings.width < 2 || settings.width > kMaxSize || settings.height < 2
        || settings.height > kMaxSize)
        return fail(
            "bad_value", QStringLiteral("width and height must be between 2 and %1").arg(kMaxSize));
    if (args.has(QStringLiteral("fps"))) {
        const double fps = args.number(QStringLiteral("fps"));
        if (!(fps >= 1.0 && fps <= kMaxFps))
            return fail("bad_value", QStringLiteral("fps must be between 1 and 1000"));
        settings.fps = rationalFromFps(fps);
        settings.timelineStart = oneHourTimecode(settings.fps);
    }
    project.setSettings(settings);
    if (args.has(QStringLiteral("fps")))
        project.setTimelineStartTimecode(project.activeTimelineUid(), settings.timelineStart);
    return CommandResult::success(
        {{QStringLiteral("name"), settings.name}, {QStringLiteral("width"), settings.width},
            {QStringLiteral("height"), settings.height},
            {QStringLiteral("fps"), settings.fps.toDouble()}},
        QStringLiteral("created %1 (%2x%3, %4 fps)")
            .arg(ctx.projectPath)
            .arg(settings.width)
            .arg(settings.height)
            .arg(settings.fps.toDouble(), 0, 'g', 6));
}

CommandResult runProjectInfo(CommandContext& ctx, const CommandArgs&)
{
    const Project& project = *ctx.project;
    const ProjectSettings settings = project.settings();
    QJsonArray timelines;
    for (quint64 uid : project.timelineUids()) {
        if (const auto data = project.timelineData(uid))
            timelines.append(timelineToJson(*data, uid == project.activeTimelineUid()));
    }
    int files = 0;
    for (const MediaItem& item : project.mediaPool()->items()) {
        if (item.kind == MediaItemKind::File)
            ++files;
    }
    QJsonObject data {{QStringLiteral("name"), settings.name},
        {QStringLiteral("path"), ctx.projectPath}, {QStringLiteral("width"), settings.width},
        {QStringLiteral("height"), settings.height},
        {QStringLiteral("fps"), settings.fps.toDouble()},
        {QStringLiteral("activeTimeline"), qint64(project.activeTimelineUid())},
        {QStringLiteral("timelines"), timelines}, {QStringLiteral("mediaCount"), files},
        {QStringLiteral("offlineMedia"), project.mediaPool()->offlineCount()}};
    QString text = QStringLiteral("%1: %2x%3 at %4 fps, %5 timeline(s), %6 media file(s)")
                       .arg(settings.name)
                       .arg(settings.width)
                       .arg(settings.height)
                       .arg(settings.fps.toDouble(), 0, 'g', 6)
                       .arg(timelines.size())
                       .arg(files);
    for (const QJsonValue& t : std::as_const(timelines)) {
        const QJsonObject o = t.toObject();
        text += QStringLiteral("\n  %1%2 %3: %4 clips, %5 V + %6 A tracks, %7")
                    .arg(o[QStringLiteral("active")].toBool() ? QStringLiteral("*")
                                                              : QStringLiteral(" "))
                    .arg(o[QStringLiteral("id")].toInteger())
                    .arg(o[QStringLiteral("name")].toString())
                    .arg(o[QStringLiteral("clips")].toInt())
                    .arg(o[QStringLiteral("videoTracks")].toInt())
                    .arg(o[QStringLiteral("audioTracks")].toInt())
                    .arg(o[QStringLiteral("durationTimecode")].toString());
    }
    return CommandResult::success(data, text);
}

CommandResult runProjectSave(CommandContext& ctx, const CommandArgs&)
{
    // File mode: the registry saves the project after the command (to `output` when given), so
    // there is nothing to do here; this only makes `project save --output copy.kkproj` a copy.
    if (!ctx.live)
        return CommandResult::success({}, QStringLiteral("saved"));
    if (!ctx.saveProject)
        return fail("live_unsupported", QStringLiteral("this window cannot save from here"));
    if (ctx.project->filePath().isEmpty())
        return fail("no_file",
            QStringLiteral("the open project has not been saved yet: save it once with File > "
                           "Save Project, then this works"));
    QString error;
    if (!ctx.saveProject(&error))
        return fail(
            "save_failed", error.isEmpty() ? QStringLiteral("the project was not saved") : error);
    return CommandResult::success({{QStringLiteral("project"), ctx.project->filePath()}},
        QStringLiteral("saved %1").arg(ctx.project->filePath()));
}

CommandResult runSessionInfo(CommandContext& ctx, const CommandArgs&)
{
    if (!ctx.live || !ctx.project)
        return CommandResult::success({{QStringLiteral("mode"), QStringLiteral("file")},
                                          {QStringLiteral("project"), QJsonValue::Null}},
            QStringLiteral("file mode: every command reads the project file given as `project`, "
                           "changes it and saves it; no window is involved. To control an open "
                           "Kinoklip window, switch on \"Let local tools control Kinoklip (MCP)\" "
                           "in its Preferences."));
    const Project& project = *ctx.project;
    const QString path = project.filePath();
    const QJsonValue pathValue = path.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(path);
    return CommandResult::success(
        {{QStringLiteral("mode"), QStringLiteral("live")}, {QStringLiteral("project"), pathValue},
            {QStringLiteral("name"), project.name()},
            {QStringLiteral("unsaved"), project.isDirty()},
            {QStringLiteral("activeTimeline"), qint64(project.activeTimelineUid())}},
        QStringLiteral("live mode: the open project \"%1\"%2. Edits appear in the window, one "
                       "undo step per call; project.save saves it.")
            .arg(project.name(),
                path.isEmpty() ? QStringLiteral(" (not saved yet)")
                               : QStringLiteral(" (") + path + QLatin1Char(')')));
}

CommandResult runProjectArchive(CommandContext& ctx, const CommandArgs& args)
{
    const QString folder = QFileInfo(args.string(QStringLiteral("folder"))).absoluteFilePath();
    pack::ArchiveOptions options;
    options.proxyMedia = args.boolean(QStringLiteral("proxies"));
    options.optimizedMedia = args.boolean(QStringLiteral("optimized"));
    const QString file = ctx.project->filePath().isEmpty() ? ctx.projectPath : ctx.project->filePath();
    const QString baseDir = file.isEmpty() ? QDir::currentPath() : QFileInfo(file).absolutePath();
    const pack::ArchiveResult result = pack::exportArchive(
        ctx.project->documentJson(file.isEmpty() ? baseDir + QStringLiteral("/untitled") : file),
        baseDir, ctx.project->settings().name, folder, options);
    if (result.status != pack::Status::Ok)
        return fail("archive_failed", result.error);
    return CommandResult::success(
        {{QStringLiteral("folder"), folder}, {QStringLiteral("project"), result.projectFile},
            {QStringLiteral("media"), result.mediaFiles}, {QStringLiteral("luts"), result.lutFiles},
            {QStringLiteral("proxies"), result.proxyFiles},
            {QStringLiteral("skipped"), QJsonArray::fromStringList(result.skipped)}},
        QStringLiteral("wrote the project archive %1 (%2 media file(s), %3 skipped)")
            .arg(folder)
            .arg(result.mediaFiles)
            .arg(result.skipped.size()));
}

CommandResult runProjectRestoreArchive(CommandContext&, const CommandArgs& args)
{
    const QString folder = QFileInfo(args.string(QStringLiteral("folder"))).absoluteFilePath();
    if (!pack::isArchiveFolder(folder))
        return fail("not_an_archive", QStringLiteral("%1 is not a project archive").arg(folder));
    const QString into = QFileInfo(args.string(QStringLiteral("into"))).absoluteFilePath();
    const pack::RestoreResult result = pack::restoreArchive(folder, into);
    if (result.status != pack::Status::Ok)
        return fail("restore_failed", result.error);
    return CommandResult::success(
        {{QStringLiteral("project"), result.projectFile}, {QStringLiteral("files"), result.files}},
        QStringLiteral("restored the archive to %1").arg(result.projectFile));
}

} // namespace

void registerProjectCommands(CommandRegistry& registry)
{
    registry.add({{QStringLiteral("project.archive"),
                      QStringLiteral("Export Project Archive: copy the project and every media "
                                     "file it uses into a new archive folder (.dra)."),
                      {{QStringLiteral("folder"), ParamType::Path,
                           QStringLiteral("archive folder to create (must not exist)"), true, {}, {}},
                          {QStringLiteral("proxies"), ParamType::Boolean,
                              QStringLiteral("also copy the linked proxy media"), false,
                              QStringLiteral("false"), {}},
                          {QStringLiteral("optimized"), ParamType::Boolean,
                              QStringLiteral("also copy the linked optimized media"), false,
                              QStringLiteral("false"), {}}},
                      ProjectUse::Read, false},
        runProjectArchive});
    registry.add({{QStringLiteral("project.restore-archive"),
                      QStringLiteral("Restore Project Archive: copy an archive folder to a new "
                                     "folder; the project inside it uses the copies."),
                      {{QStringLiteral("folder"), ParamType::Path,
                           QStringLiteral("the archive folder (.dra)"), true, {}, {}},
                          {QStringLiteral("into"), ParamType::Path,
                              QStringLiteral("new folder to restore into (must not exist)"), true,
                              {}, {}}},
                      ProjectUse::None, false},
        runProjectRestoreArchive});
    registry.add({{QStringLiteral("version"), QStringLiteral("Print the Kinoklip version."), {},
                      ProjectUse::None, false},
        runVersion});
    registry.add(
        {{QStringLiteral("project.new"),
             QStringLiteral("Create a new, empty project file (V1/A1 timeline)."),
             {{QStringLiteral("name"), ParamType::String,
                  QStringLiteral("project name (default: the file name)"), false, {}, {}},
                 {QStringLiteral("width"), ParamType::Integer,
                     QStringLiteral("picture width in pixels (default: your user default config, else 1920)"),
                     false, QStringLiteral("1920"), {}},
                 {QStringLiteral("height"), ParamType::Integer,
                     QStringLiteral("picture height in pixels (default: your user default config, else 1080)"),
                     false, QStringLiteral("1080"), {}},
                 {QStringLiteral("fps"), ParamType::Number,
                     QStringLiteral("frame rate, e.g. 24, 25, 29.97, 59.94 (default: your user default "
                                    "config, else 24)"),
                     false,
                     QStringLiteral("24"), {}}},
             ProjectUse::Create, false},
            runProjectNew});
    registry.add({{QStringLiteral("session.info"),
                      QStringLiteral("Tell which mode the commands run in: `file` (they change "
                                     "project files) or `live` (they change the project open in "
                                     "the Kinoklip window), and which project is open."),
                      {}, ProjectUse::None, false},
        runSessionInfo});
    registry.add({{QStringLiteral("project.save"),
                      QStringLiteral("Save the project: in live mode the open project, like File "
                                     "> Save Project (it needs a file already); in file mode a "
                                     "copy to `output`."),
                      {}, ProjectUse::Modify, false, false, false},
        runProjectSave});
    registry.add({{QStringLiteral("project.info"),
                      QStringLiteral("Show the project: settings, timelines, media count."), {},
                      ProjectUse::Read, false},
        runProjectInfo});
}

} // namespace kk::commands
