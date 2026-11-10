// SPDX-License-Identifier: GPL-3.0-or-later
// McpTools implementation.
#include "app/mcp/McpTools.h"

#include <QBuffer>
#include <QJsonDocument>

namespace kk::mcp {

namespace {

using commands::ParamSpec;
using commands::ParamType;

QString key(const char* text)
{
    return QString::fromLatin1(text);
}

QJsonObject propertySchema(const ParamSpec& p)
{
    QString description = p.description;
    QJsonObject o;
    switch (p.type) {
    case ParamType::String:
    case ParamType::Path:
        o.insert(key("type"), key("string"));
        break;
    case ParamType::Integer:
        o.insert(key("type"), key("integer"));
        break;
    case ParamType::Number:
        o.insert(key("type"), key("number"));
        break;
    case ParamType::Boolean:
        o.insert(key("type"), key("boolean"));
        break;
    case ParamType::Time:
        // A string: it carries both forms, and the registry also accepts a JSON number.
        o.insert(key("type"), key("string"));
        description += QStringLiteral(" (frames or timecode: \"125\" or \"00:00:05:10\")");
        break;
    case ParamType::StringList:
        o.insert(key("type"), key("array"));
        o.insert(key("items"), QJsonObject {{key("type"), key("string")}});
        break;
    }
    if (!p.choices.isEmpty())
        o.insert(key("enum"), QJsonArray::fromStringList(p.choices));
    if (!p.defaultText.isEmpty())
        description += QStringLiteral(" (default: %1)").arg(p.defaultText);
    o.insert(key("description"), description);
    return o;
}

} // namespace

QString toolName(const QString& commandName)
{
    return QString(commandName).replace(QLatin1Char('.'), QLatin1Char('_'));
}

QJsonObject inputSchema(const commands::CommandSpec& spec, bool live)
{
    QJsonObject properties;
    QJsonArray required;
    for (const ParamSpec& p : commands::CommandRegistry::fullParams(spec, live)) {
        properties.insert(p.name, propertySchema(p));
        if (p.required)
            required.append(p.name);
    }
    QJsonObject schema {{key("type"), key("object")}, {key("properties"), properties},
        {key("additionalProperties"), false}};
    if (!required.isEmpty())
        schema.insert(key("required"), required);
    return schema;
}

QJsonArray toolList(const commands::CommandRegistry& registry, bool live)
{
    QJsonArray tools;
    for (const commands::Command& c : registry.all()) {
        if (!c.spec.remoteAllowed)
            continue;
        const bool modifies
            = commands::CommandRegistry::describe(c.spec)[key("modifiesProject")].toBool();
        tools.append(QJsonObject {{key("name"), toolName(c.spec.name)},
            {key("description"), c.spec.description},
            {key("inputSchema"), inputSchema(c.spec, live)},
            {key("annotations"), QJsonObject {{key("readOnlyHint"), !modifies}}}});
    }
    return tools;
}

const commands::Command* findCommand(const commands::CommandRegistry& registry, const QString& tool)
{
    for (const commands::Command& c : registry.all()) {
        if (c.spec.remoteAllowed && toolName(c.spec.name) == tool)
            return &c;
    }
    return nullptr;
}

QJsonObject toolResult(const commands::CommandResult& result, bool withStructured)
{
    const auto text = [](const QString& s) {
        return QJsonObject {
            {QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), s}};
    };
    const auto compact = [](const QJsonObject& o) {
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    };
    QJsonArray content;
    QJsonObject out;
    if (!result.ok) {
        const QJsonObject error = result.toJson();
        content.append(text(QStringLiteral("%1: %2").arg(result.errorCode, result.error)));
        content.append(text(compact(error)));
        out.insert(key("isError"), true);
        if (withStructured)
            out.insert(key("structuredContent"), error);
    } else {
        content.append(text(result.text));
        content.append(text(compact(result.data)));
        if (!result.image.isNull()) {
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            result.image.save(&buffer, "PNG");
            content.append(QJsonObject {{QStringLiteral("type"), QStringLiteral("image")},
                {QStringLiteral("data"), QString::fromLatin1(png.toBase64())},
                {QStringLiteral("mimeType"), QStringLiteral("image/png")}});
        }
        out.insert(key("isError"), false);
        if (withStructured)
            out.insert(key("structuredContent"), result.data);
    }
    out.insert(key("content"), content);
    return out;
}

} // namespace kk::mcp
