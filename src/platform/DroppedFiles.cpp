// SPDX-License-Identifier: GPL-3.0-or-later
// DroppedFiles implementation: uri-list parsing and the document portal's RetrieveFiles call.
#include "platform/DroppedFiles.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QLoggingCategory>
#include <QMimeData>
#include <QRegularExpression>
#include <QUrl>
#include <QVariantMap>

namespace kk::platform {

Q_LOGGING_CATEGORY(lcDrop, "kinoklip.drop")

namespace {
constexpr char kKdeUriListMime[] = "application/x-kde4-urilist";
constexpr char kMozUrlMime[] = "text/x-moz-url";
} // namespace

bool hasDroppedFiles(const QMimeData* data)
{
    return data
        && (data->hasUrls() || data->hasFormat(QLatin1String(kPortalFileTransferMime))
            || data->hasFormat(QLatin1String(kKdeUriListMime))
            || data->hasFormat(QLatin1String(kMozUrlMime)));
}

namespace {

/// Asks org.freedesktop.portal.Documents (FileTransfer interface) for the files of @p key.
QStringList retrievePortalFiles(const QByteArray& key)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || key.isEmpty())
        return {};
    QDBusMessage call
        = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Documents"),
            QStringLiteral("/org/freedesktop/portal/documents"),
            QStringLiteral("org.freedesktop.portal.FileTransfer"), QStringLiteral("RetrieveFiles"));
    // The key is NUL-terminated text in the MIME payload.
    QByteArray text = key;
    while (text.endsWith('\0'))
        text.chop(1);
    call << QString::fromUtf8(text) << QVariantMap();
    const QDBusReply<QStringList> reply = bus.call(call, QDBus::Block, kPortalTimeoutMs);
    if (!reply.isValid()) {
        qCInfo(lcDrop) << "document portal RetrieveFiles failed:" << reply.error().message();
        return {};
    }
    return reply.value();
}

} // namespace

QStringList droppedLocalFiles(const QMimeData* data)
{
    QStringList paths;
    if (!data)
        return paths;
    auto add = [&paths](const QString& path) {
        if (!path.isEmpty() && !paths.contains(path))
            paths.append(path);
    };
    if (data->hasUrls()) {
        for (const QUrl& url : data->urls()) {
            if (url.isLocalFile())
                add(url.toLocalFile());
        }
    }
    if (paths.isEmpty() && data->hasFormat(QLatin1String(kPortalFileTransferMime))) {
        for (const QString& path :
            retrievePortalFiles(data->data(QLatin1String(kPortalFileTransferMime))))
            add(path);
    }
    // Qt Wayland (6.7 - 6.12 at least) reads text/uri-list through the document portal whenever
    // the source also offers a portal key, and when the portal cannot resolve it (no
    // xdg-document-portal, portal not reachable from the bundle's session) it never falls back
    // to the real uri-list: QWaylandMimeData::retrieveData_sys() re-reads the *key* instead
    // (qwaylanddataoffer.cpp, "Failed retrieving files, falling back to uris"). KDE sources
    // (Dolphin, Plasma desktop: KUrlMimeData) also offer the same list as
    // application/x-kde4-urilist, and Firefox-like sources text/x-moz-url; Qt reads those raw.
    for (const char* alias : {kKdeUriListMime, kMozUrlMime}) {
        if (!paths.isEmpty() || !data->hasFormat(QLatin1String(alias)))
            continue;
        const QByteArray raw = data->data(QLatin1String(alias));
        const bool moz = qstrcmp(alias, kMozUrlMime) == 0;
        // text/x-moz-url is UTF-16 "url\ntitle" pairs; the KDE list is a plain uri-list.
        const QString text = moz
            ? QString::fromUtf16(reinterpret_cast<const char16_t*>(raw.constData()), raw.size() / 2)
            : QString::fromUtf8(raw);
        const QStringList lines
            = text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
        for (int i = 0; i < lines.size(); i += moz ? 2 : 1) {
            const QString line = lines.at(i).trimmed();
            if (line.startsWith(QLatin1Char('#')))
                continue;
            const QUrl url(line);
            if (url.isLocalFile())
                add(url.toLocalFile());
        }
    }
    qCDebug(lcDrop) << "dropped files:" << paths << "formats:" << data->formats();
    return paths;
}

QStringList draggedLocalFiles(const QMimeData* data)
{
    QStringList paths;
    if (!data)
        return paths;
    auto addUrl = [&paths](const QUrl& url) {
        if (url.isLocalFile() && !paths.contains(url.toLocalFile()))
            paths.append(url.toLocalFile());
    };
    auto addLine = [&addUrl](const QString& line) {
        if (!line.startsWith(QLatin1Char('#')))
            addUrl(QUrl(line.trimmed()));
    };
    if (data->hasFormat(QLatin1String(kKdeUriListMime))) {
        const QString text = QString::fromUtf8(data->data(QLatin1String(kKdeUriListMime)));
        for (const QString& line :
            text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts))
            addLine(line);
    } else if (data->hasUrls() && !data->hasFormat(QLatin1String(kPortalFileTransferMime))) {
        for (const QUrl& url : data->urls())
            addUrl(url);
    }
    return paths;
}

} // namespace kk::platform
