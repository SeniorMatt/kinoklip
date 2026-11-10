// SPDX-License-Identifier: GPL-3.0-or-later
// McpTools — turns the command registry into MCP tools (names, JSON Schema) and a command
// result into an MCP tool result. No I/O and no protocol state: pure functions.
#pragma once

#include "app/commands/Command.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace kk::mcp {

/// The tool name of command @p commandName: dots become underscores ("clip.add" -> "clip_add").
[[nodiscard]] QString toolName(const QString& commandName);

/// JSON Schema (type "object") of the parameters of @p spec, universal parameters included.
/// Required parameters are listed in "required"; a Time is a string described as "frames or
/// timecode"; a choice list is an "enum"; a list of strings an array of strings.
[[nodiscard]] QJsonObject inputSchema(const commands::CommandSpec& spec);

/// The "tools" array of tools/list: one entry (name, description, inputSchema, annotations)
/// per command of @p registry.
[[nodiscard]] QJsonArray toolList(const commands::CommandRegistry& registry);

/// The command registered under tool name @p tool, or nullptr.
[[nodiscard]] const commands::Command* findCommand(
    const commands::CommandRegistry& registry, const QString& tool);

/// The result of a tools/call for @p result. content: the sentence as text, the JSON as text
/// (for clients that do not read structuredContent) and, for a picture, an image item (base64
/// PNG). structuredContent (the JSON object) only when @p withStructured (protocol 2025-06-18).
/// A failure has isError true and the stable error code in the text.
[[nodiscard]] QJsonObject toolResult(const commands::CommandResult& result, bool withStructured);

} // namespace kk::mcp
