// SPDX-License-Identifier: GPL-3.0-or-later
// Command: log.export (spec section 64). Writes the session log of this process; see
// app/LogExport.h. Not remote-allowed: it writes a file chosen by the caller and the log can
// contain folder paths (the same rule as settings.import).
#include "app/LogExport.h"
#include "app/commands/Command.h"
#include "app/commands/CommandSupport.h"

#include <QDir>

namespace kk::commands {

namespace {

CommandResult runLogExport(CommandContext&, const CommandArgs& args)
{
    const QString file = args.string(QStringLiteral("file"));
    QString error;
    if (!logexport::exportToFile(file, &error))
        return fail("write_failed", error);
    return CommandResult::success({{QStringLiteral("file"), file}},
        QStringLiteral("wrote the session log to %1").arg(QDir::toNativeSeparators(file)));
}

} // namespace

void registerLogCommands(CommandRegistry& registry)
{
    registry.add({{QStringLiteral("log.export"),
                      QStringLiteral("Write the log of this Kinoklip session (system facts, the "
                                     "decoder for each media file, playback statistics, warnings "
                                     "and errors) to a text file for a bug report. Paths under "
                                     "the home folder are written as ~/..., the user name and "
                                     "computer name are removed. In the window the same file is "
                                     "Help > Export Log File."),
                      {{QStringLiteral("file"), ParamType::Path,
                          QStringLiteral("the text file to write"), true, {}, {}}},
                      ProjectUse::None, false, false, false, false, false},
        runLogExport});
}

} // namespace kk::commands
