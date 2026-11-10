// SPDX-License-Identifier: GPL-3.0-or-later
// McpStdio implementation.
#include "app/mcp/McpStdio.h"

#include "app/AppInfo.h"

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
    if (!options.isEmpty()) {
        std::fprintf(err, "kinoklip mcp: unknown option %s (usage: kinoklip mcp)\n",
            qPrintable(options.first()));
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
    McpServer server(commands::CommandRegistry::builtin());
    return serve(server, in, protocol);
}

} // namespace kk::mcp
