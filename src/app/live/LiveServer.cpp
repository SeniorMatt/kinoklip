// SPDX-License-Identifier: GPL-3.0-or-later
// LiveServer implementation.
#include "app/live/LiveServer.h"

#include "app/AppInfo.h"
#include "app/live/LiveProtocol.h"
#include "core/Project.h"

#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QtConcurrent>

#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kk::live {

namespace {

constexpr int kProbeMs = 200; ///< how long to wait for a possible owner of a stale socket

/// False when the other end of @p socket is another user (Linux: SO_PEERCRED). The socket file
/// is user-only already; this is the second lock. Other systems rely on the file permissions.
bool peerIsCurrentUser(const QLocalSocket* socket)
{
#ifdef Q_OS_LINUX
    ucred cred {};
    socklen_t length = sizeof cred;
    const qintptr fd = socket->socketDescriptor();
    if (fd < 0 || ::getsockopt(int(fd), SOL_SOCKET, SO_PEERCRED, &cred, &length) != 0)
        return false;
    return cred.uid == ::getuid();
#else
    Q_UNUSED(socket);
    return true;
#endif
}

} // namespace

LiveServer::LiveServer(
    const commands::CommandRegistry& registry, commands::LiveTarget target, QObject* parent)
    : QObject(parent)
    , m_registry(registry)
    , m_target(std::move(target))
{ }

LiveServer::~LiveServer()
{
    close();
}

QString LiveServer::socketPath() const
{
    return m_path.isEmpty() ? live::socketPath() : m_path;
}

bool LiveServer::isListening() const
{
    return m_server && m_server->isListening();
}

bool LiveServer::listen(QString* error)
{
    if (isListening())
        return true;
    const QString path = socketPath();
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    if (QFileInfo::exists(path)) {
        if (!isOwnedByCurrentUser(path))
            return fail(QStringLiteral("%1 belongs to another user").arg(path));
        // A live socket means another window offers control already; a dead one is a leftover.
        QLocalSocket probe;
        probe.connectToServer(path);
        if (probe.waitForConnected(kProbeMs))
            return fail(QStringLiteral("another Kinoklip window already offers local control"));
        QLocalServer::removeServer(path);
    }
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption); // mode 0700: this user only
    m_server->setMaxPendingConnections(8);
    if (!m_server->listen(path)) {
        const QString why = m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return fail(why);
    }
    connect(m_server, &QLocalServer::newConnection, this, &LiveServer::acceptClients);
    return true;
}

void LiveServer::close()
{
    const bool had = !m_clients.isEmpty();
    const auto sockets = m_clients.keys();
    m_clients.clear();
    for (QLocalSocket* s : sockets) {
        s->disconnect(this);
        s->disconnectFromServer();
        s->deleteLater();
    }
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
    if (had)
        emit clientCountChanged(0);
}

void LiveServer::acceptClients()
{
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket* socket = m_server->nextPendingConnection();
        if (!peerIsCurrentUser(socket)) {
            socket->deleteLater();
            continue;
        }
        m_clients.insert(socket, Client {socket, {}, {}, false});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { readFrom(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] { dropClient(socket); });
        emit clientCountChanged(clientCount());
    }
}

void LiveServer::dropClient(QLocalSocket* socket)
{
    if (!m_clients.remove(socket))
        return;
    socket->deleteLater();
    emit clientCountChanged(clientCount());
}

void LiveServer::readFrom(QLocalSocket* socket)
{
    const auto it = m_clients.find(socket);
    if (it == m_clients.end())
        return;
    it->buffer.append(socket->readAll());
    // A line is a whole message. An endless line from a broken client is cut off.
    if (!it->buffer.contains('\n') && it->buffer.size() > kMaxLineBytes) {
        dropClient(socket);
        return;
    }
    int newline;
    while ((newline = int(it->buffer.indexOf('\n'))) >= 0) {
        it->pending.enqueue(it->buffer.left(newline));
        it->buffer.remove(0, newline + 1);
    }
    drain(socket);
}

void LiveServer::drain(QLocalSocket* socket)
{
    // Lines run in order; a background job (render) holds the rest until it has answered.
    while (true) {
        const auto it = m_clients.find(socket);
        if (it == m_clients.end() || it->busy || it->pending.isEmpty())
            return;
        const QByteArray line = it->pending.dequeue();
        if (!line.trimmed().isEmpty())
            handleLine(socket, line);
    }
}

