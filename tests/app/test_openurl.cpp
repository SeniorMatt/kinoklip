// SPDX-License-Identifier: GPL-3.0-or-later
// platform::openUrl: the portal first (a fake org.freedesktop.portal.OpenURI on the session
// bus), then xdg-open / kde-open / gio stubs on PATH that record their arguments and
// environment (the bundle launcher's variables must be undone), then nothing; and the
// copy-link message of guide::openUrl when nothing opened. ctest runs this test under
// dbus-run-session (a private bus); the portal tests skip themselves without a session bus.
#include "platform/OpenUrl.h"
#include "platform/PortalFileChooser.h"
#include "ui/help/CopyLinkDialog.h"
#include "ui/help/Guide.h"

#include <QApplication>
#include <QClipboard>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>

#include <unistd.h>

using namespace kk;
using platform::UrlOpener;

namespace {

constexpr char kFakeService[] = "org.kinoklip.TestOpenURI";
constexpr char kMissingService[] = "org.kinoklip.NoSuchPortal";
const QUrl kLink(
    QStringLiteral("https://github.com/SeniorMatt/kinoklip/issues/"
                   "new?template=bug_report.yml&title=%5BBug%5D%20&environment=a%2Bb%20%26%20c"));

/// Minimal OpenURI portal: records each call and answers with a Response signal on the
/// request object, like xdg-desktop-portal does.
class FakeOpenUri : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.OpenURI")
    Q_PROPERTY(uint version READ version)

public:
    explicit FakeOpenUri(QDBusConnection bus)
        : m_bus(std::move(bus))
    { }
    uint version() const { return 4; }

    // set by the test (between calls; the fake runs on its own thread)
    uint responseCode = 0;
    bool replyError = false;
    // recorded
    int calls = 0;
    QString lastMethod;
    QString lastUri;
    QString lastFilePath;
    QVariantMap lastOptions;

public slots:
    QDBusObjectPath OpenURI(const QString&, const QString& uri, const QVariantMap& options)
    {
        lastUri = uri;
        return answer(QStringLiteral("OpenURI"), options);
    }
    QDBusObjectPath OpenFile(
        const QString&, const QDBusUnixFileDescriptor& fd, const QVariantMap& options)
    {
        // the path behind the descriptor, as the real portal resolves it
        lastFilePath = QFileInfo(QStringLiteral("/proc/self/fd/%1").arg(fd.fileDescriptor()))
                           .symLinkTarget();
        return answer(QStringLiteral("OpenFile"), options);
    }

private:
    QDBusObjectPath answer(const QString& method, const QVariantMap& options)
    {
        ++calls;
        lastMethod = method;
        lastOptions = options;
        if (replyError) {
            sendErrorReply(QDBusError::AccessDenied, QStringLiteral("not allowed"));
            return {};
        }
        const QString handle = platform::portal::requestPath(
            message().service(), options.value(QStringLiteral("handle_token")).toString());
        QTimer::singleShot(30, this, [this, handle] {
            QDBusMessage signal = QDBusMessage::createSignal(handle,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"));
            signal << responseCode << QVariantMap();
            m_bus.send(signal);
        });
        return QDBusObjectPath(handle);
    }

    QDBusConnection m_bus;
};

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TestOpenUrl : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_bin; ///< stub helpers
    QString m_logs;
    QByteArray m_path;
    QDBusConnection m_serviceBus {QString()};
    FakeOpenUri* m_fake = nullptr;
    QThread m_fakeThread;

