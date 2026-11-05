// SPDX-License-Identifier: GPL-3.0-or-later
// Help > Report a Bug or Inconsistency (src/app/BugReport, BugReportDialog): what the report
// can carry (nothing personal), the GitHub issue-form link, the dialog and the menu entry, and
// the issue forms in .github/ISSUE_TEMPLATE. The URL opener is replaced: no browser opens and
// nothing is sent anywhere.
#include "app/AppInfo.h"
#include "app/BugReport.h"
#include "app/BugReportDialog.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/Project.h"
#include "core/SelectionModel.h"
#include "render/GlInfo.h"
#include "ui/common/ActionRegistry.h"
#include "ui/common/AppContext.h"
#include "ui/common/Resources.h"
#include "ui/common/StandardActions.h"
#include "ui/help/CopyLinkDialog.h"
#include "ui/help/Guide.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTest>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

using namespace kk;
using bugreport::Kind;

namespace {

/// Records the URLs the app would open.
struct FakeOpener
{
    QList<QUrl> opened;
    bool succeed = true; ///< false: no browser could be opened
    FakeOpener()
    {
        guide::setUrlOpenerForTesting([this](const QUrl& url) {
            opened.append(url);
            return succeed;
        });
    }
    ~FakeOpener() { guide::setUrlOpenerForTesting({}); }
};

QString queryValue(const QUrl& url, const char* key)
{
    return QUrlQuery(url).queryItemValue(QLatin1String(key), QUrl::FullyDecoded);
}

QStringList queryKeys(const QUrl& url)
{
    QStringList keys;
    for (const auto& item : QUrlQuery(url).queryItems())
        keys.append(item.first);
    return keys;
}

bugreport::SystemInfo sampleInfo()
{
    bugreport::SystemInfo info;
    info.version = QStringLiteral("1.1.2-beta.1");
    info.lines = {{QStringLiteral("Kinoklip"), QStringLiteral("1.1.2-beta.1")},
        {QStringLiteral("Qt"), QStringLiteral("6.8.2 (built with 6.8.2)")},
        {QStringLiteral("Odd"), QStringLiteral("a+b & c=d #e 100% ü")}};
    return info;
}

/// The personal strings that must never appear (too short ones cannot be told from words).
QStringList personalStrings()
{
    QStringList out {QDir::homePath()};
    for (const QString& s : {qEnvironmentVariable("USER"), QSysInfo::machineHostName()}) {
        if (s.size() >= 3)
            out.append(s);
    }
    return out;
}

/// The `id:` values of an issue form, read with a simple line parser.
QStringList formIds(const QString& file, bool* ok)
{
    QFile f(QStringLiteral(KK_TEST_DATA_DIR "/../../.github/ISSUE_TEMPLATE/") + file);
    *ok = f.open(QIODevice::ReadOnly);
    QStringList ids;
    const QRegularExpression re(QStringLiteral("^\\s+id:\\s*(\\S+)\\s*$"));
    for (const QByteArray& line : f.readAll().split('\n')) {
        const auto m = re.match(QString::fromUtf8(line));
        if (m.hasMatch())
            ids.append(m.captured(1));
    }
    return ids;
}

} // namespace

