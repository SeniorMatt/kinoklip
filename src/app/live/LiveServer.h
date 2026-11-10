// SPDX-License-Identifier: GPL-3.0-or-later
// LiveServer — hosts the command registry on a local socket so tools on this computer can control
// the open window (`kinoklip mcp`, `kinoklip <command> --live`). The window owns one; it listens
// only while Preferences > Local Control is on.
#pragma once

#include "app/commands/Command.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QString>

class QLocalServer;
class QLocalSocket;

namespace kk::live {

/// A QLocalServer for the command registry. Each call runs on the thread of the server (the GUI
/// thread) against the project of the window, through CommandRegistry::executeLive(), so it is
/// one undo step; renders and frame exports (CommandSpec::background) run on a copy of the
/// project in a worker thread and answer when done, so the window stays responsive.
///
/// Security: the socket is created with QLocalServer::UserAccessOption in the user's runtime
/// folder, a connection from another user (SO_PEERCRED) is dropped, and nothing listens on the
/// network. Thread-safety: GUI thread only.
class LiveServer : public QObject
{
    Q_OBJECT
public:
    /// @p target is the window's project and Save; both must outlive the server.
    explicit LiveServer(const commands::CommandRegistry& registry, commands::LiveTarget target,
        QObject* parent = nullptr);
    ~LiveServer() override;

    /// Another socket path than live::socketPath() (tests); call before listen().
    void setSocketPath(const QString& path) { m_path = path; }
    /// Starts listening. Fails (false, @p error) when another window already listens there; a
    /// socket file left by a crashed run is replaced.
    bool listen(QString* error = nullptr);
    /// Closes every connection and stops listening.
    void close();
    [[nodiscard]] bool isListening() const;
    /// The socket path in use.
    [[nodiscard]] QString socketPath() const;
    /// Number of connected clients.
    [[nodiscard]] int clientCount() const { return int(m_clients.size()); }

signals:
    /// A client connected or went away (the window shows an indicator while > 0).
    void clientCountChanged(int count);

private:
    struct Client
    {
        QLocalSocket* socket = nullptr;
        QByteArray buffer;
        QQueue<QByteArray> pending; ///< lines that arrived while a background job runs
        bool busy = false;
    };

    void acceptClients();
    void readFrom(QLocalSocket* socket);
    void drain(QLocalSocket* socket);
    void handleLine(QLocalSocket* socket, const QByteArray& line);
    void runCall(QLocalSocket* socket, const QJsonValue& id, const QString& command,
        const QJsonObject& arguments);
    void send(QLocalSocket* socket, const QJsonObject& message);
    void dropClient(QLocalSocket* socket);

    const commands::CommandRegistry& m_registry;
    commands::LiveTarget m_target;
    QLocalServer* m_server = nullptr;
    QString m_path;
    QHash<QLocalSocket*, Client> m_clients;
};

} // namespace kk::live