    /// A helper stub that writes "<name>", its arguments ("arg=...") and its environment to
    /// <logs>/<name>.log (atomically: written then renamed; env lists the stub's own PATH).
    void writeStub(const QString& name)
    {
        const QString script = m_bin + u'/' + name;
        QFile f(script);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QString log = m_logs + u'/' + name + QStringLiteral(".log");
        // A second run after "<name>.fail" exists: exits 3 like xdg-open without a browser.
        f.write(QStringLiteral(
            "#!/bin/sh\nexport PATH=/usr/bin:/bin\n[ -e \"%2.fail\" ] && { : > \"%2.tried\"; exit "
            "3; }\n"
            "{ echo \"%1\"; for a in \"$@\"; do echo \"arg=$a\"; done; env; } > \"%2.tmp\" "
            "&& mv \"%2.tmp\" \"%2\"\n")
                    .arg(name, log)
                    .toUtf8());
        f.close();
        QVERIFY(f.setPermissions(f.permissions() | QFile::ExeOwner));
    }
    QString logOf(const QString& name) const
    {
        return m_logs + u'/' + name + QStringLiteral(".log");
    }
    void clearLogs()
    {
        for (const char* n : {"xdg-open", "kde-open", "kde-open5", "gio"})
            QFile::remove(logOf(QLatin1String(n)));
    }
    /// Waits for the stub's log; returns its lines.
    QStringList waitForLog(const QString& name)
    {
        const QString log = logOf(name);
        if (!QTest::qWaitFor([&] { return QFile::exists(log); }, 5000))
            return {};
        return QString::fromUtf8(readAll(log)).split(u'\n', Qt::SkipEmptyParts);
    }
    void setPortal(const char* service) { qputenv("KINOKLIP_PORTAL_SERVICE", service); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_bin = m_dir.filePath(QStringLiteral("bin"));
        m_logs = m_dir.filePath(QStringLiteral("logs"));
        QVERIFY(QDir().mkpath(m_bin));
        QVERIFY(QDir().mkpath(m_logs));
        for (const char* n : {"xdg-open", "kde-open", "kde-open5", "gio"})
            writeStub(QLatin1String(n));
        m_path = qgetenv("PATH");
        qputenv("PATH", m_bin.toLocal8Bit() + ':' + m_path);

        // What packaging/linux/apprun.sh leaves in the app's environment.
        qputenv("KINOKLIP_ORIG_VARS",
            "LD_LIBRARY_PATH QT_PLUGIN_PATH XDG_DATA_DIRS QT_QPA_PLATFORMTHEME");
        qputenv("KINOKLIP_ORIG_XDG_DATA_DIRS", "/usr/local/share:/usr/share");
        qputenv("KINOKLIP_ORIG_QT_QPA_PLATFORMTHEME", "kde");
        qunsetenv("KINOKLIP_ORIG_LD_LIBRARY_PATH");
        qunsetenv("KINOKLIP_ORIG_QT_PLUGIN_PATH");
        qputenv("LD_LIBRARY_PATH", "/bundle/usr/lib/qt");
        qputenv("QT_PLUGIN_PATH", "/bundle/usr/lib/qt/plugins");
        qputenv("XDG_DATA_DIRS", "/bundle/usr/share:/usr/local/share:/usr/share");
        qputenv("QT_QPA_PLATFORMTHEME", "generic");
        qputenv("KINOKLIP_APPDIR", "/bundle");

        if (QDBusConnection::sessionBus().isConnected()) {
            m_serviceBus
                = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fakeopenuri");
            QVERIFY(m_serviceBus.isConnected());
            m_fake = new FakeOpenUri(m_serviceBus);
            m_fakeThread.start();
            m_fake->moveToThread(&m_fakeThread);
            QVERIFY(m_serviceBus.registerObject(platform::portal::kObjectPath, m_fake,
                QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
            QVERIFY(m_serviceBus.registerService(kFakeService));
        }
    }

    void init()
    {
        clearLogs();
        // the default order, spelled out: offscreen opens nothing unless it is set
        qputenv("KINOKLIP_URL_OPENERS", "portal,helper,qt");
        setPortal(kMissingService);
        if (m_fake) {
            m_fake->responseCode = 0;
            m_fake->replyError = false;
            m_fake->calls = 0;
        }
    }

    void defaultOrderIsPortalHelperQt()
    {
        qunsetenv("KINOKLIP_URL_OPENERS");
        QCOMPARE(platform::urlOpenerOrder(),
            QStringList(
                {QStringLiteral("portal"), QStringLiteral("helper"), QStringLiteral("qt")}));
        qputenv("KINOKLIP_URL_OPENERS", "Helper, bogus,portal,helper");
        QCOMPARE(platform::urlOpenerOrder(),
            QStringList({QStringLiteral("helper"), QStringLiteral("portal")}));
        qputenv("KINOKLIP_URL_OPENERS", "none");
        QVERIFY(platform::urlOpenerOrder().isEmpty());
        QCOMPARE(platform::openUrlWith(kLink), UrlOpener::None);
    }

    void offscreenOpensNothingByDefault()
    {
        qunsetenv("KINOKLIP_URL_OPENERS");
        QCOMPARE(platform::openUrlWith(kLink), UrlOpener::None);
        QTest::qWait(300);
        QVERIFY(!QFile::exists(logOf(QStringLiteral("xdg-open"))));
    }

    void portalOpensLinks()
    {
        if (!m_fake)
            QSKIP("no session bus (run under dbus-run-session)");
        setPortal(kFakeService);
        QCOMPARE(platform::openUrlWith(kLink), UrlOpener::Portal);
        QCOMPARE(m_fake->lastMethod, QStringLiteral("OpenURI"));
        QCOMPARE(m_fake->lastUri, kLink.toString(QUrl::FullyEncoded));
        QVERIFY(m_fake->lastOptions.value(QStringLiteral("handle_token"))
                    .toString()
                    .startsWith(QStringLiteral("kinoklip_open_")));
        QTest::qWait(300);
        QVERIFY(!QFile::exists(logOf(QStringLiteral("xdg-open")))); // no second window
    }

    void portalOpensLocalFilesByDescriptor()
    {
        if (!m_fake)
            QSKIP("no session bus (run under dbus-run-session)");
        setPortal(kFakeService);
        const QString file = m_dir.filePath(QStringLiteral("clip one.mp4"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        QCOMPARE(platform::openUrlWith(QUrl::fromLocalFile(file)), UrlOpener::Portal);
        QCOMPARE(m_fake->lastMethod, QStringLiteral("OpenFile"));
        QCOMPARE(m_fake->lastFilePath, QFileInfo(file).canonicalFilePath());
        // folders too (Open Folder, Reveal)
        QCOMPARE(platform::openUrlWith(QUrl::fromLocalFile(m_dir.path())), UrlOpener::Portal);
        QCOMPARE(m_fake->lastFilePath, QFileInfo(m_dir.path()).canonicalFilePath());
    }

    void portalFailureFallsBackToTheHelper_data()
    {
        QTest::addColumn<bool>("replyError");
        QTest::newRow("response 2 (no handler)") << false;
        QTest::newRow("D-Bus error") << true;
    }
    void portalFailureFallsBackToTheHelper()
    {
        if (!m_fake)
            QSKIP("no session bus (run under dbus-run-session)");
        QFETCH(bool, replyError);
        setPortal(kFakeService);
        m_fake->responseCode = 2;
        m_fake->replyError = replyError;
        QCOMPARE(platform::openUrlWith(kLink), UrlOpener::Helper);
        QCOMPARE(m_fake->calls, 1);
        const QStringList log = waitForLog(QStringLiteral("xdg-open"));
        QVERIFY(log.contains(QStringLiteral("arg=") + kLink.toString(QUrl::FullyEncoded)));
    }

    void cancelledChooserIsNotRetried()
    {
        if (!m_fake)
            QSKIP("no session bus (run under dbus-run-session)");
        setPortal(kFakeService);
        m_fake->responseCode = 1; // the user closed the app chooser
        QCOMPARE(platform::openUrlWith(kLink), UrlOpener::Portal);
        QTest::qWait(300);
        QVERIFY(!QFile::exists(logOf(QStringLiteral("xdg-open"))));
    }

    void helperGetsTheOriginalEnvironmentAndTheUrl()
    {
        // no portal on the bus: xdg-open from PATH, started with the user's environment
        QCOMPARE(platform::openUrlWith(kLink), UrlOpener::Helper);
        const QStringList log = waitForLog(QStringLiteral("xdg-open"));
        QVERIFY(!log.isEmpty());
        QCOMPARE(log.first(), QStringLiteral("xdg-open"));
        QCOMPARE(log.filter(QStringLiteral("arg=")),
            QStringList {QStringLiteral("arg=") + kLink.toString(QUrl::FullyEncoded)});
        QVERIFY(log.filter(QRegularExpression(QStringLiteral("^LD_LIBRARY_PATH="))).isEmpty());
        QVERIFY(log.filter(QRegularExpression(QStringLiteral("^QT_PLUGIN_PATH="))).isEmpty());
        QVERIFY(log.contains(QStringLiteral("XDG_DATA_DIRS=/usr/local/share:/usr/share")));
        QVERIFY(log.contains(QStringLiteral("QT_QPA_PLATFORMTHEME=kde")));
        QVERIFY(log.filter(QRegularExpression(QStringLiteral("^KINOKLIP_"))).isEmpty());
        // our own environment keeps the bundle's values
        QCOMPARE(qgetenv("LD_LIBRARY_PATH"), QByteArray("/bundle/usr/lib/qt"));
    }

    void helperLocalFilesArePaths()
    {
        QCOMPARE(platform::openUrlWith(QUrl::fromLocalFile(m_dir.path())), UrlOpener::Helper);
        const QStringList log = waitForLog(QStringLiteral("xdg-open"));
        QVERIFY(log.contains(QStringLiteral("arg=") + m_dir.path()));
    }

    void helpersAreTriedInOrder()
    {
        const QString hidden = m_dir.filePath(QStringLiteral("hidden"));
        QVERIFY(QDir().mkpath(hidden));
        auto hide = [&](const char* n) {
            QVERIFY(
                QFile::rename(m_bin + u'/' + QLatin1String(n), hidden + u'/' + QLatin1String(n)));
        };
        auto unhide = [&](const char* n) {
            QFile::rename(hidden + u'/' + QLatin1String(n), m_bin + u'/' + QLatin1String(n));
        };
        // The host may have real helpers: only our stub directory is on PATH here.
        qputenv("PATH", m_bin.toLocal8Bit());

        hide("xdg-open");
        QCOMPARE(platform::openUrlWith(kLink), UrlOpener::Helper);
        QVERIFY(!waitForLog(QStringLiteral("kde-open")).isEmpty());
        hide("kde-open");
        hide("kde-open5");
        QCOMPARE(platform::openUrlWith(kLink), UrlOpener::Helper);
        const QStringList gio = waitForLog(QStringLiteral("gio"));
        QCOMPARE(gio.filter(QStringLiteral("arg=")),
            QStringList({QStringLiteral("arg=open"),
                QStringLiteral("arg=") + kLink.toString(QUrl::FullyEncoded)}));
        hide("gio");
        // nothing left (no portal, no helper; Qt's step is left out: offscreen it is a no-op)
        qputenv("KINOKLIP_URL_OPENERS", "portal,helper");
        QCOMPARE(platform::openUrlWith(kLink), UrlOpener::None);
        QVERIFY(!platform::openUrl(kLink));

        for (const char* n : {"xdg-open", "kde-open", "kde-open5", "gio"})
            unhide(n);
        qputenv("PATH", m_bin.toLocal8Bit() + ':' + m_path);
    }

    void helperThatFailsTriesTheNext()
    {
        // xdg-open exists but finds no browser (exit 3): not "opened"
        QFile mark(logOf(QStringLiteral("xdg-open")) + QStringLiteral(".fail"));
        QVERIFY(mark.open(QIODevice::WriteOnly));
        mark.close();
        QCOMPARE(platform::openUrlWith(kLink), UrlOpener::Helper);
        QVERIFY(QFile::exists(logOf(QStringLiteral("xdg-open")) + QStringLiteral(".tried")));
        QVERIFY(!waitForLog(QStringLiteral("kde-open")).isEmpty());
        // every helper fails: nothing opened, so the caller can show the link
        for (const char* n : {"kde-open", "kde-open5", "gio"}) {
            QFile other(logOf(QLatin1String(n)) + QStringLiteral(".fail"));
            QVERIFY(other.open(QIODevice::WriteOnly));
        }
        qputenv("PATH", m_bin.toLocal8Bit() + ":/usr/bin:/bin");
        qputenv("KINOKLIP_URL_OPENERS", "helper");
        const UrlOpener all = platform::openUrlWith(kLink);
        for (const char* n : {"xdg-open", "kde-open", "kde-open5", "gio"})
            QFile::remove(logOf(QLatin1String(n)) + QStringLiteral(".fail"));
        qputenv("PATH", m_bin.toLocal8Bit() + ':' + m_path);
        QCOMPARE(all, UrlOpener::None);
    }

    void aSlowHelperCountsAsOpenedAndSurvives()
    {
        // a browser in the foreground: still running after the watch, not killed
        const QString script = m_bin + QStringLiteral("/xdg-open");
        const QByteArray original = readAll(script);
        const QString done = m_logs + QStringLiteral("/slow.done");
        QFile f(script);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QStringLiteral("#!/bin/sh\n/bin/sleep 4\n: > \"%1\"\n").arg(done).toUtf8());
        f.close();
        qputenv("KINOKLIP_URL_OPENERS", "helper");
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(platform::openUrlWith(kLink), UrlOpener::Helper);
        QVERIFY(timer.elapsed() < 3800); // returned before the helper ended
        QVERIFY(QTest::qWaitFor([&] { return QFile::exists(done); }, 8000)); // it was not killed
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(original);
        f.close();
    }

    void failureShowsTheLinkToCopy()
    {
        qputenv("KINOKLIP_URL_OPENERS", "none");
        QWidget window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(!guide::openUrl(kLink, &window));
        auto* dialog = window.findChild<guide::CopyLinkDialog*>();
        QVERIFY(dialog);
        QVERIFY(dialog->isVisible());
        QCOMPARE(dialog->linkText(), kLink.toString(QUrl::FullyEncoded));
        auto* field = dialog->findChild<QLineEdit*>(QStringLiteral("CopyLinkField"));
        QVERIFY(field);
        QVERIFY(field->isReadOnly());
        QCOMPARE(field->selectedText(), field->text());
        auto* copy = dialog->findChild<QPushButton*>(QStringLiteral("CopyLinkButton"));
        QVERIFY(copy);
        copy->click();
        QCOMPARE(QApplication::clipboard()->text(), kLink.toString(QUrl::FullyEncoded));
        dialog->close();

        // success shows nothing
        guide::setUrlOpenerForTesting([](const QUrl&) { return true; });
        QVERIFY(guide::openUrl(kLink, &window));
        guide::setUrlOpenerForTesting({});
        QTest::qWait(50); // the closed dialog's deleteLater
        QVERIFY(!window.findChild<guide::CopyLinkDialog*>());
    }

    void cleanupTestCase()
    {
        qputenv("PATH", m_path);
        if (m_fake) {
            m_serviceBus.unregisterService(kFakeService);
            m_serviceBus.unregisterObject(platform::portal::kObjectPath);
            m_fakeThread.quit();
            m_fakeThread.wait();
            delete m_fake;
        }
    }
};

QTEST_MAIN(TestOpenUrl)
#include "test_openurl.moc"