class TestBugReport : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("kinoklip-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_bugreport"));
        QSettings().clear();
        initResources();
        theme::apply(*qApp);
    }

    // ---- What is collected --------------------------------------------------------------

    void collectorHasEveryField()
    {
        const bugreport::SystemInfo info = bugreport::collect();
        QCOMPARE(info.version, appinfo::displayVersion());
        QStringList labels;
        for (const auto& line : info.lines)
            labels.append(line.first);
        const QStringList expected {QStringLiteral("Kinoklip"), QStringLiteral("Package"),
            QStringLiteral("Distribution"), QStringLiteral("Kernel"), QStringLiteral("Desktop"),
            QStringLiteral("Qt"), QStringLiteral("FFmpeg"), QStringLiteral("OpenGL"),
            QStringLiteral("Hardware decoding"), QStringLiteral("Screens"), QStringLiteral("Look"),
            QStringLiteral("Language")};
        QCOMPARE(labels, expected);
        const QString text = bugreport::environmentText(info.lines);
        QVERIFY(text.startsWith(QStringLiteral("Kinoklip: ") + appinfo::displayVersion()));
        QVERIFY2(
            text.contains(QStringLiteral("Qt: ") + QLatin1String(qVersion())), qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("avcodec ")));
        QVERIFY(text.contains(QStringLiteral("swresample ")));
        QVERIFY(text.contains(QStringLiteral("Kernel: ") + QSysInfo::kernelVersion()));
        QVERIFY(text.contains(QStringLiteral("Qt platform ") + QGuiApplication::platformName()));
        QVERIFY(text.contains(QStringLiteral(" colour theme")));
        // Not started in this process: no context is made for the report.
        QVERIFY(text.contains(QStringLiteral("OpenGL: not started yet")));
        // One fact per line, no empty values.
        for (const auto& line : info.lines)
            QVERIFY2(!line.second.isEmpty() && !line.second.contains(QLatin1Char('\n')),
                qPrintable(line.first));
    }

    void collectorShowsTheKnownOpenGlStrings()
    {
        render::rememberGlDriverInfo(
            {QStringLiteral("Mesa"), QStringLiteral("llvmpipe (LLVM 19.1.0, 256 bits)"),
                QStringLiteral("4.5 (Core Profile) Mesa 24.2.8")});
        const QString text = bugreport::environmentText(bugreport::collect().lines);
        render::rememberGlDriverInfo({});
        QVERIFY2(
            text.contains(QStringLiteral(
                "OpenGL: llvmpipe (LLVM 19.1.0, 256 bits); 4.5 (Core Profile) Mesa 24.2.8; Mesa")),
            qPrintable(text));
    }

    void collectorSharesNothingPersonal()
    {
        // Even values that would carry the home folder, user or host name come out without.
        const QByteArray oldDesktop = qgetenv("XDG_CURRENT_DESKTOP");
        const QByteArray oldAppImage = qgetenv("APPIMAGE");
        const QString host = QSysInfo::machineHostName();
        qputenv("XDG_CURRENT_DESKTOP",
            (QDir::homePath() + QStringLiteral("/desk:") + host + QStringLiteral(":")
                + qEnvironmentVariable("USER"))
                .toUtf8());
        qputenv(
            "APPIMAGE", (QDir::homePath() + QStringLiteral("/Apps/Kinoklip.AppImage")).toUtf8());
        const bugreport::SystemInfo info = bugreport::collect();
        auto restore = [](const char* name, const QByteArray& value) {
            if (value.isNull())
                qunsetenv(name);
            else
                qputenv(name, value);
        };
        restore("XDG_CURRENT_DESKTOP", oldDesktop);
        restore("APPIMAGE", oldAppImage);

        const QString text
            = info.version + QLatin1Char('\n') + bugreport::environmentText(info.lines);
        for (const QString& personal : personalStrings())
            QVERIFY2(
                !text.contains(personal), qPrintable(personal + QStringLiteral(" in:\n") + text));
        QVERIFY(text.contains(QStringLiteral("Package: AppImage")));
        QVERIFY(!text.contains(QStringLiteral("Kinoklip.AppImage")));
        // The real collection (environment as the test runs) is clean too.
        const bugreport::SystemInfo plain = bugreport::collect();
        const QString plainText = bugreport::environmentText(plain.lines);
        for (const QString& personal : personalStrings())
            QVERIFY2(!plainText.contains(personal), qPrintable(personal));
        qInfo().noquote() << "shared text in this environment:\n" + plainText;
    }

    void scrubRemovesWholeWordsOnly()
    {
        const QString user = qEnvironmentVariable("USER");
        if (user.size() < 3)
            QSKIP("USER too short to check");
        QCOMPARE(bugreport::scrub(QStringLiteral("by ") + user + QStringLiteral(".")),
            QStringLiteral("by <removed>."));
        // Part of a longer word stays (e.g. a user "kde" and "kdeplasma").
        QCOMPARE(bugreport::scrub(user + QStringLiteral("x")), user + QStringLiteral("x"));
        QCOMPARE(bugreport::scrub(QDir::homePath() + QStringLiteral("/Videos/a.mp4")),
            QStringLiteral("<removed>/Videos/a.mp4"));
    }

    void osReleaseIsParsed()
    {
        const QByteArray text = "NAME=\"Linux Mint\"\nVERSION=\"22.1 (Xia)\"\nID=linuxmint\n"
                                "# comment\nVERSION_ID='22.1'\nPRETTY_NAME=\"Linux Mint 22.1\"\n";
        const auto [name, version] = bugreport::parseOsRelease(text);
        QCOMPARE(name, QStringLiteral("Linux Mint"));
        QCOMPARE(version, QStringLiteral("22.1"));
        const auto arch = bugreport::parseOsRelease("NAME=\"Arch Linux\"\nID=arch\n");
        QCOMPARE(arch.first, QStringLiteral("Arch Linux"));
        QVERIFY(arch.second.isEmpty());
    }

    // ---- The link -----------------------------------------------------------------------

    void linkNamesTheTemplateAndPrefillsTheFields()
    {
        const bugreport::SystemInfo info = sampleInfo();
        const bugreport::IssueLink link = bugreport::issueLink(Kind::Bug, info, true);
        QCOMPARE(link.url.scheme(), QStringLiteral("https"));
        QCOMPARE(link.url.host(), QStringLiteral("github.com"));
        QCOMPARE(link.url.path(), QStringLiteral("/SeniorMatt/kinoklip/issues/new"));
        QCOMPARE(queryKeys(link.url),
            (QStringList {QStringLiteral("template"), QStringLiteral("title"),
                QStringLiteral("version"), QStringLiteral("environment")}));
        QCOMPARE(queryValue(link.url, "template"), QStringLiteral("bug_report.yml"));
        QCOMPARE(queryValue(link.url, "title"), QStringLiteral("[Bug] "));
        QCOMPARE(queryValue(link.url, "version"), QStringLiteral("1.1.2-beta.1"));
        QCOMPARE(queryValue(link.url, "environment"), bugreport::environmentText(info.lines));
        QCOMPARE(link.environment, bugreport::environmentText(info.lines));
        QCOMPARE(link.sharedText(), link.environment);

        // Every special character percent-encoded ('+' would read as a space on GitHub).
        const QByteArray encoded = link.url.toEncoded();
        QVERIFY2(
            encoded.contains("a%2Bb%20%26%20c%3Dd%20%23e%20100%25%20%C3%BC"), encoded.constData());
        QVERIFY(encoded.contains("%0A"));
        QVERIFY(encoded.contains("title=%5BBug%5D%20"));
        QVERIFY(!encoded.contains(' '));

        QCOMPARE(
            queryValue(bugreport::issueLink(Kind::ResolveDifference, info, true).url, "template"),
            QStringLiteral("resolve_difference.yml"));
        QCOMPARE(queryValue(bugreport::issueLink(Kind::ResolveDifference, info, true).url, "title"),
            QStringLiteral("[Resolve] "));
    }

    void ideaSharesTheVersionOnly()
    {
        const bugreport::IssueLink link = bugreport::issueLink(Kind::Idea, sampleInfo(), true);
        QCOMPARE(queryValue(link.url, "template"), QStringLiteral("feature_request.yml"));
        QCOMPARE(queryValue(link.url, "version"), QStringLiteral("1.1.2-beta.1"));
        QVERIFY(!queryKeys(link.url).contains(QStringLiteral("environment")));
        QCOMPARE(link.sharedText(), QStringLiteral("Kinoklip: 1.1.2-beta.1"));
    }

    void withoutItCarriesNothing()
    {
        for (Kind kind : {Kind::Bug, Kind::ResolveDifference, Kind::Idea}) {
            const bugreport::IssueLink link = bugreport::issueLink(kind, sampleInfo(), false);
            QCOMPARE(queryKeys(link.url), QStringList {QStringLiteral("template")});
            QCOMPARE(queryValue(link.url, "template"), bugreport::templateFile(kind));
            QVERIFY(link.environment.isEmpty());
            QVERIFY(link.version.isEmpty());
            QVERIFY(link.sharedText().isEmpty());
        }
    }

    void longEnvironmentIsShortenedBelowTheCap()
    {
        bugreport::SystemInfo info = sampleInfo();
        for (int i = 0; i < 400; ++i)
            info.lines.append({QStringLiteral("Fact %1").arg(i), QString(30, QChar(u'ä'))});
        const bugreport::IssueLink link = bugreport::issueLink(Kind::Bug, info, true);
        QVERIFY2(link.url.toEncoded().size() <= bugreport::kMaxUrlLength,
            QByteArray::number(link.url.toEncoded().size()).constData());
        QVERIFY(link.url.toEncoded().size() > bugreport::kMaxUrlLength - 400); // not cut too much
        QVERIFY(link.environment.startsWith(QStringLiteral("Kinoklip: 1.1.2-beta.1\nQt: ")));
        QVERIFY(!link.environment.contains(QStringLiteral("Fact 399")));
        // What the dialog shows is exactly what the link carries.
        QCOMPARE(queryValue(link.url, "environment"), link.environment);

        // One huge value: cut with "...".
        bugreport::SystemInfo huge;
        huge.version = QStringLiteral("1.1.2-beta.1");
        huge.lines = {{QStringLiteral("Kinoklip"), QString(20000, QLatin1Char('+'))}};
        const bugreport::IssueLink cut = bugreport::issueLink(Kind::Bug, huge, true);
        QVERIFY(cut.url.toEncoded().size() <= bugreport::kMaxUrlLength);
        QVERIFY(cut.environment.endsWith(QStringLiteral("...")));
        QVERIFY(cut.environment.size() > 1000);
        QCOMPARE(queryValue(cut.url, "environment"), cut.environment);
    }

    // ---- The dialog ---------------------------------------------------------------------

    void dialogShowsExactlyWhatIsShared()
    {
        FakeOpener opener;
        BugReportDialog dialog(sampleInfo());
        QCOMPARE(dialog.kind(), Kind::Bug);
        QCOMPARE(dialog.previewText(), bugreport::environmentText(sampleInfo().lines));
        auto* preview = dialog.findChild<QPlainTextEdit*>(QStringLiteral("BugReportPreview"));
        QVERIFY(preview && preview->isReadOnly());
        dialog.setKind(Kind::Idea);
        QCOMPARE(dialog.previewText(), QStringLiteral("Kinoklip: 1.1.2-beta.1"));
        dialog.setKind(Kind::ResolveDifference);
        QCOMPARE(dialog.previewText(), bugreport::environmentText(sampleInfo().lines));
        QVERIFY(opener.opened.isEmpty()); // showing it opens nothing
        // The note says nothing is sent by the app.
        bool noted = false;
        for (const QLabel* label : dialog.findChildren<QLabel*>())
            noted |= label->text().contains(QStringLiteral("Kinoklip itself sends nothing"));
        QVERIFY(noted);
    }

    void cancelOpensNothing()
    {
        FakeOpener opener;
        BugReportDialog dialog(sampleInfo());
        dialog.show();
        dialog.cancelButton()->click();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(opener.opened.isEmpty());
    }

    void shareOpensThePrefilledForm()
    {
        FakeOpener opener;
        BugReportDialog dialog(sampleInfo());
        dialog.show();
        dialog.setKind(Kind::ResolveDifference);
        dialog.shareButton()->click();
        QCOMPARE(opener.opened.size(), 1);
        QCOMPARE(opener.opened[0],
            bugreport::issueLink(Kind::ResolveDifference, sampleInfo(), true).url);
        QCOMPARE(queryValue(opener.opened[0], "environment"),
            bugreport::environmentText(sampleInfo().lines));
        QVERIFY(!dialog.isVisible());
    }

    void withoutOpensTheEmptyForm()
    {
        FakeOpener opener;
        BugReportDialog dialog(sampleInfo());
        dialog.show();
        dialog.withoutButton()->click();
        QCOMPARE(opener.opened.size(), 1);
        QCOMPARE(queryKeys(opener.opened[0]), QStringList {QStringLiteral("template")});
        QCOMPARE(queryValue(opener.opened[0], "template"), QStringLiteral("bug_report.yml"));
        QVERIFY(!opener.opened[0].toString().contains(QStringLiteral("Qt")));
    }

    void failureKeepsTheDialogAndShowsTheLink()
    {
        FakeOpener opener;
        opener.succeed = false;
        BugReportDialog dialog(sampleInfo());
        dialog.show();
        dialog.shareButton()->click();
        QCOMPARE(opener.opened.size(), 1);
        QVERIFY(dialog.isVisible()); // not closed: the user can retry or copy the info
        auto* message = dialog.findChild<guide::CopyLinkDialog*>();
        QVERIFY(message);
        QVERIFY(message->isVisible());
        QCOMPARE(message->linkText(), opener.opened[0].toString(QUrl::FullyEncoded));
        message->close();
    }

    void copyPutsTheTextOnTheClipboard()
    {
        FakeOpener opener;
        BugReportDialog dialog(sampleInfo());
        QGuiApplication::clipboard()->setText(QStringLiteral("before"));
        dialog.copyButton()->click();
        QCOMPARE(QGuiApplication::clipboard()->text(), dialog.previewText());
        QVERIFY(opener.opened.isEmpty());
    }

    // ---- The menu -----------------------------------------------------------------------

    void helpMenuHasTheEntryWithoutAKey()
    {
        FakeOpener opener;
        Project project;
        SelectionModel selection;
        ActionRegistry actions;
        actions.loadDefaultKeymap();
        registerStandardActions(actions);
        AppContext ctx;
        ctx.project = &project;
        ctx.selection = &selection;
        ctx.actions = &actions;
        MainWindowOptions options;
        options.autosave = false;
        options.restoreLayout = false;
        options.saveLayout = false;
        auto window = std::make_unique<MainWindow>(ctx, options);

        const QString id = QStringLiteral("help.report_bug");
        QVERIFY(actions.contains(id));
        QVERIFY(actions.shortcuts(id).isEmpty());
        QAction* action = actions.action(id);
        QCOMPARE(action->text(), QStringLiteral("&Report a Bug or Inconsistency…"));
        auto* help = window->findChild<QMenu*>(QStringLiteral("menu:help"));
        QVERIFY(help);
        QVERIFY(help->actions().contains(action));

        action->trigger();
        auto* dialog = window->findChild<BugReportDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        QVERIFY(dialog->previewText().startsWith(QStringLiteral("Kinoklip: ")));
        for (const QString& personal : personalStrings())
            QVERIFY(!dialog->previewText().contains(personal));
        QVERIFY(opener.opened.isEmpty()); // opening the dialog opens no page
        QCOMPARE(window->showBugReportDialog(), dialog); // one dialog at a time
        dialog->cancelButton()->click();
        QVERIFY(opener.opened.isEmpty());
    }

    // ---- The issue forms ----------------------------------------------------------------

    void issueFormsHaveTheFieldsTheAppFills()
    {
        for (Kind kind : {Kind::Bug, Kind::ResolveDifference, Kind::Idea}) {
            bool ok = false;
            const QStringList ids = formIds(bugreport::templateFile(kind), &ok);
            QVERIFY2(ok, qPrintable(bugreport::templateFile(kind)));
            QCOMPARE(QSet<QString>(ids.begin(), ids.end()).size(), ids.size()); // unique
            QVERIFY(ids.contains(QLatin1String(bugreport::kVersionField)));
            QCOMPARE(ids.contains(QLatin1String(bugreport::kEnvironmentField)),
                bugreport::hasEnvironment(kind));
        }
        bool ok = false;
        const QStringList bug = formIds(QStringLiteral("bug_report.yml"), &ok);
        for (const char* id : {"page", "steps", "actual", "expected", "frequency", "attachments"})
            QVERIFY2(bug.contains(QLatin1String(id)), id);
        QFile bugFile(
            QStringLiteral(KK_TEST_DATA_DIR "/../../.github/ISSUE_TEMPLATE/bug_report.yml"));
        QVERIFY(bugFile.open(QIODevice::ReadOnly));
        QVERIFY(bugFile.readAll().contains("title: \"[Bug] \""));
    }
};

QTEST_MAIN(TestBugReport)
#include "test_bugreport.moc"
