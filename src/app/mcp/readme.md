# src/app/mcp - the Model Context Protocol server

`kinoklip mcp` lets a language-model client (an assistant that speaks the open
[Model Context Protocol](https://modelcontextprotocol.io)) control Kinoklip. It is a front end of
the command layer (`../commands/`), like the command line: no editing logic lives here. Written from
the public specification; no SDK. Local only: standard input and output, no network. When a running
window has Preferences > Local Control on, the tool calls are forwarded to it over a user-only local
socket (`../live/`); `--file` keeps the server on project files.

## Files

| File | What |
|---|---|
| `McpServer.h/.cpp` | the protocol: JSON-RPC 2.0 message in, reply out (`handle()`), `initialize` with version negotiation (2025-06-18, 2024-11-05), `ping`, `tools/list`, `tools/call`, error codes -32700 / -32600 / -32601 / -32602, progress notifications when the client sent a `progressToken`. Knows nothing about stdio; runs commands through an `Executor` (default `CommandRegistry::execute()`), so tests feed it strings |
| `McpTools.h/.cpp` | the registry as tools: tool name (`clip.add` -> `clip_add`), JSON Schema of the parameters, the tool result (sentence + JSON as text, `structuredContent` for 2025-06-18, an `image` item for a picture, `isError` with the stable error code) |
| `McpStdio.h/.cpp` | `kinoklip mcp`: one JSON message per line on stdin / stdout. Descriptor 1 is pointed at stderr while serving, the protocol keeps a private copy of the real stdout, so nothing but protocol messages can reach it; log lines go to stderr |

## Rules

- Never write anything but protocol messages to stdout. Logs go to stderr.
- The tool list always comes from `CommandRegistry`; a new command is a new tool with no change here. A command that returns `CommandResult::image` is sent as image content.
- Commands run on the thread that reads the messages, one at a time. `McpStdio.cpp` decides per call whether
  it runs in the window (`live::LiveClient`) or on files (`CommandRegistry::execute()`); `McpServer` knows
  only the `Executor` and, for `tools/list`, a probe saying whether the schemas are the live ones.

## How to extend

1. A new capability for the model is a new command in `../commands/` (described parameters; they become the schema and the tool description). Nothing to add here.
2. A new protocol method: a branch in `McpServer::handleOne()` and a capability entry in the `initialize` result; test it in `tests/app/test_mcp.cpp`.