void LiveServer::send(QLocalSocket* socket, const QJsonObject& message)
{
    if (socket && socket->state() == QLocalSocket::ConnectedState)
        socket->write(encode(message));
}

void LiveServer::handleLine(QLocalSocket* socket, const QByteArray& line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        send(socket,
            resultToJson(commands::CommandResult::failure(
                QStringLiteral("bad_message"), QStringLiteral("not a JSON object"))));
        return;
    }
    const QJsonObject message = doc.object();
    const QString op = message.value(QStringLiteral("op")).toString();
    if (op == QLatin1String("hello")) {
        const commands::LiveTarget& t = m_target;
        const QString path = t.project->filePath();
        send(socket,
            {{QStringLiteral("op"), QStringLiteral("hello")},
                {QStringLiteral("protocol"), kProtocolVersion},
                {QStringLiteral("app"), QString::fromLatin1(appinfo::name)},
                {QStringLiteral("version"), appinfo::displayVersion()},
                {QStringLiteral("project"),
                    path.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(path)},
                {QStringLiteral("name"), t.project->name()}});
        return;
    }
    if (op == QLatin1String("call")) {
        runCall(socket, message.value(QStringLiteral("id")),
            message.value(QStringLiteral("command")).toString(),
            message.value(QStringLiteral("arguments")).toObject());
        return;
    }
    send(socket,
        resultToJson(commands::CommandResult::failure(
            QStringLiteral("bad_message"), QStringLiteral("unknown op \"%1\"").arg(op))));
}

void LiveServer::runCall(QLocalSocket* socket, const QJsonValue& id, const QString& command,
    const QJsonObject& arguments)
{
    const auto reply = [this, id](QLocalSocket* s, const commands::CommandResult& result) {
        QJsonObject o = resultToJson(result);
        o.insert(QStringLiteral("id"), id);
        send(s, o);
    };
    const auto progressTo = [this, id](QLocalSocket* s) {
        return [this, id, s = QPointer<QLocalSocket>(s)](double fraction, const QString& message) {
            send(s,
                {{QStringLiteral("op"), QStringLiteral("progress")}, {QStringLiteral("id"), id},
                    {QStringLiteral("fraction"), fraction}, {QStringLiteral("message"), message}});
        };
    };

    const commands::Command* c = m_registry.find(command);
    if (!c || !c->spec.background) {
        reply(socket, m_registry.executeLive(command, arguments, m_target, progressTo(socket)));
        return;
    }

    // A render or frame export: take a copy of the document now, on this thread, and run the
    // command on it in a worker, so the window keeps painting and the user can keep working.
    const QString documentPath = m_target.project->filePath();
    const QByteArray document = m_target.project->documentJson(documentPath);
    auto* watcher = new QFutureWatcher<commands::CommandResult>(this);
    m_clients[socket].busy = true;
    // The worker reports progress from its own thread; the message is sent from this one.
    const QPointer<QLocalSocket> target(socket);
    const QPointer<LiveServer> self(this);
    const commands::ProgressFn workerProgress = [self, id, target](
                                                    double fraction, const QString& m) {
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, id, target, fraction, m] {
                if (self)
                    self->send(target,
                        {{QStringLiteral("op"), QStringLiteral("progress")},
                            {QStringLiteral("id"), id}, {QStringLiteral("fraction"), fraction},
                            {QStringLiteral("message"), m}});
            },
            Qt::QueuedConnection);
    };
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, target, reply] {
        const commands::CommandResult result = watcher->result();
        watcher->deleteLater();
        if (!target)
            return; // the client went away while it worked
        reply(target, result);
        if (const auto it = m_clients.find(target); it != m_clients.end())
            it->busy = false;
        drain(target);
    });
    // The registry is the static built-in one; the worker takes a reference to it, not the server.
    const commands::CommandRegistry& registry = m_registry;
    watcher->setFuture(
        QtConcurrent::run([&registry, command, arguments, document, documentPath, workerProgress] {
            return registry.executeOnSnapshot(
                command, arguments, document, documentPath, workerProgress);
        }));
}

} // namespace kk::live
