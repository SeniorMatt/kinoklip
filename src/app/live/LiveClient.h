// SPDX-License-Identifier: GPL-3.0-or-later
// LiveClient — the other end of LiveServer, used by `kinoklip mcp` and `kinoklip <command> --live`:
// connects to the open window's local socket and forwards commands. Blocking calls (these
// processes have nothing else to do while a command runs).
#pragma once

#include "app/commands/Command.h"

#include <QByteArray>
#include <QLocalSocket>
#include <QString>

namespace kk::live {

/// One connection to a window. Thread-safety: one thread.
class LiveClient
{
public:
    /// Connects to the socket (default live::socketPath()) and checks the hello reply. False
    /// with @p why set when nothing listens there (the option is off or no window is open), the
    /// socket belongs to another user, or the protocol version differs.
    bool connectToWindow(QString* why = nullptr, const QString& path = {});
    [[nodiscard]] bool isConnected() const
    {
        return m_socket.state() == QLocalSocket::ConnectedState;
    }
    void disconnectFromWindow();

    /// The project the window had open at hello (a path, empty if it was never saved).
    [[nodiscard]] QString projectPath() const { return m_projectPath; }
    [[nodiscard]] QString projectName() const { return m_projectName; }

    /// Runs @p command in the window and waits for the result; @p progress gets the progress
    /// messages. A connection that breaks is the failure `live_disconnected` (the command may
    /// have run).
    [[nodiscard]] commands::CommandResult call(const QString& command, const QJsonObject& arguments,
        const commands::ProgressFn& progress = {});

private:
    /// Next message line within @p timeoutMs (-1 = wait), parsed; false when the connection
    /// ended or the time ran out.
    bool readMessage(QJsonObject& message, int timeoutMs);

    QLocalSocket m_socket;
    QByteArray m_buffer;
    QString m_projectPath;
    QString m_projectName;
    qint64 m_nextId = 1;
};

} // namespace kk::live
