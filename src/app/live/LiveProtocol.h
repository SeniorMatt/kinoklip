// SPDX-License-Identifier: GPL-3.0-or-later
// LiveProtocol — the small private protocol between `kinoklip mcp` / `kinoklip <command> --live`
// and an open Kinoklip window: line-delimited JSON on a per-user local socket. Private means it
// may change between versions; the version number below is checked by the hello exchange.
//
//   client -> window   {"op":"hello"}
//   window -> client   {"op":"hello","protocol":1,"app":"Kinoklip","version":"...",
//                       "project":"/path.kkproj"|null,"name":"Project name"}
//   client -> window   {"op":"call","id":7,"command":"clip.add","arguments":{...}}
//   window -> client   {"op":"progress","id":7,"fraction":0.4,"message":"..."}   (zero or more)
//   window -> client   {"op":"result","id":7,"ok":true,"data":{...},"text":"...","image":"<base64
//   png>"}
//                      {"op":"result","id":7,"ok":false,"errorCode":"unknown_clip","error":"..."}
//
// One call is answered before the next of the same client starts. A message is one line.
#pragma once

#include "app/commands/Command.h"

#include <QJsonObject>
#include <QString>

namespace kk::live {

/// Version of this protocol, in the hello reply.
inline constexpr int kProtocolVersion = 1;

/// Upper bound of one message line (a result with a frame picture is the largest).
inline constexpr qint64 kMaxLineBytes = 64LL * 1024 * 1024;

/// The socket of the current user: `$XDG_RUNTIME_DIR/kinoklip-control-<uid>.sock` (a folder only
/// the user can enter), else `<temp>/kinoklip-control-<uid>.sock`. The environment variable
/// KK_CONTROL_SOCKET names another path (tests).
[[nodiscard]] QString socketPath();

/// True when @p path is a socket owned by the current user (the client refuses anything else).
[[nodiscard]] bool isOwnedByCurrentUser(const QString& path);

/// A command result as a protocol message (op "result", without the id).
[[nodiscard]] QJsonObject resultToJson(const commands::CommandResult& result);
/// The result in a "result" message.
[[nodiscard]] commands::CommandResult resultFromJson(const QJsonObject& message);

/// One message as a compact line with the trailing newline.
[[nodiscard]] QByteArray encode(const QJsonObject& message);

} // namespace kk::live
