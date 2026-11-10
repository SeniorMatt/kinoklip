// SPDX-License-Identifier: GPL-3.0-or-later
// LiveClient implementation.
#include "app/live/LiveClient.h"

#include "app/live/LiveProtocol.h"

#include <QFileInfo>
#include <QJsonDocument>

namespace kk::live {

namespace {

constexpr int kConnectMs = 500; ///< a window that listens answers at once
constexpr int kHelloMs = 5000; ///< the window may be busy with a long edit
constexpr int kPollMs = 250; ///< how often a waiting call checks that the window is still there

} // namespace

void LiveClient::disconnectFromWindow()
{
    if (m_socket.state() != QLocalSocket::UnconnectedState) {
        m_socket.disconnectFromServer();
        if (m_socket.state() != QLocalSocket::UnconnectedState)
            m_socket.waitForDisconnected(kPollMs);
    }
    m_buffer.clear();
}

bool LiveClient::connectToWindow(QString* why, const QString& pathArgument)
{
    const QString path = pathArgument.isEmpty() ? socketPath() : pathArgument;
    auto fail = [&](const QString& message) {
        if (why)
            *why = message;
        disconnectFromWindow();
        return false;
    };
    if (isConnected())
        return true;
    m_socket.abort();
    m_buffer.clear();
    if (!isOwnedByCurrentUser(path))
        return fail(QFileInfo::exists(path)
                ? QStringLiteral("%1 belongs to another user").arg(path)
                : QStringLiteral("no Kinoklip window offers local control (%1 does not exist)")
                      .arg(path));
    m_socket.connectToServer(path);
    if (!m_socket.waitForConnected(kConnectMs))
        return fail(QStringLiteral("no Kinoklip window accepts local control: %1")
                        .arg(m_socket.errorString()));
    m_socket.write(encode({{QStringLiteral("op"), QStringLiteral("hello")}}));
    QJsonObject hello;
    if (!readMessage(hello, kHelloMs)
        || hello.value(QStringLiteral("op")).toString() != QLatin1String("hello"))
        return fail(QStringLiteral("the Kinoklip window did not answer"));
    if (hello.value(QStringLiteral("protocol")).toInt() != kProtocolVersion)
        return fail(QStringLiteral("the Kinoklip window speaks protocol %1, this program %2")
                        .arg(hello.value(QStringLiteral("protocol")).toInt())
                        .arg(kProtocolVersion));
    m_projectPath = hello.value(QStringLiteral("project")).toString();
    m_projectName = hello.value(QStringLiteral("name")).toString();
    return true;
}

bool LiveClient::readMessage(QJsonObject& message, int timeoutMs)
{
    // The poll loop below also notices a window that closed without a message.
    int waited = 0;
    while (true) {
        const qsizetype newline = m_buffer.indexOf('\n');
        if (newline >= 0) {
            const QByteArray line = m_buffer.left(newline);
            m_buffer.remove(0, newline + 1);
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (!doc.isObject())
                continue;
            message = doc.object();
            return true;
        }
        if (m_socket.state() != QLocalSocket::ConnectedState && m_socket.bytesAvailable() == 0)
            return false;
        const int slice = timeoutMs < 0 ? kPollMs : qMin(kPollMs, timeoutMs - waited);
        if (slice <= 0)
            return false;
        if (m_socket.waitForReadyRead(slice))
            m_buffer.append(m_socket.readAll());
        else if (timeoutMs >= 0)
            waited += slice;
    }
}

commands::CommandResult LiveClient::call(
    const QString& command, const QJsonObject& arguments, const commands::ProgressFn& progress)
{
    const auto lost = [] {
        return commands::CommandResult::failure(QStringLiteral("live_disconnected"),
            QStringLiteral("the Kinoklip window closed the connection; the command may or may "
                           "not have been done (look at the window)"));
    };
    if (!isConnected())
        return lost();
    const qint64 id = m_nextId++;
    m_socket.write(
        encode({{QStringLiteral("op"), QStringLiteral("call")}, {QStringLiteral("id"), id},
            {QStringLiteral("command"), command}, {QStringLiteral("arguments"), arguments}}));
    m_socket.flush();
    QJsonObject message;
    while (readMessage(message, -1)) {
        if (message.value(QStringLiteral("id")).toInteger(-1) != id)
            continue; // an answer to nothing we asked
        const QString op = message.value(QStringLiteral("op")).toString();
        if (op == QLatin1String("progress")) {
            if (progress)
                progress(message.value(QStringLiteral("fraction")).toDouble(),
                    message.value(QStringLiteral("message")).toString());
        } else if (op == QLatin1String("result")) {
            return resultFromJson(message);
        }
    }
    disconnectFromWindow();
    return lost();
}

} // namespace kk::live
