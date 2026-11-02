// SPDX-License-Identifier: GPL-3.0-or-later
// PortalFileChooser implementation.
#include "platform/PortalFileChooser.h"

#include <QApplication>
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>
#include <QWidget>
#include <QWindow>

#include <memory>

#ifdef KK_HAVE_PORTAL_WINDOW_ID
#include <QtGui/private/qdesktopunixservices_p.h>
#include <QtGui/private/qguiapplication_p.h>
#include <qpa/qplatformintegration.h>
#endif

QDBusArgument& operator<<(QDBusArgument& arg, const kk::platform::portal::Filter& filter)
{
    arg.beginStructure();
    arg << filter.name;
    arg.beginArray(qMetaTypeId<QPair<uint, QString>>());
    for (const auto& [type, pattern] : filter.patterns) {
        arg.beginStructure();
        arg << type << pattern;
        arg.endStructure();
    }
    arg.endArray();
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, kk::platform::portal::Filter& filter)
{
    filter = {};
    arg.beginStructure();
    arg >> filter.name;
    arg.beginArray();
    while (!arg.atEnd()) {
        uint type = 0;
        QString pattern;
        arg.beginStructure();
        arg >> type >> pattern;
        arg.endStructure();
        filter.patterns.append({type, pattern});
    }
    arg.endArray();
    arg.endStructure();
    return arg;
}

namespace kk::platform::portal {

namespace {

constexpr int kVersionTimeoutMs = 10000; // first call may start xdg-desktop-portal
constexpr int kCallTimeoutMs = 25000; // the call itself only returns the request handle

/// Receives org.freedesktop.portal.Request.Response and ends the local event loop.
class ResponseReceiver : public QObject
{
    Q_OBJECT
public:
    QEventLoop loop;
    bool answered = false;
    uint response = 2;
    QVariantMap results;

public slots:
    void onResponse(uint code, const QVariantMap& values)
    {
        answered = true;
        response = code;
        results = values;
        loop.quit();
    }
};

/// Blocks input to the application's windows like a modal dialog does, without showing
/// anything (the portal's dialog is another process' window). Same trick Qt uses for native
/// dialogs: a modal widget that is "shown" with WA_DontShowOnScreen.
class ModalBlocker
{
public:
    ModalBlocker()
    {
        if (!qobject_cast<QApplication*>(QCoreApplication::instance()))
            return;
        m_widget = std::make_unique<QWidget>(nullptr, Qt::Window);
        m_widget->setAttribute(Qt::WA_DontShowOnScreen);
        m_widget->setWindowModality(Qt::ApplicationModal);
        m_widget->show();
    }
    ~ModalBlocker()
    {
        if (m_widget)
            m_widget->hide();
    }
    ModalBlocker(const ModalBlocker&) = delete;
    ModalBlocker& operator=(const ModalBlocker&) = delete;

private:
    std::unique_ptr<QWidget> m_widget;
};

QByteArray nulTerminated(const QString& path)
{
    QByteArray bytes = QFile::encodeName(path);
    bytes.append('\0');
    return bytes;
}

} // namespace

void registerTypes()
{
    static const bool done = [] {
        qDBusRegisterMetaType<QPair<uint, QString>>();
        qDBusRegisterMetaType<Filter>();
        qDBusRegisterMetaType<FilterList>();
        return true;
    }();
    Q_UNUSED(done);
}

QString caseInsensitiveGlob(const QString& glob)
{
    QString out;
    out.reserve(glob.size() * 4);
    bool inBracket = false;
    for (const QChar c : glob) {
        if (c == QLatin1Char('['))
            inBracket = true;
        else if (c == QLatin1Char(']'))
            inBracket = false;
        if (!inBracket && c.isLetter() && c.toLower() != c.toUpper()) {
            out += QLatin1Char('[') + c.toLower() + c.toUpper() + QLatin1Char(']');
        } else {
            out += c;
        }
    }
    return out;
}

FilterList parseQtFilters(const QString& qtFilter)
{
    // Same syntax QFileDialog accepts: entries separated by ";;" (or new lines), patterns in
    // the last "(...)" separated by spaces.
    static const QRegularExpression patternList(QStringLiteral("^(.*)\\(([^()]*)\\)\\s*$"));
    FilterList filters;
    const QStringList entries
        = qtFilter.split(QRegularExpression(QStringLiteral(";;|\\n")), Qt::SkipEmptyParts);
    for (const QString& rawEntry : entries) {
        const QString entry = rawEntry.trimmed();
        if (entry.isEmpty())
            continue;
        Filter filter;
        QString patterns = entry;
        if (const auto m = patternList.match(entry); m.hasMatch()) {
            filter.name = m.captured(1).trimmed();
            patterns = m.captured(2);
        }
        if (filter.name.isEmpty())
            filter.name = entry;
        for (const QString& p : patterns.split(QLatin1Char(' '), Qt::SkipEmptyParts))
            filter.patterns.append({0U, caseInsensitiveGlob(p)});
        if (!filter.patterns.isEmpty())
            filters.append(filter);
    }
    return filters;
}

QString methodFor(FileDialogRequest::Mode mode)
{
    return mode == FileDialogRequest::Mode::SaveFile ? QStringLiteral("SaveFile")
                                                     : QStringLiteral("OpenFile");
}

QVariantMap buildOptions(
    const FileDialogRequest& request, const QString& startDir, const QString& handleToken)
{
    using Mode = FileDialogRequest::Mode;
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), handleToken);
    options.insert(QStringLiteral("modal"), true);
    if (!startDir.isEmpty())
        options.insert(QStringLiteral("current_folder"), nulTerminated(startDir));
    switch (request.mode) {
    case Mode::OpenFile:
        options.insert(QStringLiteral("multiple"), false);
        break;
    case Mode::OpenFiles:
        options.insert(QStringLiteral("multiple"), true);
        break;
    case Mode::Directory:
        options.insert(QStringLiteral("multiple"), false);
        options.insert(QStringLiteral("directory"), true);
        break;
    case Mode::SaveFile: {
        options.insert(QStringLiteral("current_name"), request.fileName);
        const QString existing = QDir(startDir).filePath(request.fileName);
        if (!request.fileName.isEmpty() && QFileInfo(existing).isFile())
            options.insert(QStringLiteral("current_file"), nulTerminated(existing));
        break;
    }
    }
    if (request.mode != Mode::Directory) {
        const FilterList filters = parseQtFilters(request.filter);
        if (!filters.isEmpty()) {
            options.insert(QStringLiteral("filters"), QVariant::fromValue(filters));
            options.insert(QStringLiteral("current_filter"), QVariant::fromValue(filters.first()));
        }
    }
    return options;
}

