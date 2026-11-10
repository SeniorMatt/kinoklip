// SPDX-License-Identifier: GPL-3.0-or-later
// McpStdio — `kinoklip mcp`: serves one MCP session on stdin / stdout, one JSON message per line.
#pragma once

#include "app/mcp/McpServer.h"

#include <cstdio>

namespace kk::mcp {

/// Reads messages from @p in (one per line) until end of input, passes them to @p server and
/// writes every reply and notification to @p out as one line, flushed. Nothing else is ever
/// written to @p out. Returns 0.
int serve(McpServer& server, std::FILE* in, std::FILE* out);

/// `kinoklip mcp`: the server of the built-in command registry on stdin / stdout. Anything that
/// library code prints to standard output is redirected to standard error first (the protocol
/// keeps a private copy of the real stdout), so the stream carries protocol messages only. Log
/// lines go to @p err. Needs a QGuiApplication (offscreen is fine) and kk::initResources().
/// @return the exit code: 0 at end of input.
int runStdioServer(const QStringList& options, std::FILE* in, std::FILE* out, std::FILE* err);

} // namespace kk::mcp
