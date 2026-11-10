// SPDX-License-Identifier: GPL-3.0-or-later
// Help > Export Log File (spec section 64): home path shortening, the sections the dialog
// names, the file, the command and its refusal on the remote paths, the menu entry and the note
// in the bug report dialog.
#include "app/BugReportDialog.h"
#include "app/LogExport.h"
#include "app/LogExportDialog.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/commands/Command.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <memory>

using namespace kk;

namespace {

sessionlog::Entry entry(qint64 ms, QtMsgType type, const char* category, const QString& text)
{
    sessionlog::Entry e;
    e.milliseconds = ms;
    e.type = type;
    e.category = QString::fromLatin1(category);
    e.text = text;
    return e;
}

QString readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

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

} // namespace

class TestLogExport : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_logexport"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // 64.5: paths under the home folder become ~/..., a longer folder name is left alone, the
    // user name and computer name are removed.
    void homeFolderIsShortened()
    {
        qputenv("HOME", "/home/anna");
        qputenv("USER", "anna");
        QCOMPARE(
            logexport::shorten(QStringLiteral("seek failed in /home/anna/Videos/a b.mp4 at 3")),
            QStringLiteral("seek failed in ~/Videos/a b.mp4 at 3"));
        QCOMPARE(logexport::shorten(QStringLiteral("/home/anna")), QStringLiteral("~"));
        QCOMPARE(logexport::shorten(QStringLiteral("(/home/anna/x)")), QStringLiteral("(~/x)"));
        QCOMPARE(logexport::shorten(QStringLiteral("/home/annabel/x")),
            QStringLiteral("/home/annabel/x"));
        QCOMPARE(logexport::shorten(QStringLiteral("/run/media/anna/disk/x.mov")),
            QStringLiteral("/run/media/<removed>/disk/x.mov"));
        QVERIFY(!logexport::shorten(QStringLiteral("/home/anna/anna/anna.mp4"))
                .contains(QStringLiteral("/home/anna")));
    }

    // 64.5: the file has the sections the dialog names, in that order, with the right lines in
    // each, the dropped-lines note, and no home folder.
    void fileHasTheSectionsTheDialogNames()
    {
        qputenv("HOME", "/home/anna");
        qputenv("USER", "anna");
        logexport::Session session;
        session.elapsedMilliseconds = 3'725'000;
        session.dropped = 42;
        session.entries = {
            entry(10, QtInfoMsg, "kk.media",
                QStringLiteral("decoding clip.mp4 (h264 high 1920x1080) with VAAPI (iHD 24.1.0)")),
            entry(20, QtInfoMsg, "kk.media",
                QStringLiteral("hardware decoding VAAPI available: /dev/dri/renderD128")),
            entry(1500, QtInfoMsg, "kk.playback",
                QStringLiteral("stats: 25.0 fps (of 25.00), 0 dropped")),
            entry(2500, QtWarningMsg, "kk.media",
                QStringLiteral("seek failed in /home/anna/Videos/clip.mp4 averror")),
            entry(2600, QtInfoMsg, "kk.core", QStringLiteral("something else happened")),
            entry(2700, QtCriticalMsg, "default", QStringLiteral("two\nlines")),
        };
        bugreport::SystemInfo info;
        info.version = QStringLiteral("1.2.3");
        info.lines = {{QStringLiteral("Kinoklip"), QStringLiteral("1.2.3")},
            {QStringLiteral("Desktop"),
                QStringLiteral("KDE, wayland session, Qt platform wayland")},
            {QStringLiteral("OpenGL"), QStringLiteral("Mesa Intel(R) Xe Graphics; 4.6; Intel")}};
        const QString text = logexport::build(
            info, session, QDateTime(QDate(2026, 11, 10), QTime(12, 0), Qt::UTC));

        QVERIFY(text.startsWith(QStringLiteral("Kinoklip log file\n")));
        QVERIFY(text.contains(QStringLiteral("42 earlier lines dropped")));
        QVERIFY(text.contains(QStringLiteral("Session length: 1 h 2 min")));
        int at = 0;
        for (const QString& title : logexport::sectionTitles()) {
            const int next = text.indexOf(QStringLiteral("\n== %1 ==\n").arg(title), at);
            QVERIFY2(next >= at && next >= 0, qPrintable(title));
            at = next + 1;
        }
        auto section = [&](int i) {
            const QStringList titles = logexport::sectionTitles();
            const int from = text.indexOf(QStringLiteral("\n== %1 ==\n").arg(titles.at(i)));
            const int to = i + 1 < titles.size()
                ? text.indexOf(QStringLiteral("\n== %1 ==\n").arg(titles.at(i + 1)))
                : text.size();
            return text.mid(from, to - from);
        };
        QVERIFY(section(0).contains(QStringLiteral("OpenGL: Mesa Intel(R) Xe Graphics")));
        QVERIFY(section(0).contains(QStringLiteral("wayland")));
        QVERIFY(section(1).contains(QStringLiteral("decoding clip.mp4 (h264")));
        QVERIFY(section(1).contains(QStringLiteral("hardware decoding VAAPI")));
        QVERIFY(section(2).contains(QStringLiteral("stats: 25.0 fps")));
        QVERIFY(section(3).contains(QStringLiteral("seek failed in ~/Videos/clip.mp4")));
        QVERIFY(section(3).contains(QStringLiteral("error default: two\n    lines")));
        QVERIFY(section(4).contains(QStringLiteral("something else happened")));
        QVERIFY(!section(4).contains(QStringLiteral("stats:")));
        QVERIFY(!text.contains(QStringLiteral("/home/anna")));
        QVERIFY(text.contains(QStringLiteral("[    0.010] info kk.media: decoding clip.mp4")));
    }

    // 64.5: empty sections say so.
    void emptySessionStillHasEverySection()
    {
        const QString text = logexport::build(bugreport::SystemInfo {}, logexport::Session {});
        for (const QString& title : logexport::sectionTitles())
            QVERIFY2(text.contains(QStringLiteral("== %1 ==").arg(title)), qPrintable(title));
        QVERIFY(!text.contains(QStringLiteral("earlier lines dropped")));
    }

    // 64.6: the dialog says what the file contains, lists the same sections, shows the text.
    void dialogNamesTheSectionsAndShowsTheText()
    {
        sessionlog::buffer().clear();
        sessionlog::record(QtWarningMsg, "kk.test", QStringLiteral("a warning for the dialog"));
        const QString text = logexport::buildCurrent();
        LogExportDialog dialog(text);
        QCOMPARE(dialog.windowTitle(), QStringLiteral("Export Log File"));
        for (const QString& title : logexport::sectionTitles()) {
            QVERIFY2(dialog.contentsText().contains(title), qPrintable(title));
            QVERIFY2(text.contains(QStringLiteral("== %1 ==").arg(title)), qPrintable(title));
        }
        const QString contents = dialog.contentsText();
        for (const char* word : {"Wayland or X11", "graphics card", "OpenGL", "decoder",
                 "playback statistics", "warnings and errors", "file", "home folder", "~/..."})
            QVERIFY2(contents.contains(QLatin1String(word), Qt::CaseInsensitive), word);
        QVERIFY(contents.contains(QStringLiteral("nothing is sent")));
        auto* preview = dialog.findChild<QPlainTextEdit*>(QStringLiteral("LogPreview"));
        QVERIFY(preview);
        QVERIFY(preview->isReadOnly());
        QVERIFY(preview->toPlainText().contains(QStringLiteral("a warning for the dialog")));
        QCOMPARE(dialog.logText(), text);
        QVERIFY(dialog.findChild<QPushButton*>(QStringLiteral("SaveLogButton")));
        sessionlog::buffer().clear();
    }

    // 64.7: the suggested name and the write.
    void exportWritesTheFile()
    {
        QCOMPARE(logexport::defaultFileName(QDate(2026, 11, 10)),
            QStringLiteral("kinoklip-log-2026-11-10.txt"));
        QVERIFY(QRegularExpression(QStringLiteral("^kinoklip-log-\\d{4}-\\d\\d-\\d\\d\\.txt$"))
                .match(logexport::defaultFileName())
                .hasMatch());
        sessionlog::buffer().clear();
        sessionlog::record(QtWarningMsg, "kk.test", QStringLiteral("written to the file"));
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("log.txt"));
        QString error;
        QVERIFY2(logexport::exportToFile(path, &error), qPrintable(error));
        const QString text = readAll(path);
        QVERIFY(text.contains(QStringLiteral("written to the file")));
        for (const QString& title : logexport::sectionTitles())
            QVERIFY(text.contains(QStringLiteral("== %1 ==").arg(title)));
        QVERIFY(
            !logexport::exportToFile(dir.filePath(QStringLiteral("no/such/dir/log.txt")), &error));
        QVERIFY(!error.isEmpty());
        sessionlog::buffer().clear();
    }

    // 64.8: the registry command, kept off the remote paths.
    void commandWritesAndIsForThePersonAtTheKeyboard()
    {
        const commands::CommandRegistry& registry = commands::CommandRegistry::builtin();
        const commands::Command* command = registry.find(QStringLiteral("log.export"));
        QVERIFY(command);
        QVERIFY(!command->spec.remoteAllowed);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("cmd.txt"));
        const commands::CommandResult result
            = registry.execute(QStringLiteral("log.export"), {{QStringLiteral("file"), path}});
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.data.value(QStringLiteral("file")).toString(), path);
        QVERIFY(readAll(path).startsWith(QStringLiteral("Kinoklip log file")));
        // Without a file name it is an error, not a write.
        QVERIFY(!registry.execute(QStringLiteral("log.export"), {}).ok);
        // The local control socket and the MCP server refuse it.
        Project project;
        commands::LiveTarget target;
        target.project = &project;
        const commands::CommandResult live = registry.executeLive(QStringLiteral("log.export"),
            {{QStringLiteral("file"), dir.filePath(QStringLiteral("x.txt"))}}, target);
        QVERIFY(!live.ok);
        QCOMPARE(live.errorCode, QStringLiteral("not_allowed_remotely"));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("x.txt"))));
    }

    // 64.9: Help > Export Log File... exists, follows Report a Bug, has words, opens the dialog.
    void helpMenuHasTheEntry()
    {
        Fixture f;
        QAction* action = f.actions.action(QStringLiteral("help.export_log"));
        QVERIFY(action);
        QVERIFY(QString(action->text())
                .remove(QLatin1Char('&'))
                .contains(QStringLiteral("Export Log File")));
        QVERIFY(action->text().endsWith(QStringLiteral("…"))
            || action->text().endsWith(QStringLiteral("...")));
        QVERIFY(!action->statusTip().isEmpty());
        QVERIFY(f.actions.defaultShortcuts(QStringLiteral("help.export_log")).isEmpty());
        const QStringList help = f.actions.menuEntries(QStringLiteral("help"));
        const int report = help.indexOf(QStringLiteral("help.report_bug"));
        QVERIFY(report >= 0);
        QVERIFY(help.indexOf(QStringLiteral("help.export_log")) > report);
        auto* helpMenu = f.window->menuBar()->findChild<QMenu*>(QStringLiteral("menu:help"));
        QVERIFY(helpMenu);
        QVERIFY(helpMenu->actions().contains(action));

        bool shown = false;
        QTimer::singleShot(0, this, [&] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (w->objectName() == QLatin1String("LogExportDialog") && w->isVisible()) {
                    shown = true;
                    static_cast<QDialog*>(w)->reject();
                }
            }
        });
        action->trigger();
        QVERIFY(shown);
    }

    // 64.10: the bug report dialog points at the log export.
    void bugReportDialogMentionsTheLog()
    {
        Fixture f;
        BugReportDialog* dialog = f.window->showBugReportDialog();
        auto* note = dialog->findChild<QLabel*>(QStringLiteral("BugReportLogNote"));
        QVERIFY(note);
        QVERIFY(note->text().contains(QStringLiteral("Export Log File")));
        dialog->close();
    }
};

QTEST_MAIN(TestLogExport)
#include "test_logexport.moc"
