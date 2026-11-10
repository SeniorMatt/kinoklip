// SPDX-License-Identifier: GPL-3.0-or-later
// The menu and panel reference the application writes (spec section 63): runs offscreen,
// every entry and panel has a description, the text is the same on every run, the switch works
// as a process and leaves the user's settings alone.
#include "app/CommandLine.h"
#include "app/MainWindow.h"
#include "app/MenuReference.h"
#include "app/Theme.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace kk;

namespace {

/// What main() creates for the menu reference run.
struct Fixture
{
    Project project;
    SelectionModel selection;
    ActionRegistry actions;
    AppContext ctx;
    std::unique_ptr<MainWindow> window;

    explicit Fixture(bool withUndescribedAction = false)
    {
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        theme::registerActions(actions);
        if (withUndescribedAction)
            actions.add(QStringLiteral("testing.no_description"), QStringLiteral("No Words"),
                QStringLiteral("testing"));
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

class TestMenuReference : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_menureference"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // 63.1: the generator walks the window offscreen and the first line has the counts.
    void generatorRunsOffscreen()
    {
        Fixture f;
        const menureference::Reference ref = menureference::build(*f.window);
        QVERIFY(ref.entries > 250);
        QVERIFY(ref.menus >= 12);
        QVERIFY(ref.panels > 25);
        const QString first = ref.markdown.left(ref.markdown.indexOf(QLatin1Char('\n')));
        QCOMPARE(first,
            QStringLiteral("%1 menu entries in %2 menus; %3 panels")
                .arg(ref.entries)
                .arg(ref.menus)
                .arg(ref.panels));
        QVERIFY(!ref.markdown.contains(QStringLiteral("\r")));
        // No date, no machine-specific text.
        QVERIFY(
            !ref.markdown.contains(QRegularExpression(QStringLiteral("20[0-9][0-9]-[0-9][0-9]"))));
        QVERIFY(!ref.markdown.contains(QDir::homePath()));
    }

    // 63.2: every menu entry and every panel has a description.
    void everyEntryAndPanelIsDescribed()
    {
        Fixture f;
        const menureference::Reference ref = menureference::build(*f.window);
        QVERIFY2(ref.undescribed.isEmpty(), qPrintable(ref.undescribed.join(QStringLiteral("; "))));
        // Read it back as a reader of the file would: no empty last column anywhere.
        for (const QString& line : ref.markdown.split(QLatin1Char('\n'))) {
            if (!line.startsWith(QLatin1String("| ")) || line.startsWith(QLatin1String("| Entry"))
                || line.startsWith(QLatin1String("| Panel")))
                continue;
            const QStringList cells = line.split(QStringLiteral(" | "));
            QVERIFY2(
                !cells.last().trimmed().isEmpty() && cells.last().trimmed() != QLatin1String("|"),
                qPrintable(line));
        }
        // The table of descriptions has no line for a command that does not exist.
        ActionRegistry plain;
        plain.loadDefaultKeymap();
        registerStandardActions(plain);
        QVERIFY2(describeStandardActions(plain).isEmpty(),
            qPrintable(describeStandardActions(plain).join(QLatin1Char(' '))));
        for (const QString& id : plain.ids())
            QVERIFY2(!plain.action(id)->statusTip().isEmpty(), qPrintable(id));
    }

    // 63.3: the file is the same on every run, also from a second window.
    void stableAcrossRuns()
    {
        Fixture a;
        const QString one = menureference::build(*a.window).markdown;
        QCOMPARE(menureference::build(*a.window).markdown, one);
        Fixture b;
        QCOMPARE(menureference::build(*b.window).markdown, one);
    }

    // 63.4: the layout of the page: menu bar order, table head, shortcuts, marks, submenus.
    void layoutOfThePage()
    {
        Fixture f;
        const QString md = menureference::build(*f.window).markdown;
        QVERIFY(md.contains(
            QStringLiteral("| Entry | Shortcut (default Resolve keymap) | What it does |")));
        const int file = md.indexOf(QStringLiteral("\n## File\n"));
        const int edit = md.indexOf(QStringLiteral("\n## Edit\n"));
        const int help = md.indexOf(QStringLiteral("\n## Help\n"));
        const int panels = md.indexOf(QStringLiteral("\n## Panels\n"));
        QVERIFY(file > 0 && file < edit && edit < help && help < panels);
        QVERIFY(md.contains(QStringLiteral("| Undo | Ctrl+Z | ")));
        QVERIFY(md.contains(QStringLiteral("| Snapping (toggle) | N | ")));
        QVERIFY(md.contains(QStringLiteral("| Playback Resolution > Full Resolution (choice) | ")));
        QVERIFY(md.contains(QStringLiteral("| Import > Media")));
        QVERIFY(!md.contains(QStringLiteral("| - |"))); // separators are ignored
        QVERIFY(!md.contains(QStringLiteral("&"))); // mnemonics are removed
        // Menus that list files or machine settings are one row each.
        QVERIFY(md.contains(QStringLiteral("| Open Recent |  | ")));
        QVERIFY(!md.contains(QStringLiteral("No Recent Projects")));
        QVERIFY(md.contains(QStringLiteral("| Color Theme |  | ")));
        // Panels, page by page, in the page bar's order.
        int at = panels;
        for (const char* page :
            {"Media", "Cut", "Edit", "Compositing", "Color", "Audio", "Deliver"}) {
            const int next = md.indexOf(QStringLiteral("### %1 page").arg(QLatin1String(page)), at);
            QVERIFY2(next > at, page);
            at = next;
        }
        QVERIFY(md.contains(QStringLiteral("| Timeline Viewer | ")));
        QVERIFY(md.contains(QStringLiteral("| Render Queue | ")));
    }

    // 63.5: a command without words keeps the file from being written.
    void undescribedCommandIsAnError()
    {
        Fixture f(true);
        const menureference::Reference ref = menureference::build(*f.window);
        QVERIFY(ref.undescribed.join(QLatin1Char(' ')).contains(QStringLiteral("No Words")));
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("out.md"));
        QString error;
        QVERIFY(!menureference::writeFile(*f.window, path, &error));
        QVERIFY(error.contains(QStringLiteral("No Words")));
        QVERIFY(!QFile::exists(path));
    }

