// SPDX-License-Identifier: GPL-3.0-or-later
// "Render finished" desktop notification (spec 45.5, 45.6): what a finished
// render queue run reports (a fake Notifier on a real RenderQueue), and the D-Bus calls of the
// freedesktop and portal notifiers against fake services on the session bus. ctest runs this
// test under dbus-run-session (a private bus); the fakes use their own service names, so even
// a run on the desktop's bus never shows a notification.
#include "ExportTestUtil.h"
#include "core/Preferences.h"
#include "export/ExportPresets.h"
#include "export/RenderQueue.h"
#include "platform/DesktopNotifications.h"
#include "ui/deliver/RenderFinishNotifier.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <QtTest>

using namespace kk;
using namespace kk::test;
using platform::DesktopNotification;

namespace {

constexpr char kFakeService[] = "org.kinoklip.TestNotifications";
constexpr char kFakePortal[] = "org.kinoklip.TestNotificationPortal";

/// Records what would be shown; the handler of the last notification can be "clicked".
class FakeNotifier final : public platform::Notifier
{
public:
    bool show(const DesktopNotification& n, ActionHandler onAction) override
    {
        shown.append(n);
        handler = std::move(onAction);
        return true;
    }
    bool supportsActions() override { return true; }

    QList<DesktopNotification> shown;
    ActionHandler handler;
};

/// org.freedesktop.Notifications as Plasma / dunst offer it.
class FakeNotificationServer : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
    QStringList capabilities {QStringLiteral("actions"), QStringLiteral("body")};
    QString signature;
    QVariantList arguments;
    int calls = 0;

public Q_SLOTS:
    QStringList GetCapabilities() { return capabilities; }
    uint Notify(const QString&, uint, const QString&, const QString&, const QString&,
        const QStringList&, const QVariantMap&, int)
    {
        signature = message().signature();
        arguments = message().arguments();
        ++calls;
        return 7;
    }
};

/// org.freedesktop.portal.Notification.
class FakePortal : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Notification")
public:
    QString signature;
    QString id;
    QVariantMap notification;
    int calls = 0;

public Q_SLOTS:
    void AddNotification(const QString& notificationId, const QVariantMap& n)
    {
        signature = message().signature();
        id = notificationId;
        notification = n;
        ++calls;
    }
};

QList<QVariantMap> buttonsOf(const QVariant& v)
{
    QList<QVariantMap> list;
    if (v.canConvert<QDBusArgument>())
        qvariant_cast<QDBusArgument>(v) >> list;
    else
        list = v.value<QList<QVariantMap>>();
    return list;
}

} // namespace

