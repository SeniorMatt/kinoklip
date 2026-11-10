# src/app/live - live control of the open window

Lets tools on this computer run the commands of `../commands/` on the project that is open in the main
window: `kinoklip mcp` (an AI assistant) and `kinoklip <command> --live`. Off by default; switched on in
Preferences > System > Local Control (`prefs::localControlEnabled()`, spec 55.8).

## Files

| File | What |
|---|---|
| `LiveServer.h/.cpp` | `QLocalServer` hosted by the main window (`MainWindow::liveServer()`, started and stopped by `applyLocalControlPreference()`). Per client: lines in, one call at a time. A call runs on the GUI thread through `CommandRegistry::executeLive()` (one undo step); a `CommandSpec::background` command runs on a copy of the project in a worker thread and answers when done. Emits `clientCountChanged` (the interface bar shows "Assistant connected") |
| `LiveClient.h/.cpp` | the blocking client: connect and hello, `call()` with progress |
| `LiveProtocol.h/.cpp` | the socket path, ownership check, JSON of a `CommandResult` (a picture is base64 PNG), the protocol described at the top of the header |

## Rules

- Local and user-only: the socket is in `$XDG_RUNTIME_DIR`, made with `QLocalServer::UserAccessOption`, a client refuses a socket that another user owns and the server drops a peer of another user. Nothing here may listen on a network port.
- No editing logic: the server only forwards to `CommandRegistry`. A new command needs nothing here.
- Never block the GUI thread for long: heavy commands are `background`. Everything else is an edit of the project model.
- The protocol is private and versioned (`kProtocolVersion`): change it only together with both ends.

## How to extend

1. A new capability is a new command in `../commands/`; it appears in live mode by itself.
2. A long job: set `CommandSpec::background` (ProjectUse::Read) so it runs on the snapshot.
3. A new message: handle it in `LiveServer::handleLine()`, send it from `LiveClient`, add a case to `tests/app/test_livecontrol.cpp`, and bump `kProtocolVersion` if old clients would misread it.