    // 63.6: the file is written whole, UTF-8, ending in a newline.
    void writesTheFile()
    {
        Fixture f;
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("out.md"));
        QString error;
        QVERIFY2(menureference::writeFile(*f.window, path, &error), qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QCOMPARE(QString::fromUtf8(bytes), menureference::build(*f.window).markdown);
        QVERIFY(bytes.endsWith('\n'));
    }

    // 63.7: --menu-reference is a command-line switch.
    void switchIsParsed()
    {
        CommandLineOptions options;
        QString error;
        QVERIFY(parseCommandLine({QStringLiteral("kinoklip"), QStringLiteral("--menu-reference"),
                                     QStringLiteral("x.md")},
            options, &error));
        QVERIFY(options.isMenuReference());
        QCOMPARE(options.menuReferenceFile, QStringLiteral("x.md"));
        CommandLineOptions none;
        QVERIFY(parseCommandLine({QStringLiteral("kinoklip")}, none, &error));
        QVERIFY(!none.isMenuReference());
    }

#ifdef KK_BINARY_PATH
    // 63.8: the real program writes the same file twice, with default settings whatever the
    // user's settings say, and leaves those settings alone.
    void processWritesTheSameFileWithIsolatedSettings()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        // A settings file that would change the output if it were read (another keyboard profile).
        const QString configDir = home.filePath(QStringLiteral("config/cutwise"));
        QVERIFY(QDir().mkpath(configDir));
        QFile conf(configDir + QStringLiteral("/Kinoklip.conf"));
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("[keymap]\nprofile=vim\n");
        conf.close();
        const QByteArray before = [&] {
            QFile c(conf.fileName());
            c.open(QIODevice::ReadOnly);
            return c.readAll();
        }();

        QString outputs[2];
        for (int i = 0; i < 2; ++i) {
            const QString out = home.filePath(QStringLiteral("ref%1.md").arg(i));
            QProcess process;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("XDG_CONFIG_HOME"), home.filePath(QStringLiteral("config")));
            env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
            env.insert(QStringLiteral("KK_NO_AUDIO"), QStringLiteral("1"));
            process.setProcessEnvironment(env);
            process.start(
                QStringLiteral(KK_BINARY_PATH), {QStringLiteral("--menu-reference"), out});
            QVERIFY(process.waitForFinished(120000));
            QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
            QFile file(out);
            QVERIFY(file.open(QIODevice::ReadOnly));
            outputs[i] = QString::fromUtf8(file.readAll());
        }
        QVERIFY(!outputs[0].isEmpty());
        QCOMPARE(outputs[0], outputs[1]);
        QVERIFY(outputs[0].contains(QStringLiteral("| Undo | Ctrl+Z | "))); // not the Vim profile
        QFile c(conf.fileName());
        QVERIFY(c.open(QIODevice::ReadOnly));
        QCOMPARE(c.readAll(), before);
    }
#endif
};

QTEST_MAIN(TestMenuReference)
#include "test_menureference.moc"