QString requestPath(const QString& uniqueName, const QString& token)
{
    QString sender = uniqueName;
    if (sender.startsWith(QLatin1Char(':')))
        sender.remove(0, 1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
}

QStringList pathsFromUris(const QStringList& uris)
{
    QStringList paths;
    for (const QString& uri : uris) {
        const QUrl url(uri);
        if (url.isLocalFile())
            paths.append(QDir::cleanPath(url.toLocalFile()));
    }
    return paths;
}

bool isDocumentPortalPath(const QString& path)
{
    static const QRegularExpression doc(QStringLiteral("^/run/user/[0-9]+/doc(/|$)"));
    if (doc.match(path).hasMatch())
        return true;
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return !runtime.isEmpty()
        && (path == runtime + QStringLiteral("/doc")
            || path.startsWith(runtime + QStringLiteral("/doc/")));
}

QString parentWindowIdentifier(QWindow* window)
{
    if (!window)
        return {};
    const QString platform = QGuiApplication::platformName();
#ifdef KK_HAVE_PORTAL_WINDOW_ID
    // Qt's own platform services know how to export the window (xdg-foreign on Wayland).
    // Private API: used only with the exact Qt minor version we were compiled against (the
    // bundled Qt; a newer host Qt may have a different class layout).
    const QString built = QStringLiteral(QT_VERSION_STR).section(QLatin1Char('.'), 0, 1);
    const QString running = QString::fromLatin1(qVersion()).section(QLatin1Char('.'), 0, 1);
    if (built == running) {
        if (auto* integration = QGuiApplicationPrivate::platformIntegration()) {
            if (auto* services = dynamic_cast<QDesktopUnixServices*>(integration->services())) {
                // "wayland:" with an empty handle when the compositor has no xdg-foreign
                // (e.g. weston): no usable parent.
                const QString id = services->portalWindowIdentifier(window);
                if (!id.isEmpty() && !id.endsWith(QLatin1Char(':')))
                    return id;
            }
        }
    }
#endif
    if (platform == QLatin1String("xcb"))
        return QStringLiteral("x11:%1").arg(window->winId(), 0, 16);
    return {};
}

FileChooser::FileChooser(const QDBusConnection& bus, QString service)
    : m_bus(bus)
    , m_service(std::move(service))
{
    registerTypes();
}

uint FileChooser::version() const
{
    if (!m_bus.isConnected())
        return 0;
    QDBusConnectionInterface* bus = m_bus.interface();
    if (!bus)
        return 0;
    if (!bus->isServiceRegistered(m_service).value()) {
        const QDBusReply<QStringList> activatable
            = bus->call(QStringLiteral("ListActivatableNames"));
        if (!activatable.isValid() || !activatable.value().contains(m_service))
            return 0;
    }
    QDBusMessage get = QDBusMessage::createMethodCall(m_service, QLatin1String(kObjectPath),
        QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    get << QString::fromLatin1(kInterface) << QStringLiteral("version");
    const QDBusMessage reply = m_bus.call(get, QDBus::Block, kVersionTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return 0;
    return qvariant_cast<QDBusVariant>(reply.arguments().constFirst()).variant().toUInt();
}

FileChooser::Result FileChooser::run(
    const FileDialogRequest& request, const QString& startDir, const QString& parentWindow) const
{
    Result result;
    if (!m_bus.isConnected()) {
        result.error = QStringLiteral("no session bus");
        return result;
    }
    const QString token = QStringLiteral("kinoklip%1").arg(QRandomGenerator::global()->generate());
    const QVariantMap options = buildOptions(request, startDir, token);

    ResponseReceiver receiver;
    // Subscribe before calling (the answer may come before the call returns); portals older
    // than 0.9 use another handle path, re-subscribed below.
    QString handle = requestPath(m_bus.baseService(), token);
    QDBusConnection bus = m_bus;
    const auto subscribe = [&](const QString& path, bool on) {
        if (on)
            bus.connect(m_service, path, QLatin1String(kRequestInterface),
                QStringLiteral("Response"), &receiver, SLOT(onResponse(uint, QVariantMap)));
        else
            bus.disconnect(m_service, path, QLatin1String(kRequestInterface),
                QStringLiteral("Response"), &receiver, SLOT(onResponse(uint, QVariantMap)));
    };
    subscribe(handle, true);

    // The portal going away (crash) must not leave us waiting forever.
    bool portalGone = false;
    QDBusServiceWatcher watcher(m_service, bus, QDBusServiceWatcher::WatchForUnregistration);
    QObject::connect(&watcher, &QDBusServiceWatcher::serviceUnregistered, &receiver.loop, [&] {
        portalGone = true;
        receiver.loop.quit();
    });

    QDBusMessage call = QDBusMessage::createMethodCall(
        m_service, QLatin1String(kObjectPath), QLatin1String(kInterface), methodFor(request.mode));
    call << parentWindow << request.title << options;
    QDBusPendingCallWatcher pending(bus.asyncCall(call, kCallTimeoutMs));
    bool callFailed = false;
    QObject::connect(&pending, &QDBusPendingCallWatcher::finished, &receiver.loop,
        [&](QDBusPendingCallWatcher* w) {
            const QDBusPendingReply<QDBusObjectPath> reply = *w;
            if (reply.isError()) {
                callFailed = true;
                result.error = reply.error().message();
                receiver.loop.quit();
                return;
            }
            const QString actual = reply.value().path();
            if (actual != handle && !receiver.answered) {
                subscribe(handle, false);
                handle = actual;
                subscribe(handle, true);
            }
        });

    {
        const ModalBlocker blocker;
        if (!receiver.answered)
            receiver.loop.exec(QEventLoop::DialogExec);
    }
    subscribe(handle, false);

    if (callFailed)
        return result;
    if (portalGone && !receiver.answered) {
        result.error = QStringLiteral("the portal went away");
        return result;
    }
    result.ok = true;
    if (!receiver.answered) {
        // The application is quitting (QCoreApplication::exit ends every event loop): close
        // the dialog and report "cancelled", not a failure that would open Qt's dialog.
        QDBusMessage close = QDBusMessage::createMethodCall(
            m_service, handle, QLatin1String(kRequestInterface), QStringLiteral("Close"));
        bus.send(close);
        return result;
    }
    if (receiver.response == 0)
        result.paths = pathsFromUris(receiver.results.value(QStringLiteral("uris")).toStringList());
    return result;
}

} // namespace kk::platform::portal

#include "PortalFileChooser.moc"
