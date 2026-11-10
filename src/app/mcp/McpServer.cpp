// SPDX-License-Identifier: GPL-3.0-or-later
// McpServer implementation.
#include "app/mcp/McpServer.h"

#include "app/AppInfo.h"
#include "app/mcp/McpTools.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace kk::mcp {

namespace {

// JSON-RPC 2.0 error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

const QString kLatestVersion = QStringLiteral("2025-06-18");

QString k(const char* text)
{
    return QString::fromLatin1(text);
}

QJsonObject errorReply(const QJsonValue& id, int code, const QString& message)
{
    return {{k("jsonrpc"), k("2.0")}, {k("id"), id},
        {k("error"), QJsonObject {{k("code"), code}, {k("message"), message}}}};
}

QJsonObject okReply(const QJsonValue& id, const QJsonObject& result)
{
    return {{k("jsonrpc"), k("2.0")}, {k("id"), id}, {k("result"), result}};
}

QByteArray compact(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

/// What the model is told when it connects.
QString serverInstructions()
{
    return QStringLiteral(
        "Kinoklip is a video editor. Each tool edits a project file given as the `project` "
        "argument (an absolute path ending in .kkproj; relative paths are relative to the "
        "server's folder) and saves the change into it, so call tools one after another. Start "
        "with project_new, then media_import, clip_add, clip_trim, clip_split, marker_add, "
        "transition_add. Use project_info, clip_list and media_list to see the state, "
        "transition_list and effect_list for ids, and frame_view to look at a frame of the "
        "timeline. Times are timeline frames or timecode HH:MM:SS:FF counted from the start of "
        "the timeline. Clip ids come from clip_list. Errors carry a stable code in the text.");
}

} // namespace

QStringList supportedProtocolVersions()
{
    return {kLatestVersion, QStringLiteral("2024-11-05")};
}

McpServer::McpServer(const commands::CommandRegistry& registry, Executor executor)
    : m_registry(registry)
    , m_executor(std::move(executor))
{
    if (!m_executor) {
        m_executor = [this](const QString& command, const QJsonObject& arguments,
                         const commands::ProgressFn& progress) {
            return m_registry.execute(command, arguments, progress);
        };
    }
}

void McpServer::notify(const QString& method, const QJsonObject& params) const
{
    if (!m_notifier)
        return;
    m_notifier(compact({{k("jsonrpc"), k("2.0")}, {k("method"), method}, {k("params"), params}}));
}

QByteArray McpServer::handle(const QByteArray& message)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(message, &parseError);
    if (parseError.error != QJsonParseError::NoError || (!doc.isObject() && !doc.isArray()))
        return compact(errorReply(QJsonValue::Null, kParseError,
            QStringLiteral("parse error: %1").arg(parseError.errorString())));
    if (doc.isObject()) {
        const QJsonObject reply = handleOne(doc.object());
        return reply.isEmpty() ? QByteArray() : compact(reply);
    }
    // A batch (allowed by 2024-11-05): one array of replies, nothing when all were notifications.
    QJsonArray replies;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject reply = v.isObject()
            ? handleOne(v.toObject())
            : errorReply(QJsonValue::Null, kInvalidRequest, QStringLiteral("invalid request"));
        if (!reply.isEmpty())
            replies.append(reply);
    }
    if (doc.array().isEmpty())
        return compact(errorReply(
            QJsonValue::Null, kInvalidRequest, QStringLiteral("invalid request: empty batch")));
    return replies.isEmpty() ? QByteArray() : QJsonDocument(replies).toJson(QJsonDocument::Compact);
}

QJsonObject McpServer::handleOne(const QJsonObject& message)
{
    const QJsonValue id = message.value(k("id")); // undefined for a notification
    const bool isRequest = message.contains(k("id"));
    const QJsonValue methodValue = message.value(k("method"));
    if (message.value(k("jsonrpc")).toString() != k("2.0") || !methodValue.isString()) {
        // A response from the client (no method) to something we never asked is ignored.
        if (methodValue.isUndefined()
            && (message.contains(k("result")) || message.contains(k("error"))))
            return {};
        return errorReply(isRequest ? id : QJsonValue::Null, kInvalidRequest,
            QStringLiteral("invalid request: \"jsonrpc\":\"2.0\" and a method are required"));
    }
    const QString method = methodValue.toString();
    const QJsonObject params = message.value(k("params")).toObject();

    if (!isRequest)
        return {}; // notifications (initialized, cancelled, ...) need no reply

    if (method == QLatin1String("initialize")) {
        const QString wanted = params.value(k("protocolVersion")).toString();
        m_protocolVersion = supportedProtocolVersions().contains(wanted) ? wanted : kLatestVersion;
        return okReply(id,
            {{k("protocolVersion"), m_protocolVersion},
                {k("capabilities"),
                    QJsonObject {{k("tools"), QJsonObject {{k("listChanged"), false}}}}},
                {k("serverInfo"),
                    QJsonObject {
                        {k("name"), k("kinoklip")}, {k("version"), appinfo::displayVersion()}}},
                {k("instructions"), serverInstructions()}});
    }
    if (method == QLatin1String("ping"))
        return okReply(id, {});
    if (method == QLatin1String("tools/list"))
        return okReply(id, {{k("tools"), toolList(m_registry)}});
    if (method == QLatin1String("tools/call"))
        return callTool(id, params);
    return errorReply(id, kMethodNotFound, QStringLiteral("method not found: %1").arg(method));
}

QJsonObject McpServer::callTool(const QJsonValue& id, const QJsonObject& params)
{
    const QString tool = params.value(k("name")).toString();
    const commands::Command* command = findCommand(m_registry, tool);
    if (!command)
        return errorReply(id, kInvalidParams, QStringLiteral("unknown tool: %1").arg(tool));
    const QJsonValue arguments = params.value(k("arguments"));
    if (!arguments.isUndefined() && !arguments.isNull() && !arguments.isObject())
        return errorReply(id, kInvalidParams, QStringLiteral("arguments must be an object"));

    // Progress goes out as notifications only when the client asked with a progressToken.
    const QJsonValue token = params.value(k("_meta")).toObject().value(k("progressToken"));
    commands::ProgressFn progress;
    if (token.isString() || token.isDouble()) {
        // The specification wants "progress" to increase with every notification.
        progress = [this, token, last = 0.0](double fraction, const QString& message) mutable {
            if (fraction <= last)
                return;
            last = fraction;
            notify(k("notifications/progress"),
                {{k("progressToken"), token}, {k("progress"), fraction}, {k("total"), 1.0},
                    {k("message"), message}});
        };
    }
    const commands::CommandResult result
        = m_executor(command->spec.name, arguments.toObject(), progress);
    return okReply(id, toolResult(result, m_protocolVersion == kLatestVersion));
}

} // namespace kk::mcp
