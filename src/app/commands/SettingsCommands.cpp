// SPDX-License-Identifier: GPL-3.0-or-later
// Commands: settings.export, settings.import (spec section 61). They work on the application's
// own settings, not on a project; see app/SettingsTransfer.h for what a settings file holds.
#include "app/SettingsTransfer.h"
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"

#include <QDir>
#include <QJsonArray>

namespace kk::commands {

namespace {

namespace st = kk::settingstransfer;

CommandResult runSettingsExport(CommandContext&, const CommandArgs& args)
{
    const QString file = args.string(QStringLiteral("file"));
    const bool recent = args.boolean(QStringLiteral("recent-projects"), false);
    QString error;
    if (!st::exportToFile(file, st::ExportOptions {recent}, &error))
        return fail("write_failed", error);
    return CommandResult::success({{QStringLiteral("file"), file}, {QStringLiteral("recentProjects"), recent}},
        QStringLiteral("wrote the settings to %1").arg(QDir::toNativeSeparators(file)));
}

CommandResult runSettingsImport(CommandContext&, const CommandArgs& args)
{
    const QString file = args.string(QStringLiteral("file"));
    st::ApplyOptions options;
    options.includeRecentProjects = args.boolean(QStringLiteral("recent-projects"), false);
    st::Parsed parsed;
    QString error;
    if (!st::parseFile(file, &parsed, &error))
        return fail("bad_settings_file", error);
    const st::ApplyResult applied = st::apply(parsed, options);
    if (!applied.ok)
        return fail("import_failed", applied.error);
    QJsonArray notes;
    for (const QString& n : applied.notes)
        notes.append(n);
    return CommandResult::success(
        {{QStringLiteral("settings"), applied.settingsApplied},
            {QStringLiteral("files"), applied.filesApplied},
            {QStringLiteral("backup"), applied.backupPath},
            {QStringLiteral("restartNeeded"), applied.restartNeeded}, {QStringLiteral("notes"), notes}},
        QStringLiteral("imported %1 settings and %2 files; backup of the old settings: %3%4")
            .arg(applied.settingsApplied)
            .arg(applied.filesApplied)
            .arg(QDir::toNativeSeparators(applied.backupPath),
                applied.restartNeeded ? QStringLiteral("; restart Kinoklip to apply") : QString()));
}

} // namespace

void registerSettingsCommands(CommandRegistry& registry)
{
    registry.add({{QStringLiteral("settings.export"),
                      QStringLiteral("Write all of Kinoklip's settings (preferences, keyboard "
                                     "profile and shortcuts, layout, view options, user presets) "
                                     "to one .kinoklip-settings file. Not included: projects, "
                                     "media, caches, folders of this computer."),
                      {{QStringLiteral("file"), ParamType::Path,
                           QStringLiteral("the settings file to write"), true, {}, {}},
                          {QStringLiteral("recent-projects"), ParamType::Boolean,
                              QStringLiteral("also write the list of recent projects"), false,
                              QStringLiteral("false"), {}}},
                      ProjectUse::None, false, false, false},
        runSettingsExport});
    registry.add({{QStringLiteral("settings.import"),
                      QStringLiteral("Read a .kinoklip-settings file and replace the current "
                                     "settings with its values. A backup of the old settings is "
                                     "written first. Local Control is never switched on by a "
                                     "file. Restart Kinoklip to apply."),
                      {{QStringLiteral("file"), ParamType::Path,
                           QStringLiteral("the settings file to read"), true, {}, {}},
                          {QStringLiteral("recent-projects"), ParamType::Boolean,
                              QStringLiteral("also take over the list of recent projects"), false,
                              QStringLiteral("false"), {}}},
                      ProjectUse::None, false, false, false, false, false},
        runSettingsImport});
}

} // namespace kk::commands
