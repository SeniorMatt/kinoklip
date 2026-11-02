// SPDX-License-Identifier: GPL-3.0-or-later
// ChildProcessEnv tests: the launcher's environment changes are undone for child processes.
#include "platform/ChildProcessEnv.h"

#include <QProcess>
#include <QTest>

using namespace kk;

class TestChildProcessEnv : public QObject
{
    Q_OBJECT
private slots:
    void notBundledIsUntouched()
    {
        qunsetenv("KINOKLIP_ORIG_VARS");
        qputenv("KK_TEST_VALUE", "1");
        QVERIFY(platform::launcherChangedVariables().isEmpty());
        QCOMPARE(platform::childEnvironment().value(QStringLiteral("KK_TEST_VALUE")),
            QStringLiteral("1"));
    }

    void bundledRestoresOriginals()
    {
        // What AppRun does: LD_LIBRARY_PATH was unset, XDG_DATA_DIRS had a value.
        qputenv("KINOKLIP_ORIG_VARS", "LD_LIBRARY_PATH XDG_DATA_DIRS");
        qputenv("KINOKLIP_ORIG_XDG_DATA_DIRS", "/usr/local/share:/usr/share");
        qunsetenv("KINOKLIP_ORIG_LD_LIBRARY_PATH");
        qputenv("LD_LIBRARY_PATH", "/bundle/usr/lib/qt");
        qputenv("XDG_DATA_DIRS", "/bundle/usr/share:/usr/local/share:/usr/share");
        qputenv("KINOKLIP_APPDIR", "/bundle");

        const QProcessEnvironment env = platform::childEnvironment();
        QVERIFY(!env.contains(QStringLiteral("LD_LIBRARY_PATH")));
        QCOMPARE(env.value(QStringLiteral("XDG_DATA_DIRS")),
            QStringLiteral("/usr/local/share:/usr/share"));
        QVERIFY(!env.contains(QStringLiteral("KINOKLIP_APPDIR")));
        QVERIFY(!env.contains(QStringLiteral("KINOKLIP_ORIG_VARS")));

        // A real child sees exactly that.
        QProcess sh;
        platform::prepareChildProcess(sh);
        sh.start(QStringLiteral("/bin/sh"),
            {QStringLiteral("-c"),
                QStringLiteral("echo \"${LD_LIBRARY_PATH-unset}|$XDG_DATA_DIRS\"")});
        QVERIFY(sh.waitForFinished(5000));
        QCOMPARE(
            sh.readAllStandardOutput().trimmed(), QByteArray("unset|/usr/local/share:/usr/share"));

        // The scoped variant changes our own environment only temporarily.
        {
            const platform::ScopedOriginalEnvironment original;
            QVERIFY(!qEnvironmentVariableIsSet("LD_LIBRARY_PATH"));
            QCOMPARE(qgetenv("XDG_DATA_DIRS"), QByteArray("/usr/local/share:/usr/share"));
        }
        QCOMPARE(qgetenv("LD_LIBRARY_PATH"), QByteArray("/bundle/usr/lib/qt"));
        QCOMPARE(
            qgetenv("XDG_DATA_DIRS"), QByteArray("/bundle/usr/share:/usr/local/share:/usr/share"));
    }
};

QTEST_GUILESS_MAIN(TestChildProcessEnv)
#include "test_childprocessenv.moc"
