// SPDX-License-Identifier: GPL-3.0-or-later
// OpenUrl implementation.
#include "platform/OpenUrl.h"

#include "platform/ChildProcessEnv.h"
#include "platform/PortalFileChooser.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDesktopServices>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <fcntl.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcOpenUrl, "kk.openurl", QtInfoMsg)

namespace kk::platform {

namespace {

constexpr char kOrderVar[] = "KINOKLIP_URL_OPENERS";
constexpr char kServiceVar[] = "KINOKLIP_PORTAL_SERVICE";
constexpr char kOpenUriInterface[] = "org.freedesktop.portal.OpenURI";
/// The call itself (may include activating the portal service).
constexpr int kCallTimeoutMs = 5000;
/// How long we wait for the portal's Response after the call returned. Success arrives right
/// after the handler started; an app chooser keeps it open, which we then take as success.
constexpr int kResponseWaitMs = 1500;
/// Starts the helper ("$@"), reports its exit code if it ends within 1.5 s, else 0 (still
/// running: a browser in the foreground) and leaves it running.
constexpr char kHelperWatch[] = "\"$@\" </dev/null >/dev/null 2>&1 &\n"
                                "p=$!\n"
                                "i=0\n"
                                "while [ $i -lt 15 ]; do\n"
                                "  kill -0 $p 2>/dev/null || { wait $p; exit $?; }\n"
                                "  sleep 0.1\n"
                                "  i=$((i+1))\n"
                                "done\n"
                                "exit 0\n";
constexpr int kHelperStartMs = 2000;
constexpr int kHelperWaitMs = 4000;

QString portalService()
{
    const QString s = qEnvironmentVariable(kServiceVar);
    return s.isEmpty() ? QString::fromLatin1(portal::kService) : s;
}

bool serviceAvailable(const QDBusConnection& bus, const QString& service)
{
    QDBusConnectionInterface* iface = bus.interface();
    if (!iface)
        return false;
    if (iface->isServiceRegistered(service).value())
        return true;
    const QDBusReply<QStringList> activatable = iface->call(QStringLiteral("ListActivatableNames"));
    return activatable.isValid() && activatable.value().contains(service);
}

/// Receives org.freedesktop.portal.Request.Response.
class ResponseWaiter : public QObject
{
    Q_OBJECT
public:
    bool received = false;
    uint code = 0;
    QEventLoop loop;

public slots:
    void response(uint responseCode, const QVariantMap&)
    {
        received = true;
        code = responseCode;
        loop.quit();
    }
};

bool connectResponse(
    QDBusConnection& bus, const QString& service, const QString& path, ResponseWaiter* waiter)
{
    return bus.connect(service, path, QString::fromLatin1(portal::kRequestInterface),
        QStringLiteral("Response"), waiter, SLOT(response(uint, QVariantMap)));
}

UrlOpener openWithPortal(const QUrl& url)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCInfo(lcOpenUrl) << "portal: no session bus";
        return UrlOpener::None;
    }
    const QString service = portalService();
    if (!serviceAvailable(bus, service)) {
        qCInfo(lcOpenUrl).noquote() << "portal:" << service << "is not available";
        return UrlOpener::None;
    }

    static int counter = 0;
    const QString token = QStringLiteral("kinoklip_open_%1").arg(++counter);
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    const QString parentWindow = portal::parentWindowIdentifier(QGuiApplication::focusWindow());