class TestRenderNotify : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    std::unique_ptr<Project> m_project;
    QString m_skip;
    QDBusConnection m_serviceBus {QString()};
    QThread m_serviceThread; // the fakes answer while the client blocks (GetCapabilities)
    FakeNotificationServer* m_server = nullptr;
    FakePortal* m_portal = nullptr;

    ExportSettings wavJob(const QString& file)
    {
        ExportSettings s;
        if (!ExportPresets().apply(QStringLiteral("audio_wav"), s))
            qFatal("no audio_wav preset");
        s.outputPath = file;
        return s;
    }

    quint64 add(RenderQueue& queue, const QString& name, const QString& path)
    {
        return queue.addJob(name, wavJob(path), m_project->timeline()->snapshot(),
            makeMediaPathMap(m_project->mediaPool()->state()));
    }

    /// A path whose folder cannot be created (its parent is a file): the job fails.
    QString unwritablePath()
    {
        const QString file = m_dir.filePath(QStringLiteral("a_file"));
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly))
            qFatal("cannot create %s", qPrintable(file));
        return file + QStringLiteral("/sub/out.wav");
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-test"));
        QCoreApplication::setApplicationName(QStringLiteral("test_rendernotify"));
        QSettings().clear();
        QVERIFY(m_dir.isValid());
        m_project = std::make_unique<Project>();
        buildExportProject(*m_project, &m_skip);
    }

    void cleanupTestCase()
    {
        if (m_server || m_portal) {
            m_serviceBus.unregisterService(kFakeService);
            m_serviceBus.unregisterService(kFakePortal);
            m_serviceThread.quit();
            m_serviceThread.wait();
            delete m_server;
            delete m_portal;
        }
        QSettings().clear();
    }

    // ---- Texts ----

    void summaryTexts()
    {
        using Status = RenderQueue::Status;
        auto job = [](const QString& name, Status status, double seconds, const QString& out) {
            RenderQueue::Job j;
            j.name = name;
            j.status = status;
            j.result.ok = status == Status::Done;
            j.result.cancelled = status == Status::Cancelled;
            j.result.seconds = seconds;
            j.result.outputPath = out;
            if (j.result.ok)
                j.result.files = {out};
            if (status == Status::Failed)
                j.result.error = QStringLiteral("disk full");
            return j;
        };
        auto s
            = RenderFinishNotifier::summarize({job("Timeline 1", Status::Done, 42.4, "/v/a.mp4")});
        QVERIFY(s);
        QCOMPARE(s->summary, QStringLiteral("Render complete"));
        QCOMPARE(s->body, QStringLiteral("Timeline 1 rendered in 0:42"));
        QCOMPARE(s->revealPath, QStringLiteral("/v/a.mp4"));
        QCOMPARE(s->playPath, QStringLiteral("/v/a.mp4"));

        s = RenderFinishNotifier::summarize({job("A", Status::Done, 60, "/v/a.mp4"),
            job("B", Status::Done, 3600, "/v/b.mp4"), job("C", Status::Done, 5, "/v/c.mp4")});
        QCOMPARE(s->summary, QStringLiteral("Render complete"));
        QCOMPARE(s->body, QStringLiteral("3 jobs rendered in 1:01:05"));
        QCOMPARE(s->revealPath, QStringLiteral("/v/c.mp4"));
        QVERIFY(s->playPath.isEmpty()); // Play only for a single file

        s = RenderFinishNotifier::summarize({job("A", Status::Done, 1, "/v/a.mp4"),
            job("B", Status::Failed, 1, "/v/b.mp4"), job("C", Status::Done, 1, "/v/c.mp4")});
        QCOMPARE(s->summary, QStringLiteral("Render finished with errors"));
        QCOMPARE(s->body, QStringLiteral("1 of 3 failed\ndisk full"));
        QCOMPARE(s->revealPath, QStringLiteral("/v/c.mp4"));

        s = RenderFinishNotifier::summarize({job("A", Status::Cancelled, 1, "/v/a.mp4"),
            job("B", Status::Failed, 1, "/v/b.mp4"), job("C", Status::Failed, 1, "/v/c.mp4")});
        QCOMPARE(s->body, QStringLiteral("2 of 3 failed, 1 cancelled"));
        QVERIFY(s->revealPath.isEmpty()); // nothing rendered: no Open Folder

        s = RenderFinishNotifier::summarize(
            {job("A", Status::Cancelled, 1, "/v/a.mp4"), job("B", Status::Done, 1, "/v/b.mp4")});
        QCOMPARE(s->body, QStringLiteral("1 of 2 cancelled"));

        // The user stopped the run (the last job was cancelled), or nothing ran: no notice.
        QVERIFY(!RenderFinishNotifier::summarize(
            {job("A", Status::Done, 1, "/v/a.mp4"), job("B", Status::Cancelled, 1, "/v/b.mp4")}));
        QVERIFY(!RenderFinishNotifier::summarize({}));
        QCOMPARE(RenderFinishNotifier::formatDuration(0.4), QStringLiteral("0:00"));
        QCOMPARE(RenderFinishNotifier::formatDuration(725), QStringLiteral("12:05"));
    }

    // ---- A real render queue with a fake notifier ----

    void finishedQueueSendsOneNotification()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        RenderQueue queue;
        QWidget window;
        RenderFinishNotifier watcher(&queue, &window);
        FakeNotifier fake;
        watcher.setNotifier(&fake);
        QStringList revealed;
        QStringList opened;
        watcher.setHandlers(
            [&](const QString& p) { revealed << p; }, [&](const QString& p) { opened << p; });
        const QString out = m_dir.filePath(QStringLiteral("one.wav"));
        add(queue, QStringLiteral("Timeline 1"), out);
        queue.startAll();
        QVERIFY(queue.waitForIdle(120000));
        QCOMPARE(fake.shown.size(), 1);
        QCOMPARE(fake.shown[0].summary, QStringLiteral("Render complete"));
        QVERIFY2(fake.shown[0].body.startsWith(QStringLiteral("Timeline 1 rendered in 0:0")),
            qPrintable(fake.shown[0].body));
        QCOMPARE(fake.shown[0].actions.size(), 2);
        QCOMPARE(fake.shown[0].actions[0].label, QStringLiteral("Open Folder"));
        QCOMPARE(fake.shown[0].actions[1].label, QStringLiteral("Play"));

        // Open Folder goes to the file-manager helper with the rendered file; Play opens it.
        fake.handler(QLatin1String(RenderFinishNotifier::kOpenFolderAction));
        QCOMPARE(revealed, QStringList {out});
        QVERIFY(opened.isEmpty());
        fake.handler(QLatin1String(RenderFinishNotifier::kPlayAction));
        QCOMPARE(opened, QStringList {out});

        // Render All of three jobs: still one notification, for the whole run.
        queue.clearFinished();
        for (int i = 0; i < 3; ++i)
            add(queue, QStringLiteral("Job %1").arg(i),
                m_dir.filePath(QStringLiteral("j%1.wav").arg(i)));
        queue.startAll();
        QVERIFY(queue.waitForIdle(120000));
        QCOMPARE(fake.shown.size(), 2);
        QVERIFY2(fake.shown[1].body.startsWith(QStringLiteral("3 jobs rendered in")),
            qPrintable(fake.shown[1].body));
        QCOMPARE(fake.shown[1].actions.size(), 1); // Open Folder, no Play for three files

        // One job started on its own (the card's render button) is a run of its own.
        queue.clearFinished();
        const quint64 single = add(queue, QStringLiteral("Single"), m_dir.filePath("s.wav"));
        add(queue, QStringLiteral("Left queued"), m_dir.filePath("q.wav"));
        queue.start(single);
        QVERIFY(queue.waitForIdle(120000));
        QCOMPARE(fake.shown.size(), 3);
        QVERIFY(fake.shown[2].body.startsWith(QStringLiteral("Single rendered in")));
    }

    void failuresChangeTheText()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        RenderQueue queue;
        RenderFinishNotifier watcher(&queue, nullptr);
        FakeNotifier fake;
        watcher.setNotifier(&fake);
        add(queue, QStringLiteral("A"), m_dir.filePath(QStringLiteral("fa.wav")));
        add(queue, QStringLiteral("B"), unwritablePath());
        add(queue, QStringLiteral("C"), m_dir.filePath(QStringLiteral("fc.wav")));
        queue.startAll();
        QVERIFY(queue.waitForIdle(120000));
        QCOMPARE(fake.shown.size(), 1);
        QCOMPARE(fake.shown[0].summary, QStringLiteral("Render finished with errors"));
        QVERIFY2(fake.shown[0].body.startsWith(QStringLiteral("1 of 3 failed")),
            qPrintable(fake.shown[0].body));
    }

    void userCancelSendsNone()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        // The short WAV job can finish before the cancel reaches it; try a few times.
        bool cancelled = false;
        for (int attempt = 0; attempt < 10 && !cancelled; ++attempt) {
            RenderQueue queue;
            RenderFinishNotifier watcher(&queue, nullptr);
            FakeNotifier fake;
            watcher.setNotifier(&fake);
            add(queue, QStringLiteral("A"),
                m_dir.filePath(QStringLiteral("ca%1.wav").arg(attempt)));
            add(queue, QStringLiteral("B"),
                m_dir.filePath(QStringLiteral("cb%1.wav").arg(attempt)));
            queue.startAll();
            queue.stop(); // Stop while the first job renders
            QVERIFY(queue.waitForIdle(120000));
            if (queue.jobs().first().status != RenderQueue::Status::Cancelled)
                continue;
            cancelled = true;
            QCOMPARE(queue.jobs().last().status, RenderQueue::Status::Queued);
            QCOMPARE(fake.shown.size(), 0);
        }
        QVERIFY2(cancelled, "the render always finished before Stop");
    }

    void preferenceOffSendsNone()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        QVERIFY(prefs::renderNotificationsEnabled()); // on by default
        prefs::setRenderNotificationsEnabled(false);
        RenderQueue queue;
        RenderFinishNotifier watcher(&queue, nullptr);
        FakeNotifier fake;
        watcher.setNotifier(&fake);
        add(queue, QStringLiteral("A"), m_dir.filePath(QStringLiteral("pa.wav")));
        queue.startAll();
        QVERIFY(queue.waitForIdle(120000));
        prefs::setRenderNotificationsEnabled(true);
        QCOMPARE(fake.shown.size(), 0);
    }

    void preferencesCheckbox()
    {
        {
            PreferencesDialog dialog(nullptr);
            QVERIFY(dialog.showSection(QStringLiteral("ui")));
            auto* box = dialog.findChild<QCheckBox*>(QStringLiteral("RenderNotifications"));
            QVERIFY(box);
            QVERIFY(box->isChecked()); // on by default
            QCOMPARE(box->text(), QStringLiteral("Show a system notification when renders finish"));
            box->setChecked(false);
            dialog.apply();
        }
        QVERIFY(!prefs::renderNotificationsEnabled());
        {
            PreferencesDialog dialog(nullptr);
            auto* box = dialog.findChild<QCheckBox*>(QStringLiteral("RenderNotifications"));
            QVERIFY(!box->isChecked());
            box->setChecked(true);
            dialog.apply();
        }
        QVERIFY(prefs::renderNotificationsEnabled());
    }

    // ---- Backend choice ----

    void backendChoice()
    {
        using platform::notifications::Backend;
        using platform::notifications::chooseBackend;
        QCOMPARE(chooseBackend(QString(), false), Backend::Freedesktop);
        QCOMPARE(chooseBackend(QString(), true), Backend::Portal); // Flatpak
        QCOMPARE(chooseBackend(QStringLiteral("off"), false), Backend::None);
        QCOMPARE(chooseBackend(QStringLiteral("Portal"), false), Backend::Portal);
        QCOMPARE(chooseBackend(QStringLiteral("freedesktop"), true), Backend::Freedesktop);
        // Offscreen runs (this test, screenshots) never get a real notifier.
        QVERIFY(!platform::createDesktopNotifier());
    }

    // ---- D-Bus, against fake services ----

    void startFakeServices()
    {
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("no session bus (run under dbus-run-session)");
        m_serviceBus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fakenotify");
        QVERIFY(m_serviceBus.isConnected());
        m_server = new FakeNotificationServer;
        m_portal = new FakePortal;
        m_serviceThread.start();
        m_server->moveToThread(&m_serviceThread);
        m_portal->moveToThread(&m_serviceThread);
        QVERIFY(m_serviceBus.registerObject(
            platform::notifications::kObjectPath, m_server, QDBusConnection::ExportAllSlots));
        QVERIFY(m_serviceBus.registerObject(
            platform::notifications::kPortalObjectPath, m_portal, QDBusConnection::ExportAllSlots));
        QVERIFY(m_serviceBus.registerService(kFakeService));
        QVERIFY(m_serviceBus.registerService(kFakePortal));
    }

    void freedesktopNotifyCall()
    {
        if (!m_server)
            QSKIP("no fake notification server");
        platform::FreedesktopNotifier notifier(QDBusConnection::sessionBus(), kFakeService);
        QVERIFY(notifier.supportsActions());
        QStringList keys;
        DesktopNotification n {QStringLiteral("Render complete"),
            QStringLiteral("Timeline 1 rendered in 0:42"),
            {{QStringLiteral("open-folder"), QStringLiteral("Open Folder")}}};
        QVERIFY(notifier.show(n, [&](const QString& key) { keys << key; }));
        QTRY_COMPARE(m_server->calls, 1);
        QCOMPARE(m_server->signature, QStringLiteral("susssasa{sv}i"));
        const QVariantList a = m_server->arguments;
        QCOMPARE(a.size(), 8);
        QCOMPARE(a[0].toString(), QStringLiteral("Kinoklip"));
        QCOMPARE(a[1].toUInt(), 0U);
        QCOMPARE(
            a[2].toString(), QStringLiteral("org.kinoklip.Kinoklip")); // the desktop file's icon
        QCOMPARE(a[3].toString(), QStringLiteral("Render complete"));
        QCOMPARE(a[4].toString(), QStringLiteral("Timeline 1 rendered in 0:42"));
        QCOMPARE(a[5].toStringList(),
            (QStringList {QStringLiteral("open-folder"), QStringLiteral("Open Folder")}));
        QCOMPARE(a[7].toInt(), -1);

        // The server reports the button press with the id Notify returned.
        QTest::qWait(100); // the Notify reply (id 7) has been read
        QDBusMessage other = QDBusMessage::createSignal(
            QLatin1String(platform::notifications::kObjectPath),
            QLatin1String(platform::notifications::kInterface), QStringLiteral("ActionInvoked"));
        other << uint(99) << QStringLiteral("open-folder"); // someone else's notification
        m_serviceBus.send(other);
        QDBusMessage signal = QDBusMessage::createSignal(
            QLatin1String(platform::notifications::kObjectPath),
            QLatin1String(platform::notifications::kInterface), QStringLiteral("ActionInvoked"));
        signal << uint(7) << QStringLiteral("open-folder");
        m_serviceBus.send(signal);
        QTRY_COMPARE(keys, QStringList {QStringLiteral("open-folder")});

        // Without the "actions" capability the buttons are left out.
        m_server->capabilities = {QStringLiteral("body")};
        platform::FreedesktopNotifier plain(QDBusConnection::sessionBus(), kFakeService);
        QVERIFY(!plain.supportsActions());
        QVERIFY(plain.show(n, {}));
        QTRY_COMPARE(m_server->calls, 2);
        QVERIFY(m_server->arguments[5].toStringList().isEmpty());
    }

    void portalNotifyCall()
    {
        if (!m_portal)
            QSKIP("no fake portal");
        platform::PortalNotifier notifier(QDBusConnection::sessionBus(), kFakePortal);
        QStringList keys;
        DesktopNotification n {QStringLiteral("Render complete"), QStringLiteral("body"),
            {{QStringLiteral("open-folder"), QStringLiteral("Open Folder")},
                {QStringLiteral("play"), QStringLiteral("Play")}}};
        QVERIFY(notifier.show(n, [&](const QString& key) { keys << key; }));
        QTRY_COMPARE(m_portal->calls, 1);
        QCOMPARE(m_portal->signature, QStringLiteral("sa{sv}"));
        QCOMPARE(
            m_portal->notification.value("title").toString(), QStringLiteral("Render complete"));
        QCOMPARE(m_portal->notification.value("body").toString(), QStringLiteral("body"));
        const QList<QVariantMap> buttons = buttonsOf(m_portal->notification.value("buttons"));
        QCOMPARE(buttons.size(), 2);
        QCOMPARE(buttons[1].value("label").toString(), QStringLiteral("Play"));
        QCOMPARE(buttons[1].value("action").toString(), QStringLiteral("play"));

        QDBusMessage signal
            = QDBusMessage::createSignal(QLatin1String(platform::notifications::kPortalObjectPath),
                QLatin1String(platform::notifications::kPortalInterface),
                QStringLiteral("ActionInvoked"));
        signal << m_portal->id << QStringLiteral("play") << QVariantList {};
        m_serviceBus.send(signal);
        QTRY_COMPARE(keys, QStringList {QStringLiteral("play")});
    }

    void noServiceDoesNothing()
    {
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("no session bus (run under dbus-run-session)");
        platform::FreedesktopNotifier notifier(
            QDBusConnection::sessionBus(), QStringLiteral("org.kinoklip.NoSuchNotifications"));
        QVERIFY(!notifier.show({QStringLiteral("x"), QStringLiteral("y"), {}}, {}));
        QVERIFY(!notifier.supportsActions());
        platform::PortalNotifier portal(
            QDBusConnection::sessionBus(), QStringLiteral("org.kinoklip.NoSuchPortal"));
        QVERIFY(!portal.show({QStringLiteral("x"), QStringLiteral("y"), {}}, {}));
    }
};

QTEST_MAIN(TestRenderNotify)
#include "test_rendernotify.moc"
