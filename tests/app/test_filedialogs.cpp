// SPDX-License-Identifier: GPL-3.0-or-later
// File dialog helper: backend choice, portal option building, and a full portal round trip
// against a fake org.freedesktop.portal.FileChooser on the session bus (skipped without one;
// ctest runs this test under dbus-run-session when available).
#include "platform/FileDialogs.h"
#include "platform/PortalFileChooser.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QtTest>

using namespace kk::platform;
using Mode = FileDialogRequest::Mode;

namespace {

constexpr char kFakeService[] = "org.kinoklip.TestPortal";

/// Minimal FileChooser portal: records each call and answers with a Response signal on the
/// request object, like xdg-desktop-portal does.
class FakePortal : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.FileChooser")
    Q_PROPERTY(uint version READ version)

public:
    explicit FakePortal(QDBusConnection bus)
        : m_bus(std::move(bus))
    { }

    uint version() const { return portalVersion; }

    uint portalVersion = 4;
    uint responseCode = 0;
    QStringList answerUris;
    QString lastMethod;
    QString lastParent;
    QString lastTitle;
    QVariantMap lastOptions;

public slots:
    QDBusObjectPath OpenFile(const QString& parent, const QString& title, const QVariantMap& o)
    {
        return answer(QStringLiteral("OpenFile"), parent, title, o);
    }
    QDBusObjectPath SaveFile(const QString& parent, const QString& title, const QVariantMap& o)
    {
        return answer(QStringLiteral("SaveFile"), parent, title, o);
    }

private:
    QDBusObjectPath answer(
        const QString& method, const QString& parent, const QString& title, const QVariantMap& o)
    {
        lastMethod = method;
        lastParent = parent;
        lastTitle = title;
        lastOptions = o;
        const QString handle
            = portal::requestPath(message().service(), o.value("handle_token").toString());
        // Answer after the method reply, as the real portal does once the user has chosen.
        QTimer::singleShot(50, this, [this, handle] {
            QDBusMessage signal = QDBusMessage::createSignal(handle,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"));
            QVariantMap results;
            results.insert(QStringLiteral("uris"), answerUris);
            signal << responseCode << results;
            m_bus.send(signal);
        });
        return QDBusObjectPath(handle);
    }

    QDBusConnection m_bus;
};

/// Unpacks a D-Bus a(sa(us)) that arrived as a QDBusArgument inside a QVariant.
portal::FilterList filtersOf(const QVariant& v)
{
    portal::FilterList list;
    if (v.canConvert<QDBusArgument>())
        qvariant_cast<QDBusArgument>(v) >> list;
    else
        list = v.value<portal::FilterList>();
    return list;
}

QString folderOf(const QVariant& v)
{
    QByteArray bytes = v.canConvert<QDBusArgument>() ? [&] {
        QByteArray b;
        qvariant_cast<QDBusArgument>(v) >> b;
        return b;
    }()
                                                     : v.toByteArray();
    if (bytes.endsWith('\0'))
        bytes.chop(1);
    return QString::fromUtf8(bytes);
}

} // namespace

