// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of live control (app/live, spec 55.8 - 55.16): the option is off by default and nothing
// listens; with it on, a call from a local tool changes the project open in the main window as
// one undo step; a wrong `project` is refused; the socket is user-only; renders run off the GUI
// thread; `kinoklip mcp` and `kinoklip <command> --live` forward to the window, `--file` does not.
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/commands/Command.h"
#include "app/live/LiveClient.h"
#include "app/live/LiveProtocol.h"
#include "app/live/LiveServer.h"
#include "core/EditOps.h"
#include "core/Preferences.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "core/Timeline.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/timeline/TimelineEditUtil.h"
#include "ui/dialogs/PreferencesDialog.h"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <QtConcurrent>

using namespace kk;
using kk::commands::CommandRegistry;
using kk::commands::CommandResult;

namespace {

const QString kVideo = QStringLiteral(KK_TEST_DATA_DIR "/generated/h264_25fps.mp4"); // 10 s, V + A

struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    Fixture()
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        window = std::make_unique<MainWindow>(ctx, options);
    }
};

struct ProcessResult
{
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
};

} // namespace

class TestLiveControl : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    QString m_socket; ///< KK_CONTROL_SOCKET of the tests: never the real user socket
    QString m_project; ///< 24 fps project with kVideo imported

    QString path(const QString& name) const { return m_dir.filePath(name); }

    /// Calls a command from another thread, as a separate program would, while this thread
    /// serves the window's event loop.
    CommandResult live(const QString& command, const QJsonObject& arguments = {})
    {
        auto future = QtConcurrent::run([this, command, arguments] {
            live::LiveClient client;
            QString why;
            if (!client.connectToWindow(&why))
                return CommandResult::failure(QStringLiteral("no_window"), why);
            return client.call(command, arguments);
        });
        QElapsedTimer timer;
        timer.start();
        while (!future.isFinished() && timer.elapsed() < 120'000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return future.result();
    }

    CommandResult file(const QString& command, QJsonObject arguments)
    {
        arguments.insert(QStringLiteral("project"), m_project);
        return CommandRegistry::builtin().execute(command, arguments);
    }

    /// Runs the application with @p args and @p input on stdin while this thread serves the window.
    ProcessResult runApp(const QStringList& args, const QByteArray& input = {})
    {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        env.insert(QStringLiteral("KK_NO_AUDIO"), QStringLiteral("1"));
        env.insert(QStringLiteral("KK_CONTROL_SOCKET"), m_socket);
        process.setProcessEnvironment(env);
        process.start(QStringLiteral(KK_BINARY_PATH), args);
        ProcessResult r;
        if (!process.waitForStarted(30'000))
            return r;
        process.write(input);
        process.closeWriteChannel();
        QElapsedTimer timer;
        timer.start();
        while (process.state() != QProcess::NotRunning && timer.elapsed() < 120'000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished();
        }
        r.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
        r.out = process.readAllStandardOutput();
        r.err = process.readAllStandardError();
        return r;
    }

    /// Loads the media-importing project into the window's project and switches live control on.
    void enable(Fixture& f)
    {
        QString error;
        QVERIFY2(f.project.load(m_project, &error), qPrintable(error));
        prefs::setLocalControlEnabled(true);
        f.window->applyLocalControlPreference();
        QVERIFY(f.window->liveServer()->isListening());
    }

    /// The id of the imported file in the media pool (media.list also lists the timelines).
    QString mediaId()
    {
        for (const QJsonValue& v : file(QStringLiteral("media.list"), {}).data["media"].toArray()) {
            if (v.toObject()["kind"].toString() == QLatin1String("file"))
                return v.toObject()["id"].toString();
        }
        return {};
    }

    static int clipCount(const Project& project)
    {
        int n = 0;
        const TimelineData& d = project.timeline()->data();
        for (const TrackData& t : d.videoTracks)
            n += int(t.clips.size());
        for (const TrackData& t : d.audioTracks)
            n += int(t.clips.size());
        return n;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_livecontrol"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
        QVERIFY(m_dir.isValid());
        m_socket = path(QStringLiteral("control.sock"));
        qputenv("KK_CONTROL_SOCKET", m_socket.toUtf8());
        m_project = path(QStringLiteral("live.kkproj"));
        QVERIFY(CommandRegistry::builtin()
                    .execute(QStringLiteral("project.new"),
                        {{"project", m_project}, {"width", 640}, {"height", 360}, {"fps", 24}})
                    .ok);
        QVERIFY(CommandRegistry::builtin()
                    .execute(QStringLiteral("media.import"),
                        {{"project", m_project}, {"file", QJsonArray {kVideo}}})
                    .ok);
    }

    void init() { QSettings().clear(); }

    void preferencesHasTheOptionOffByDefault()
    {
        PreferencesDialog dialog(nullptr); // standard pages only
        QVERIFY(dialog.showSection(QStringLiteral("control")));
        auto* box = dialog.findChild<QCheckBox*>(QStringLiteral("LocalControl"));
        QVERIFY(box);
        QVERIFY(box->text().contains("Let local tools control Kinoklip"));
        QVERIFY(!box->isChecked());
        QVERIFY(!prefs::localControlEnabled());
        box->setChecked(true);
        dialog.apply();
        QVERIFY(prefs::localControlEnabled());
    }

    void offByDefaultAndNothingListens()
    {
        QVERIFY(!prefs::localControlEnabled());
        Fixture f;
        QVERIFY(!f.window->liveServer()->isListening());
        QVERIFY(!QFileInfo::exists(m_socket));
        QLocalSocket socket;
        socket.connectToServer(m_socket);
        QVERIFY(!socket.waitForConnected(300));
        // Through the client, as a tool would: refused, with a reason naming the option.
        const CommandResult r = live(QStringLiteral("version"));
        QVERIFY(!r.ok);
        QCOMPARE(r.errorCode, QString("no_window"));
    }

    void switchingTheOptionOnAndOff()
    {
        Fixture f;
        prefs::setLocalControlEnabled(true);
        f.window->applyLocalControlPreference();
        QVERIFY(f.window->liveServer()->isListening());
        QVERIFY(live(QStringLiteral("version")).ok);
        prefs::setLocalControlEnabled(false);
        f.window->applyLocalControlPreference();
        QVERIFY(!f.window->liveServer()->isListening());
        QVERIFY(!live(QStringLiteral("version")).ok); // refused again
        QVERIFY(!QFileInfo::exists(m_socket));
    }

    void theSocketIsOnlyForThisUser()
    {
        Fixture f;
        enable(f);
        const QFileInfo info(m_socket);
        QVERIFY(info.exists());
        const QFile::Permissions others = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
            | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
        QVERIFY2(!(info.permissions() & others),
            qPrintable(QString::number(int(info.permissions()), 16)));
        QVERIFY(live::isOwnedByCurrentUser(m_socket));
        QVERIFY(info.permissions() & QFile::ReadOwner);
    }

    void aLeftoverSocketFileIsReplacedButALiveOneIsNot()
    {
        QFile stale(m_socket); // what a crashed run leaves behind
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.close();
        Fixture first;
        enable(first);
        QVERIFY(first.window->liveServer()->isListening());
        Fixture second; // a second window must not take the socket over
        QString error;
        QVERIFY(!second.window->liveServer()->listen(&error));
        QVERIFY2(error.contains("another Kinoklip window"), qPrintable(error));
        QVERIFY(first.window->liveServer()->isListening());
        QVERIFY(live(QStringLiteral("version")).ok); // still the first one's
    }

    void aCallChangesTheOpenProjectAsOneUndoStep()
    {
        Fixture f;
        enable(f);
        const int stepsBefore = f.project.undoStack()->count();
        QCOMPARE(clipCount(f.project), 0);
        const QString media = mediaId();
        const CommandResult r = live(QStringLiteral("clip.add"),
            {{"media", media}, {"source-out", "00:00:02:00"}}); // video and linked audio clip
        QVERIFY2(r.ok, qPrintable(r.errorCode + ": " + r.error));
        QCOMPARE(clipCount(f.project), 2);
        // One undo step for the whole call, and undo restores the project.
        QCOMPARE(f.project.undoStack()->count(), stepsBefore + 1);
        f.project.undo();
        QCOMPARE(clipCount(f.project), 0);
        f.project.redo();
        QCOMPARE(clipCount(f.project), 2);
        // A second kind of edit: it sees the window's state, with no file in between.
        const QJsonArray clips = live(QStringLiteral("clip.list")).data["clips"].toArray();
        QCOMPARE(clips.size(), 2);
        const qint64 v = clips[0]["id"].toInteger();
        const int before = f.project.undoStack()->count();
        QVERIFY(live(QStringLiteral("effect.add"),
            {{"clip", v}, {"type", "gaussian_blur"}, {"set", QJsonArray {"radius=9"}}})
                    .ok);
        QCOMPARE(f.project.undoStack()->count(), before + 1);
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips[0].effects.size(), 1);
        f.project.undo();
        QCOMPARE(f.project.timeline()->data().videoTracks[0].clips[0].effects.size(), 0);
        // The file on disk was not touched (no save per call).
        QString error;
        Project onDisk;
        QVERIFY(onDisk.load(m_project, &error));
        QCOMPARE(clipCount(onDisk), 0);
        QVERIFY(f.project.isDirty());
    }

    void aNoOpInsideACallDoesNotUndoTheRestOfTheCall()
    {
        // A live call is one open edit; a step of it that changes nothing (a switch already in
        // the wanted state) used to cancel the whole edit and throw away the earlier steps.
        Project project;
        project.newProject();
        project.beginEdit(QStringLiteral("Assistant: two steps"));
        QVERIFY(timelineedit::runEdit(&project, QStringLiteral("first"), [](TimelineData& d) {
            Marker m;
            m.frame = 4;
            d.markers.append(m);
            return true;
        }));
        QVERIFY(!timelineedit::runEdit(
            &project, QStringLiteral("second"), [](TimelineData&) { return false; }));
        project.endEdit();
        QCOMPARE(project.timeline()->data().markers.size(), 1);
        QCOMPARE(project.undoStack()->count(), 1);
        // Outside a call a step that changes nothing still leaves no undo step.
        QVERIFY(!timelineedit::runEdit(
            &project, QStringLiteral("none"), [](TimelineData&) { return false; }));
        QCOMPARE(project.undoStack()->count(), 1);
    }

    void aFailedCallLeavesNothing()
    {
        Fixture f;
        enable(f);
        const int steps = f.project.undoStack()->count();
        const CommandResult r = live(QStringLiteral("clip.add"), {{"media", "no such media"}});
        QVERIFY(!r.ok);
        QCOMPARE(f.project.undoStack()->count(), steps);
        QCOMPARE(clipCount(f.project), 0);
        QVERIFY(!live(QStringLiteral("no.such.command")).ok);
    }

    void aWrongProjectIsRefused()
    {
        Fixture f;
        enable(f);
        const CommandResult wrong
            = live(QStringLiteral("clip.list"), {{"project", path("other.kkproj")}});
        QVERIFY(!wrong.ok);
        QCOMPARE(wrong.errorCode, QString("wrong_project"));
        QVERIFY2(wrong.error.contains(m_project), qPrintable(wrong.error)); // names the open one
        QVERIFY(live(QStringLiteral("clip.list"), {{"project", m_project}}).ok);
        QVERIFY(live(QStringLiteral("clip.list")).ok); // `project` is optional
        // Writing to another file from a live call is not a thing.
        const CommandResult output
            = live(QStringLiteral("clip.add"), {{"media", "x"}, {"output", path("copy.kkproj")}});
        QCOMPARE(output.errorCode, QString("live_unsupported"));
        QVERIFY(!QFileInfo::exists(path("copy.kkproj")));
    }

    void anUnsavedProjectIsNamedAndCannotBeSavedByTheTool()
    {
        Fixture f;
        f.project.newProject();
        prefs::setLocalControlEnabled(true);
        f.window->applyLocalControlPreference();
        const CommandResult wrong = live(QStringLiteral("clip.list"), {{"project", m_project}});
        QCOMPARE(wrong.errorCode, QString("wrong_project"));
        QVERIFY2(wrong.error.contains("has not been saved"), qPrintable(wrong.error));
        QCOMPARE(live(QStringLiteral("project.save")).errorCode, QString("no_file"));
    }

    void projectSaveUsesTheWindowsSave()
    {
        Fixture f;
        enable(f);
        const QString media = mediaId();
        QVERIFY(live(QStringLiteral("clip.add"), {{"media", media}}).ok);
        QVERIFY(f.project.isDirty());
        const CommandResult saved = live(QStringLiteral("project.save"));
        QVERIFY2(saved.ok, qPrintable(saved.errorCode + saved.error));
        QVERIFY(!f.project.isDirty());
        Project onDisk;
        QString error;
        QVERIFY(onDisk.load(m_project, &error));
        QCOMPARE(clipCount(onDisk), 2);
        // Put the file back for the other tests.
        QVERIFY(QFile::remove(m_project));
        QVERIFY(CommandRegistry::builtin()
                    .execute(QStringLiteral("project.new"),
                        {{"project", m_project}, {"width", 640}, {"height", 360}, {"fps", 24}})
                    .ok);
        QVERIFY(file(QStringLiteral("media.import"), {{"file", QJsonArray {kVideo}}}).ok);
    }

    void settingsImportIsRefusedOnTheLiveSocket()
    {
        Fixture f;
        enable(f);
        const CommandResult refused
            = live(QStringLiteral("settings.import"), {{"file", "/nonexistent"}});
        QVERIFY(!refused.ok);
        QCOMPARE(refused.errorCode, QString("not_allowed_remotely"));
        // Typed by the user in file mode the command still runs (here: fails on the missing file).
        const CommandResult typed = CommandRegistry::builtin().execute(
            QStringLiteral("settings.import"), {{"file", "/nonexistent"}});
        QCOMPARE(typed.errorCode, QString("bad_settings_file"));
        QVERIFY(live(QStringLiteral("settings.export"),
                    {{"file", QDir::tempPath() + "/kk-live-export.kinoklip-settings"}})
                    .ok);
    }

    void sessionInfoTellsTheMode()
    {
        Fixture f;
        enable(f);
        const CommandResult info = live(QStringLiteral("session.info"));
        QVERIFY(info.ok);
        QCOMPARE(info.data["mode"].toString(), QString("live"));
        QCOMPARE(info.data["project"].toString(), m_project);
        const CommandResult fileMode
            = CommandRegistry::builtin().execute(QStringLiteral("session.info"), {});
        QCOMPARE(fileMode.data["mode"].toString(), QString("file"));
        QVERIFY(fileMode.data["project"].isNull());
    }

    void aBusyWindowRefusesInsteadOfSwallowingTheEdit()
    {
        Fixture f;
        enable(f);
        f.project.beginEdit(QStringLiteral("drag"));
        const CommandResult r = live(QStringLiteral("marker.add"), {{"at", 5}});
        f.project.cancelEdit();
        QCOMPARE(r.errorCode, QString("window_busy"));
        QVERIFY(live(QStringLiteral("marker.add"), {{"at", 5}}).ok);
    }

    void aFrameIsRenderedOffTheGuiThread()
    {
        Fixture f;
        enable(f);
        const QString media = mediaId();
        QVERIFY(
            live(QStringLiteral("clip.add"), {{"media", media}, {"source-out", "00:00:02:00"}}).ok);
        const int steps = f.project.undoStack()->count();
        // Progress of the worker is delivered on this thread: count the ticks of the event loop
        // while the call runs to see that the GUI thread was free.
        int ticks = 0;
        QTimer tick;
        connect(&tick, &QTimer::timeout, this, [&] { ++ticks; });
        tick.start(5);
        const CommandResult view = live(QStringLiteral("frame.view"), {{"at", 3}, {"width", 320}});
        QVERIFY2(view.ok, qPrintable(view.errorCode + ": " + view.error));
        QCOMPARE(view.image.width(), 320);
        const QString png = path("live-frame.png");
        const CommandResult exported
            = live(QStringLiteral("frame.export"), {{"at", 3}, {"file", png}, {"width", 320}});
        QVERIFY2(exported.ok, qPrintable(exported.errorCode + ": " + exported.error));
        QVERIFY(QImage(png).width() == 320);
        QVERIFY(ticks > 0);
        QCOMPARE(f.project.undoStack()->count(), steps); // a render is no edit
        QVERIFY(live(QStringLiteral("frame.view"), {{"at", 100000}}).errorCode
            == QString("out_of_range"));
    }

    void theIndicatorShowsWhileAClientIsConnected()
    {
        Fixture f;
        enable(f);
        QLabel* indicator = f.window->findChild<QLabel*>(QStringLiteral("RemoteControlIndicator"));
        QVERIFY(indicator);
        QVERIFY(indicator->isHidden());
        QSignalSpy spy(f.window->liveServer(), &live::LiveServer::clientCountChanged);
        QLocalSocket socket;
        socket.connectToServer(m_socket);
        QVERIFY(socket.waitForConnected(1000));
        QTRY_COMPARE(f.window->liveServer()->clientCount(), 1);
        QVERIFY(!indicator->isHidden());
        socket.disconnectFromServer();
        QTRY_COMPARE(f.window->liveServer()->clientCount(), 0);
        QVERIFY(indicator->isHidden());
        QVERIFY(spy.count() >= 2);
    }

    void mcpForwardsToTheWindowAndFileModeStillWorks()
    {
        Fixture f;
        enable(f);
        const QByteArray input
            = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
              "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{}}}\n"
              "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
              "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
              "{\"name\":\"session_info\",\"arguments\":{}}}\n"
              "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":"
              "{\"name\":\"marker_add\",\"arguments\":{\"at\":7,\"name\":\"from mcp\"}}}\n";
        const ProcessResult live = runApp({QStringLiteral("mcp")}, input);
        QCOMPARE(live.exitCode, 0);
        QList<QJsonValue> replies;
        for (const QByteArray& line : live.out.split('\n')) {
            if (!line.isEmpty())
                replies << QJsonValue(QJsonDocument::fromJson(line).object());
        }
        QCOMPARE(replies.size(), 4);
        // In live mode `project` is not required by any tool.
        for (const QJsonValue& tool : replies[1]["result"]["tools"].toArray()) {
            const QJsonArray required = tool["inputSchema"]["required"].toArray();
            // project_new creates the file it names, so it keeps `project` in live mode too.
            if (tool["name"].toString() != QLatin1String("project_new"))
                QVERIFY2(!required.contains("project"), qPrintable(tool["name"].toString()));
        }
        QCOMPARE(replies[2]["result"]["structuredContent"]["mode"].toString(), QString("live"));
        QVERIFY(!replies[3]["result"]["isError"].toBool());
        QCOMPARE(f.project.timeline()->data().markers.size(), 1);
        QCOMPARE(f.project.timeline()->data().markers[0].name, QString("from mcp"));
        f.project.undo();
        QCOMPARE(f.project.timeline()->data().markers.size(), 0);

        // --file forces file mode: `project` is required again and the window is not touched.
        const QByteArray fileInput
            = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}\n"
              "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
              "{\"name\":\"session_info\",\"arguments\":{}}}\n"
              "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
              "{\"name\":\"marker_add\",\"arguments\":{\"at\":9}}}\n";
        const ProcessResult forced
            = runApp({QStringLiteral("mcp"), QStringLiteral("--file")}, fileInput);
        QCOMPARE(forced.exitCode, 0);
        QList<QJsonValue> fileReplies;
        for (const QByteArray& line : forced.out.split('\n')) {
            if (!line.isEmpty())
                fileReplies << QJsonValue(QJsonDocument::fromJson(line).object());
        }
        QCOMPARE(fileReplies.size(), 3);
        QCOMPARE(fileReplies[1]["result"]["structuredContent"]["mode"].toString(), QString("file"));
        QVERIFY(fileReplies[2]["result"]["isError"].toBool()); // missing `project`
        QCOMPARE(f.project.timeline()->data().markers.size(), 0);
    }

    void theCommandLineForwardsWithLive()
    {
        Fixture f;
        enable(f);
        const ProcessResult r = runApp({"marker", "add", "--at", "12", "--live", "--json"});
        QCOMPARE(r.exitCode, 0);
        QVERIFY(QJsonDocument::fromJson(r.out.trimmed()).object()["ok"].toBool());
        QCOMPARE(f.project.timeline()->data().markers.size(), 1);
        f.project.undo();
        QCOMPARE(f.project.timeline()->data().markers.size(), 0);
        // Without --live the command wants a file.
        const ProcessResult plain = runApp({"marker", "add", "--at", "12"});
        QCOMPARE(plain.exitCode, 2);
        // Window closed to local tools: --live says why.
        prefs::setLocalControlEnabled(false);
        f.window->applyLocalControlPreference();
        const ProcessResult off = runApp({"marker", "add", "--at", "12", "--live"});
        QCOMPARE(off.exitCode, 1);
        QVERIFY2(off.err.contains("Local Control"), off.err.constData());
    }
};

QTEST_MAIN(TestLiveControl)
#include "test_livecontrol.moc"
