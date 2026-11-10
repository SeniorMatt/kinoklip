// SPDX-License-Identifier: GPL-3.0-or-later
// McpStdio implementation.
#include "app/mcp/McpStdio.h"

#include "app/AppInfo.h"
#include "app/live/LiveClient.h"

#include <QByteArray>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace kk::mcp {

namespace {

void writeLine(std::FILE* out, const QByteArray& message)
{
    std::fwrite(message.constData(), 1, size_t(message.size()), out);
    std::fputc('\n', out);
    std::fflush(out);
}

/// Next line of @p in without the line ending; false at end of input. A line is as long as it
/// is (a message may carry a long path list).
bool readLine(std::FILE* in, QByteArray& line)
{
    line.clear();
    char buffer[4096];
    bool any = false;
    while (std::fgets(buffer, sizeof buffer, in)) {
        any = true;
        line.append(buffer);
        if (line.endsWith('\n'))
            break;
    }
    while (line.endsWith('\n') || line.endsWith('\r'))
        line.chop(1);
    return any;
}

/// Where a tool call runs: in the open Kinoklip window when one accepts local control (checked at
/// every call, so a window opened or closed meanwhile is noticed), else on project files.
class Router
{
public:
    Router(const commands::CommandRegistry& registry, bool forceFile, std::FILE* err)
        : m_registry(registry)
        , m_forceFile(forceFile)
        , m_err(err)
    { }

    /// True if the window accepts calls now (connects when needed). Never in file mode.
    bool live()
    {
        if (m_forceFile)
            return false;
        if (m_client.isConnected())
            return true;
        QString why;
        const bool ok = m_client.connectToWindow(&why);
        if (ok != m_wasLive) {
            m_wasLive = ok;
            if (ok)
                std::fprintf(m_err, "kinoklip mcp: live mode, project %s\n",
                    qPrintable(m_client.projectPath().isEmpty() ? m_client.projectName()
                                                                : m_client.projectPath()));
            else
                std::fprintf(m_err, "kinoklip mcp: file mode (%s)\n", qPrintable(why));
            std::fflush(m_err);
        }
        return ok;
    }

    commands::CommandResult run(
        const QString& command, const QJsonObject& arguments, const commands::ProgressFn& progress)
    {
        if (live())
            return m_client.call(command, arguments, progress);
        return m_registry.execute(command, arguments, progress);
    }

private:
    const commands::CommandRegistry& m_registry;
    bool m_forceFile;
    std::FILE* m_err;
    live::LiveClient m_client;
    bool m_wasLive = false;
};

} // namespace

int serve(McpServer& server, std::FILE* in, std::FILE* out)
{
    server.setNotifier([out](const QByteArray& message) { writeLine(out, message); });
    QByteArray line;
    while (readLine(in, line)) {
        if (line.trimmed().isEmpty())
            continue;
        const QByteArray reply = server.handle(line);
        if (!reply.isEmpty())
            writeLine(out, reply);
    }
    return 0;
}

int runStdioServer(const QStringList& options, std::FILE* in, std::FILE* out, std::FILE* err)
{
    bool forceFile = false;
    for (const QString& option : options) {
        if (option == QLatin1String("--file")) {
            forceFile = true;
            continue;
        }
        std::fprintf(err, "kinoklip mcp: unknown option %s (usage: kinoklip mcp [--file])\n",
            qPrintable(option));
        return 2;
    }
    std::FILE* protocol = out;
#ifdef Q_OS_UNIX
    if (out == stdout) {
        // Keep a private copy of the real stdout for the protocol and point descriptor 1 at
        // stderr: a stray printf in a library can then never corrupt the stream.
        const int copy = ::dup(STDOUT_FILENO);
        if (copy >= 0 && ::dup2(STDERR_FILENO, STDOUT_FILENO) >= 0)
            protocol = ::fdopen(copy, "w");
        if (!protocol)
            protocol = out;
    }
#endif
    std::fprintf(err, "kinoklip mcp: %s ready on stdin/stdout\n", appinfo::name);
    std::fflush(err);
    Router router(commands::CommandRegistry::builtin(), forceFile, err);
    McpServer server(commands::CommandRegistry::builtin(),
        [&router](const QString& command, const QJsonObject& arguments,
            const commands::ProgressFn& progress) {
            return router.run(command, arguments, progress);
        });
    server.setLiveProbe([&router] { return router.live(); });
    return serve(server, in, protocol);
}

} // namespace kk::mcp