class TestFileDialogs : public QObject
{
    Q_OBJECT

private:
    QDBusConnection m_serviceBus {QString()};
    FakePortal* m_fake = nullptr;
    QThread m_fakeThread; // the fake answers while the client blocks (as a separate process would)
    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-test"));
        QCoreApplication::setApplicationName(QStringLiteral("test_filedialogs"));
        QSettings().clear();
        portal::registerTypes();
        QVERIFY(m_dir.isValid());
    }

    void backendChoice()
    {
        QCOMPARE(chooseFileDialogBackend({}, 0, Mode::OpenFiles), FileDialogBackend::Qt);
        QCOMPARE(chooseFileDialogBackend({}, 1, Mode::OpenFiles), FileDialogBackend::Portal);
        QCOMPARE(chooseFileDialogBackend("auto", 4, Mode::SaveFile), FileDialogBackend::Portal);
        QCOMPARE(chooseFileDialogBackend("qt", 4, Mode::OpenFile), FileDialogBackend::Qt);
        QCOMPARE(chooseFileDialogBackend("QT", 4, Mode::OpenFile), FileDialogBackend::Qt);
        QCOMPARE(chooseFileDialogBackend("portal", 0, Mode::OpenFile), FileDialogBackend::Qt);
        // folder pickers need FileChooser version 3
        QCOMPARE(chooseFileDialogBackend({}, 2, Mode::Directory), FileDialogBackend::Qt);
        QCOMPARE(chooseFileDialogBackend({}, 3, Mode::Directory), FileDialogBackend::Portal);
    }

    void filters()
    {
        QCOMPARE(portal::caseInsensitiveGlob("*.mp4"), QStringLiteral("*.[mM][pP]4"));
        QCOMPARE(portal::caseInsensitiveGlob("*"), QStringLiteral("*"));
        QCOMPARE(portal::caseInsensitiveGlob("[ab].x"), QStringLiteral("[ab].[xX]"));
        const auto list = portal::parseQtFilters(
            "Kinoklip projects (*.kkproj);;Media (*.mp4 *.MOV);;All files (*)");
        QCOMPARE(list.size(), 3);
        QCOMPARE(list[0].name, QStringLiteral("Kinoklip projects"));
        QCOMPARE(list[0].patterns.size(), 1);
        QCOMPARE(list[0].patterns[0].first, 0U);
        QCOMPARE(list[0].patterns[0].second, QStringLiteral("*.[kK][kK][pP][rR][oO][jJ]"));
        QCOMPARE(list[1].patterns.size(), 2);
        QCOMPARE(list[1].patterns[1].second, QStringLiteral("*.[mM][oO][vV]"));
        QCOMPARE(list[2].patterns[0].second, QStringLiteral("*"));
        QVERIFY(portal::parseQtFilters(QString()).isEmpty());
    }

    void options()
    {
        FileDialogRequest r;
        r.mode = Mode::OpenFiles;
        r.filter = "Media (*.mp4)";
        QVariantMap o = portal::buildOptions(r, "/home/u/Videos", "tok");
        QCOMPARE(o.value("handle_token").toString(), QStringLiteral("tok"));
        QCOMPARE(o.value("multiple").toBool(), true);
        QVERIFY(!o.contains("directory"));
        QCOMPARE(o.value("current_folder").toByteArray(), QByteArray("/home/u/Videos\0", 15));
        QCOMPARE(o.value("filters").value<portal::FilterList>().size(), 1);
        QCOMPARE(o.value("current_filter").value<portal::Filter>().name, QStringLiteral("Media"));
        QCOMPARE(portal::methodFor(r.mode), QStringLiteral("OpenFile"));

        r.mode = Mode::Directory;
        o = portal::buildOptions(r, {}, "tok");
        QCOMPARE(o.value("directory").toBool(), true);
        QCOMPARE(o.value("multiple").toBool(), false);
        QVERIFY(!o.contains("filters"));
        QVERIFY(!o.contains("current_folder"));

        r.mode = Mode::SaveFile;
        r.fileName = "x.kkproj";
        o = portal::buildOptions(r, m_dir.path(), "tok");
        QCOMPARE(portal::methodFor(r.mode), QStringLiteral("SaveFile"));
        QCOMPARE(o.value("current_name").toString(), QStringLiteral("x.kkproj"));
        QVERIFY(!o.contains("current_file")); // does not exist yet
        QFile f(m_dir.filePath("x.kkproj"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        o = portal::buildOptions(r, m_dir.path(), "tok");
        QVERIFY(o.contains("current_file"));
    }

    void uris()
    {
        QCOMPARE(portal::requestPath(":1.42", "kk7"),
            QStringLiteral("/org/freedesktop/portal/desktop/request/1_42/kk7"));
        const QStringList paths = portal::pathsFromUris({"file:///home/u/a%20b.mp4",
            "file:///run/user/1000/doc/1a2b3c/clip.mov", "https://example.org/x.mp4"});
        QCOMPARE(paths, QStringList({"/home/u/a b.mp4", "/run/user/1000/doc/1a2b3c/clip.mov"}));
        QVERIFY(portal::isDocumentPortalPath("/run/user/1000/doc/1a2b3c"));
        QVERIFY(!portal::isDocumentPortalPath("/run/user/1000/docs"));
        QVERIFY(!portal::isDocumentPortalPath("/home/u/doc"));
    }

    void startDirectoryFallbacks()
    {
        FileDialogRequest r;
        QCOMPARE(startDirectory(r), QDir::homePath());
        r.settingsKey = "paths/testDir";
        QSettings().setValue(r.settingsKey, m_dir.path());
        QCOMPARE(startDirectory(r), m_dir.path());
        QSettings().setValue(r.settingsKey, "/does/not/exist");
        QCOMPARE(startDirectory(r), QDir::homePath());
        r.directory = "/explicit";
        QCOMPARE(startDirectory(r), QStringLiteral("/explicit"));
    }

    void qtOverrideSkipsBus()
    {
        qputenv("KINOKLIP_FILE_DIALOG", "qt");
        QCOMPARE(currentFileDialogBackend(Mode::OpenFiles), FileDialogBackend::Qt);
        qunsetenv("KINOKLIP_FILE_DIALOG");
    }

    // ---- Against a fake portal on the session bus ----

    void startFakePortal()
    {
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("no session bus (run under dbus-run-session)");
        m_serviceBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fakeportal");
        QVERIFY(m_serviceBus.isConnected());
        m_fake = new FakePortal(m_serviceBus);
        m_fakeThread.start();
        m_fake->moveToThread(&m_fakeThread);
        QVERIFY(m_serviceBus.registerObject(portal::kObjectPath, m_fake,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties));
        QVERIFY(m_serviceBus.registerService(kFakeService));
        qputenv("KINOKLIP_PORTAL_SERVICE", kFakeService);
        // never fall back to Qt's (modal, here endless) dialog if the portal call fails
        qputenv("KINOKLIP_FILE_DIALOG", "portal");
        QCOMPARE(portal::FileChooser(QDBusConnection::sessionBus(), kFakeService).version(), 4U);
        QCOMPARE(portal::FileChooser(QDBusConnection::sessionBus(), "org.kinoklip.NoSuchPortal")
                     .version(),
            0U);
        QCOMPARE(currentFileDialogBackend(Mode::OpenFiles), FileDialogBackend::Portal);
    }

    void importThroughPortal()
    {
        if (!m_fake)
            QSKIP("no fake portal");
        QCOMPARE(currentFileDialogBackend(Mode::Directory), FileDialogBackend::Portal);
        QSettings().setValue("paths/lastMediaDir", m_dir.path());
        m_fake->responseCode = 0;
        m_fake->answerUris = {QUrl::fromLocalFile(m_dir.filePath("sub/a.mp4")).toString(),
            QUrl::fromLocalFile(m_dir.filePath("sub/b.mov")).toString()};
        QWidget window;
        const QStringList paths = getOpenFileNames(
            &window, "Import Media", "Media (*.mp4 *.mov);;All files (*)", "paths/lastMediaDir");
        QCOMPARE(paths, QStringList({m_dir.filePath("sub/a.mp4"), m_dir.filePath("sub/b.mov")}));
        QCOMPARE(m_fake->lastMethod, QStringLiteral("OpenFile"));
        QCOMPARE(m_fake->lastTitle, QStringLiteral("Import Media"));
        QCOMPARE(m_fake->lastOptions.value("multiple").toBool(), true);
        QCOMPARE(m_fake->lastOptions.value("modal").toBool(), true);
        QCOMPARE(folderOf(m_fake->lastOptions.value("current_folder")), m_dir.path());
        const auto filters = filtersOf(m_fake->lastOptions.value("filters"));
        QCOMPARE(filters.size(), 2);
        QCOMPARE(filters[0].name, QStringLiteral("Media"));
        QCOMPARE(filters[0].patterns.size(), 2);
        QCOMPARE(filters[0].patterns[0].second, QStringLiteral("*.[mM][pP]4"));
        // remembered folder follows the choice
        QCOMPARE(QSettings().value("paths/lastMediaDir").toString(), m_dir.filePath("sub"));
    }

    void documentPortalPathsAreReturnedButNotRemembered()
    {
        if (!m_fake)
            QSKIP("no fake portal");
        QCOMPARE(currentFileDialogBackend(Mode::Directory), FileDialogBackend::Portal);
        QSettings().setValue("paths/lastKeymapDir", m_dir.path());
        m_fake->answerUris = {"file:///run/user/1000/doc/9f8e7d/profile.json"};
        const QString path = getOpenFileName(
            nullptr, "Import Keyboard Profile", "Keymap (*.json)", "paths/lastKeymapDir");
        QCOMPARE(path, QStringLiteral("/run/user/1000/doc/9f8e7d/profile.json"));
        QCOMPARE(m_fake->lastOptions.value("multiple").toBool(), false);
        QCOMPARE(QSettings().value("paths/lastKeymapDir").toString(), m_dir.path());
    }

    void saveAndFolderThroughPortal()
    {
        if (!m_fake)
            QSKIP("no fake portal");
        QCOMPARE(currentFileDialogBackend(Mode::Directory), FileDialogBackend::Portal);
        m_fake->answerUris = {QUrl::fromLocalFile(m_dir.filePath("out.kkproj")).toString()};
        const QString saved = getSaveFileName(nullptr, "Save Project As",
            "Kinoklip projects (*.kkproj)", QString(), "My Project.kkproj", m_dir.path());
        QCOMPARE(saved, m_dir.filePath("out.kkproj"));
        QCOMPARE(m_fake->lastMethod, QStringLiteral("SaveFile"));
        QCOMPARE(m_fake->lastOptions.value("current_name").toString(),
            QStringLiteral("My Project.kkproj"));

        m_fake->answerUris = {QUrl::fromLocalFile(m_dir.path()).toString()};
        const QString folder = getExistingDirectory(nullptr, "Render Location", "paths/testRender");
        QCOMPARE(folder, m_dir.path());
        QCOMPARE(m_fake->lastMethod, QStringLiteral("OpenFile"));
        QCOMPARE(m_fake->lastOptions.value("directory").toBool(), true);
        QCOMPARE(QSettings().value("paths/testRender").toString(), m_dir.path());
    }

    void cancelReturnsNothing()
    {
        if (!m_fake)
            QSKIP("no fake portal");
        QCOMPARE(currentFileDialogBackend(Mode::Directory), FileDialogBackend::Portal);
        m_fake->responseCode = 1; // user cancelled
        m_fake->answerUris = {};
        QVERIFY(getOpenFileNames(nullptr, "Import Media", "All files (*)", QString()).isEmpty());
        m_fake->responseCode = 0;
    }

    void cleanupTestCase()
    {
        QSettings().clear();
        if (m_fake) {
            m_serviceBus.unregisterService(kFakeService);
            m_serviceBus.unregisterObject(portal::kObjectPath);
            m_fakeThread.quit();
            m_fakeThread.wait();
            delete m_fake;
        }
    }
};

QTEST_MAIN(TestFileDialogs)
#include "test_filedialogs.moc"
