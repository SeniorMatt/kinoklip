// SPDX-License-Identifier: GPL-3.0-or-later
// McpServer — the protocol side of the Model Context Protocol server: JSON-RPC 2.0 messages in,
// replies out, written from the public specification (modelcontextprotocol.io). It knows nothing
// about stdin or stdout: feed it one message and it returns the reply, so tests can drive it with
// strings. The tools are the commands of the command layer (McpTools.h).
#pragma once

#include "app/commands/Command.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace kk::mcp {

/// Protocol versions the server speaks, newest first (initialize picks the client's when listed,
/// else the newest).
[[nodiscard]] QStringList supportedProtocolVersions();

/// Runs a command for tools/call. The default runs CommandRegistry::execute(); the live mode
/// (kinoklip mcp --live) sends it to the running window instead.
using Executor = std::function<commands::CommandResult(
    const QString& command, const QJsonObject& arguments, const commands::ProgressFn& progress)>;

/// One MCP session. Thread-safety: one thread (commands use the project model).
class McpServer
{
public:
    /// Serves the tools of @p registry (must outlive the server), running them with @p executor
    /// (default: registry.execute()).
    explicit McpServer(const commands::CommandRegistry& registry, Executor executor = {});

    /// Called with a complete message (one JSON line without the newline) to send to the client
    /// before handle() returns: progress notifications of a running tool.
    void setNotifier(std::function<void(const QByteArray&)> notifier)
    {
        m_notifier = std::move(notifier);
    }

    /// Handles one message from the client and returns the reply (compact JSON, no newline), or
    /// an empty array when none is due (a notification). Malformed JSON gets error -32700, an
    /// unknown method -32601, a bad request -32600, bad parameters -32602.
    [[nodiscard]] QByteArray handle(const QByteArray& message);

    /// The protocol version agreed by initialize (empty before).
    [[nodiscard]] QString protocolVersion() const { return m_protocolVersion; }

private:
    /// The reply object for request/notification @p message; empty object = no reply.
    QJsonObject handleOne(const QJsonObject& message);
    QJsonObject callTool(const QJsonValue& id, const QJsonObject& params);
    void notify(const QString& method, const QJsonObject& params) const;

    const commands::CommandRegistry& m_registry;
    Executor m_executor;
    std::function<void(const QByteArray&)> m_notifier;
    QString m_protocolVersion;
};

} // namespace kk::mcp
