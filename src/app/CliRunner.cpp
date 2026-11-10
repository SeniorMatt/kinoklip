// SPDX-License-Identifier: GPL-3.0-or-later
// CliRunner implementation.
#include "app/CliRunner.h"

#include "app/commands/Command.h"
#include "app/live/LiveClient.h"
#include "app/mcp/McpStdio.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>

namespace kk::cli {

namespace {

using commands::Command;
using commands::CommandRegistry;
using commands::ParamSpec;
using commands::ParamType;

constexpr qint64 kProgressIntervalMs = 1000; ///< progress lines are at most this often

void put(std::FILE* file, const QString& text)
{
    const QByteArray bytes = text.toUtf8();
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), file);
}

QString typeHint(ParamType type)
{
    switch (type) {
    case ParamType::Integer:
        return QStringLiteral("<n>");
    case ParamType::Number:
        return QStringLiteral("<number>");
    case ParamType::Boolean:
        return QString();
    case ParamType::Time:
        return QStringLiteral("<frames|timecode>");
    case ParamType::Path:
        return QStringLiteral("<path>");
    default:
        return QStringLiteral("<text>");
    }
}

QString commandWords(const QString& name)
{
    return QString(name).replace(QLatin1Char('.'), QLatin1Char(' '));
}

QString usage(const Command& command)
{
    QString text = QStringLiteral("Usage: kinoklip %1 [options]\n\n%2\n\nOptions:\n")
                       .arg(commandWords(command.spec.name), command.spec.description);
    for (const ParamSpec& p : CommandRegistry::fullParams(command.spec)) {
        QString left = QStringLiteral("--") + p.name;
        if (!typeHint(p.type).isEmpty())
            left += QLatin1Char(' ') + typeHint(p.type);
        QString right = p.description;
        if (!p.choices.isEmpty())
            right
                += QStringLiteral(" (") + p.choices.join(QStringLiteral(" | ")) + QLatin1Char(')');
        if (!p.defaultText.isEmpty())
            right += QStringLiteral(" [default: ") + p.defaultText + QLatin1Char(']');
        if (p.required)
            right += QStringLiteral(" (required)");
        text += QStringLiteral("  %1  %2\n").arg(left, -28).arg(right);
    }
    text += QStringLiteral("  %1  %2\n")
                .arg(QStringLiteral("--json"), -28)
                .arg(QStringLiteral("print the result as JSON"));
    text += QStringLiteral("  %1  %2\n")
                .arg(QStringLiteral("--live"), -28)
                .arg(QStringLiteral("run it in the open Kinoklip window (Preferences > Local "
                                    "Control), not on a project file"));
    return text;
}

QString overview(const CommandRegistry& registry)
{
    QString text = QStringLiteral("Usage: kinoklip <command> [--option value ...] [--json]\n"
                                  "       kinoklip [project.kkproj]        open the window\n\n"
                                  "Commands (kinoklip help <command> shows the options):\n");
    for (const Command& c : registry.all())
        text += QStringLiteral("  %1  %2\n")
                    .arg(commandWords(c.spec.name), -18)
                    .arg(c.spec.description.section(QLatin1Char('.'), 0, 0));
    text += QStringLiteral("  %1  %2\n")
                .arg(QStringLiteral("commands"), -18)
                .arg(QStringLiteral("list the commands (--json: with every parameter)"));
    text += QStringLiteral("  %1  %2\n")
                .arg(QStringLiteral("mcp"), -18)
                .arg(QStringLiteral("serve the commands to a language model (MCP, stdio)"));
    text += QStringLiteral("  %1  %2\n")
                .arg(QStringLiteral("help"), -18)
                .arg(QStringLiteral("this text, or the options of one command"));
    return text;
}

QString mcpUsage()
{
    return QStringLiteral(
        "Usage: kinoklip mcp [--file]\n\n"
        "Starts a Model Context Protocol server on standard input and output: one JSON message "
        "per line. Every command is a tool (clip add is clip_add). The server is local, sends "
        "nothing anywhere and stops when its input closes. Put it in the configuration of an "
        "MCP client as command \"kinoklip\" with the argument \"mcp\".\n\n"
        "When a Kinoklip window is open with Preferences > Local Control > \"Let local tools "
        "control Kinoklip (MCP)\" switched on, the tools change the project open in that window "
        "(one undo step per call) and `project` can be left out. Otherwise the tools change "
        "project files. --file always works on project files.\n");
}

/// The command named by the first words of @p words; @p consumed tells how many words it used.
const Command* resolve(const CommandRegistry& registry, const QStringList& words, int* consumed)
{
    if (words.isEmpty())
        return nullptr;
    if (words.size() > 1) {
        if (const Command* c = registry.find(words[0] + QLatin1Char('.') + words[1])) {
            *consumed = 2;
            return c;
        }
    }
    *consumed = 1;
    return registry.find(words[0]);
}

bool isUsageError(const QString& code)
{
    return code == QLatin1String("unknown_command") || code == QLatin1String("unknown_parameter")
        || code == QLatin1String("bad_value") || code == QLatin1String("missing_parameter");
}

} // namespace

bool isCommand(const QStringList& arguments)
{
    if (arguments.isEmpty())
        return false;
    const QString first = arguments.first();
    if (first.startsWith(QLatin1Char('-')) || QFileInfo::exists(first))
        return false;
    if (first == QLatin1String("commands") || first == QLatin1String("help")
        || first == QLatin1String("mcp"))
        return true;
    return CommandRegistry::builtin().groups().contains(first);
}

