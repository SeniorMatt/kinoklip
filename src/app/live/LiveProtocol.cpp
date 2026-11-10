// SPDX-License-Identifier: GPL-3.0-or-later
// LiveProtocol implementation.
#include "app/live/LiveProtocol.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace kk::live {

namespace {

quint64 currentUserId()
{
#ifdef Q_OS_UNIX
    return quint64(::getuid());
#else
    return 0;
#endif
}

} // namespace

QString socketPath()
{
    if (qEnvironmentVariableIsSet("KK_CONTROL_SOCKET"))
        return qEnvironmentVariable("KK_CONTROL_SOCKET");
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
    return QDir(dir).filePath(QStringLiteral("kinoklip-control-%1.sock").arg(currentUserId()));
}

bool isOwnedByCurrentUser(const QString& path)
{
#ifdef Q_OS_UNIX
    const QFileInfo info(path);
    return info.exists() && info.ownerId() == uint(::getuid());
#else
    return QFileInfo::exists(path);
#endif
}

QJsonObject resultToJson(const commands::CommandResult& result)
{
    QJsonObject o {
        {QStringLiteral("op"), QStringLiteral("result")}, {QStringLiteral("ok"), result.ok}};
    if (result.ok) {
        o.insert(QStringLiteral("data"), result.data);
        o.insert(QStringLiteral("text"), result.text);
        if (!result.image.isNull()) {
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            result.image.save(&buffer, "PNG");
            o.insert(QStringLiteral("image"), QString::fromLatin1(png.toBase64()));
        }
    } else {
        o.insert(QStringLiteral("errorCode"), result.errorCode);
        o.insert(QStringLiteral("error"), result.error);
    }
    return o;
}

commands::CommandResult resultFromJson(const QJsonObject& message)
{
    if (!message.value(QStringLiteral("ok")).toBool())
        return commands::CommandResult::failure(
            message.value(QStringLiteral("errorCode")).toString(),
            message.value(QStringLiteral("error")).toString());
    commands::CommandResult r
        = commands::CommandResult::success(message.value(QStringLiteral("data")).toObject(),
            message.value(QStringLiteral("text")).toString());
    if (message.contains(QStringLiteral("image")))
        r.image.loadFromData(
            QByteArray::fromBase64(message.value(QStringLiteral("image")).toString().toLatin1()),
            "PNG");
    return r;
}

QByteArray encode(const QJsonObject& message)
{
    return QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
}

} // namespace kk::live