    int fd = -1;
    QDBusMessage call;
    if (url.isLocalFile()) {
        fd = ::open(QFile::encodeName(url.toLocalFile()).constData(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            qCInfo(lcOpenUrl).noquote() << "portal: cannot open" << url.toLocalFile();
            return UrlOpener::None;
        }
        call = QDBusMessage::createMethodCall(service, QLatin1String(portal::kObjectPath),
            QLatin1String(kOpenUriInterface), QStringLiteral("OpenFile"));
        call << parentWindow << QVariant::fromValue(QDBusUnixFileDescriptor(fd)) << options;
    } else {
        call = QDBusMessage::createMethodCall(service, QLatin1String(portal::kObjectPath),
            QLatin1String(kOpenUriInterface), QStringLiteral("OpenURI"));
        call << parentWindow << url.toString(QUrl::FullyEncoded) << options;
    }

    // Subscribe before calling: the Response may come right after the reply.
    ResponseWaiter waiter;
    QString requestPath = portal::requestPath(bus.baseService(), token);
    connectResponse(bus, service, requestPath, &waiter);
    const QDBusMessage reply = bus.call(call, QDBus::Block, kCallTimeoutMs);
    if (fd >= 0)
        ::close(fd); // the message carried a duplicate
    if (reply.type() != QDBusMessage::ReplyMessage) {
        bus.disconnect(service, requestPath, QString::fromLatin1(portal::kRequestInterface),
            QStringLiteral("Response"), &waiter, SLOT(response(uint, QVariantMap)));
        qCInfo(lcOpenUrl).noquote() << "portal:" << reply.errorName() << reply.errorMessage();
        return UrlOpener::None;
    }
    // Old portals ignore handle_token: listen on the path they returned.
    const QString returned = reply.arguments().isEmpty()
        ? QString()
        : qvariant_cast<QDBusObjectPath>(reply.arguments().constFirst()).path();
    if (!returned.isEmpty() && returned != requestPath) {
        bus.disconnect(service, requestPath, QString::fromLatin1(portal::kRequestInterface),
            QStringLiteral("Response"), &waiter, SLOT(response(uint, QVariantMap)));
        requestPath = returned;
        connectResponse(bus, service, requestPath, &waiter);
    }
    if (!waiter.received) {
        QTimer::singleShot(kResponseWaitMs, &waiter.loop, &QEventLoop::quit);
        waiter.loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    bus.disconnect(service, requestPath, QString::fromLatin1(portal::kRequestInterface),
        QStringLiteral("Response"), &waiter, SLOT(response(uint, QVariantMap)));
    // 0 = opened, 1 = the user cancelled the app chooser (nothing to fall back to),
    // 2 = the portal could not open it (no handler, ...).
    if (waiter.received && waiter.code == 2) {
        qCInfo(lcOpenUrl) << "portal: the portal could not open the link (response 2)";
        return UrlOpener::None;
    }
    return UrlOpener::Portal;
}

UrlOpener openWithHelper(const QUrl& url)
{
    struct Candidate
    {
        const char* name;
        QStringList args;
    };
    const Candidate candidates[] = {
        {"xdg-open", {}},
        {"kde-open", {}},
        {"kde-open5", {}},
        {"gio", {QStringLiteral("open")}},
    };
    const QProcessEnvironment env = childEnvironment();
    const QStringList dirs
        = env.value(QStringLiteral("PATH")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    const QString target = url.isLocalFile() ? url.toLocalFile() : url.toString(QUrl::FullyEncoded);
    for (const Candidate& c : candidates) {
        const QString exe = dirs.isEmpty()
            ? QString()
            : QStandardPaths::findExecutable(QLatin1String(c.name), dirs);
        if (exe.isEmpty())
            continue;
        // Through a small shell so that the helper survives us (QProcess kills its child when
        // destroyed; xdg-open may run a browser in the foreground) and a quick failure (exit 3:
        // no browser configured, ...) is still seen. The shell ends within about 1.5 s.
        QProcess process;
        process.setProgram(QStringLiteral("/bin/sh"));
        process.setArguments(QStringList {QStringLiteral("-c"), QLatin1String(kHelperWatch),
                                 QStringLiteral("kinoklip-open"), exe}
            + c.args + QStringList {target});
        process.setProcessEnvironment(env);
        process.start();
        if (!process.waitForStarted(kHelperStartMs)) {
            qCInfo(lcOpenUrl).noquote() << "helper: cannot start" << exe << process.errorString();
            continue;
        }
        if (!process.waitForFinished(kHelperWaitMs)) // only if /bin/sh stalls: the helper runs
            return UrlOpener::Helper;
        if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
            return UrlOpener::Helper;
        qCInfo(lcOpenUrl).noquote()
            << "helper:" << exe << "failed with exit code" << process.exitCode();
    }
    qCInfo(lcOpenUrl) << "helper: no working xdg-open, kde-open or gio on PATH";
    return UrlOpener::None;
}

UrlOpener openWithQt(const QUrl& url)
{
    // On Wayland with a focus window Qt returns true before anything happened and may then do
    // nothing (see OpenUrl.h): that answer would hide the failure from the user.
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))
        && QGuiApplication::focusWindow()) {
        qCInfo(lcOpenUrl) << "qt: skipped, its Wayland launch is deferred and unverifiable";
        return UrlOpener::None;
    }
    const ScopedOriginalEnvironment original;
    return QDesktopServices::openUrl(url) ? UrlOpener::QtServices : UrlOpener::None;
}

} // namespace

QString urlOpenerName(UrlOpener opener)
{
    switch (opener) {
    case UrlOpener::Portal:
        return QStringLiteral("portal");
    case UrlOpener::Helper:
        return QStringLiteral("helper");
    case UrlOpener::QtServices:
        return QStringLiteral("qt");
    case UrlOpener::None:
        break;
    }
    return QStringLiteral("none");
}

QStringList urlOpenerOrder()
{
    static const QStringList known
        = {QStringLiteral("portal"), QStringLiteral("helper"), QStringLiteral("qt")};
    if (!qEnvironmentVariableIsSet(kOrderVar))
        return known;
    QStringList order;
    const QStringList names
        = qEnvironmentVariable(kOrderVar).split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& raw : names) {
        const QString name = raw.trimmed().toLower();
        if (known.contains(name) && !order.contains(name))
            order.append(name);
    }
    return order;
}

UrlOpener openUrlWith(const QUrl& url)
{
    if (!url.isValid() || url.isEmpty())
        return UrlOpener::None;
    const QString platform = QGuiApplication::platformName();
    if (!qEnvironmentVariableIsSet(kOrderVar)
        && (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal"))) {
        qCInfo(lcOpenUrl).noquote()
            << "not opening" << url.toDisplayString() << "on the" << platform << "platform";
        return UrlOpener::None;
    }
    const QStringList order = urlOpenerOrder();
    for (const QString& step : order) {
        UrlOpener result = UrlOpener::None;
        if (step == QLatin1String("portal"))
            result = openWithPortal(url);
        else if (step == QLatin1String("helper"))
            result = openWithHelper(url);
        else if (step == QLatin1String("qt"))
            result = openWithQt(url);
        if (result != UrlOpener::None) {
            qCDebug(lcOpenUrl).noquote()
                << "opened" << url.toDisplayString() << "with" << urlOpenerName(result);
            return result;
        }
    }
    qCWarning(lcOpenUrl).noquote() << "could not open" << url.toDisplayString()
                                   << "(tried:" << order.join(QStringLiteral(", ")) + u')';
    return UrlOpener::None;
}

bool openUrl(const QUrl& url)
{
    return openUrlWith(url) != UrlOpener::None;
}

} // namespace kk::platform

#include "OpenUrl.moc"