int run(const QStringList& arguments, std::FILE* out, std::FILE* err)
{
    const CommandRegistry& registry = CommandRegistry::builtin();
    const QString first = arguments.value(0);

    if (first == QLatin1String("commands")) {
        if (arguments.mid(1) == QStringList {QStringLiteral("--json")}) {
            put(out,
                QString::fromUtf8(
                    QJsonDocument(registry.describeAll()).toJson(QJsonDocument::Compact))
                    + QLatin1Char('\n'));
            return 0;
        }
        put(out, overview(registry));
        return 0;
    }
    if (first == QLatin1String("mcp")) {
        if (arguments.mid(1) == QStringList {QStringLiteral("--help")}) {
            put(out, mcpUsage());
            return 0;
        }
        return mcp::runStdioServer(arguments.mid(1), stdin, out, err);
    }
    if (first == QLatin1String("help")) {
        QStringList words = arguments.mid(1);
        if (words.isEmpty()) {
            put(out, overview(registry));
            return 0;
        }
        int consumed = 0;
        const Command* command = resolve(registry, words, &consumed);
        if (!command) {
            put(err,
                QStringLiteral("kinoklip: unknown command \"%1\"\n")
                    .arg(words.join(QLatin1Char(' '))));
            return 2;
        }
        put(out, usage(*command));
        return 0;
    }

    int consumed = 0;
    const Command* command = resolve(registry, arguments, &consumed);
    if (!command) {
        put(err,
            QStringLiteral("kinoklip: unknown command \"%1\" (kinoklip help lists the commands)\n")
                .arg(arguments.mid(0, 2).join(QLatin1Char(' '))));
        return 2;
    }

    // Options: --name value, --name=value, a bare --flag for a boolean, --json.
    const QVector<ParamSpec> params = CommandRegistry::fullParams(command->spec);
    QJsonObject call;
    bool json = false;
    bool live = false;
    const QStringList rest = arguments.mid(consumed);
    for (qsizetype i = 0; i < rest.size(); ++i) {
        QString token = rest[i];
        if (token == QLatin1String("--help") || token == QLatin1String("-h")) {
            put(out, usage(*command));
            return 0;
        }
        if (token == QLatin1String("--json")) {
            json = true;
            continue;
        }
        if (token == QLatin1String("--live")) {
            live = true;
            continue;
        }
        if (!token.startsWith(QLatin1String("--"))) {
            put(err,
                QStringLiteral("kinoklip: unexpected argument \"%1\" (options start with --)\n")
                    .arg(token));
            return 2;
        }
        token = token.mid(2);
        QString value;
        bool hasValue = false;
        if (const qsizetype eq = token.indexOf(QLatin1Char('=')); eq >= 0) {
            value = token.mid(eq + 1);
            token = token.left(eq);
            hasValue = true;
        }
        const auto spec = std::find_if(
            params.begin(), params.end(), [&](const ParamSpec& p) { return p.name == token; });
        if (spec == params.end()) {
            put(err,
                QStringLiteral("kinoklip: unknown option --%1 (kinoklip help %2)\n")
                    .arg(token, commandWords(command->spec.name)));
            return 2;
        }
        if (!hasValue && spec->type == ParamType::Boolean) {
            // A boolean is a bare flag; "true" / "false" after it is its value.
            const QString next = i + 1 < rest.size() ? rest[i + 1] : QString();
            if (next == QLatin1String("true") || next == QLatin1String("false")) {
                value = next;
                ++i;
            }
            hasValue = true;
        }
        if (!hasValue) {
            if (i + 1 >= rest.size()) {
                put(err, QStringLiteral("kinoklip: option --%1 needs a value\n").arg(token));
                return 2;
            }
            value = rest[++i]; // a value may start with '-' (a negative time)
        }
        if (spec->type == ParamType::StringList) {
            QJsonArray list = call.value(token).toArray();
            list.append(value);
            call.insert(token, list);
        } else {
            call.insert(token, value);
        }
    }

    QElapsedTimer sinceProgress;
    sinceProgress.start();
    bool printedProgress = false;
    const commands::ProgressFn progress = [&](double fraction, const QString& message) {
        if (printedProgress && fraction < 1.0 && sinceProgress.elapsed() < kProgressIntervalMs)
            return;
        sinceProgress.restart();
        printedProgress = true;
        put(err,
            QStringLiteral("%1: %2% %3\n")
                .arg(commandWords(command->spec.name))
                .arg(int(fraction * 100))
                .arg(message));
        std::fflush(err);
    };
    commands::CommandResult result;
    if (live) {
        // Forward to the open window (it must have local control switched on).
        live::LiveClient client;
        QString why;
        if (!client.connectToWindow(&why)) {
            put(err,
                QStringLiteral("kinoklip: %1 (switch on Preferences > Local Control in the "
                               "window, or leave out --live)\n")
                    .arg(why));
            return 1;
        }
        result = client.call(command->spec.name, call, progress);
    } else {
        result = registry.execute(command->spec.name, call, progress);
    }
    if (json)
        put(out,
            QString::fromUtf8(QJsonDocument(result.toJson()).toJson(QJsonDocument::Compact))
                + QLatin1Char('\n'));
    if (result.ok) {
        if (!json)
            put(out, result.text + QLatin1Char('\n'));
        return 0;
    }
    put(err, QStringLiteral("kinoklip: %1\n").arg(result.error));
    return isUsageError(result.errorCode) ? 2 : 1;
}

} // namespace kk::cli
