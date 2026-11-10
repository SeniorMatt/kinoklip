// SPDX-License-Identifier: GPL-3.0-or-later
// The in-memory session log (spec section 64): bounds of the ring buffer, the dropped-lines
// count, what is kept, and that the message handler leaves the output of the previous handler
// (and so stderr) exactly as it was.
#include "core/SessionLog.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QProcess>
#include <QTest>

using namespace kk::sessionlog;

Q_LOGGING_CATEGORY(lcTestKk, "kk.test.sessionlog", QtInfoMsg)
Q_LOGGING_CATEGORY(lcTestOther, "other.sessionlog", QtInfoMsg)

namespace {

struct Seen
{
    QtMsgType type;
    QString category;
    QString text;
};
QList<Seen> g_seen; ///< what reached the handler that was installed before ours
QtMessageHandler g_before = nullptr;

void recordingHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    g_seen.append({type, QString::fromLatin1(context.category), message});
}

/// The child process of stderrOutputIsUnchanged(): prints a few messages with Qt's own handler,
/// with or without the session log installed in front of it.
int childMain(const QByteArray& mode)
{
    if (mode == "logged")
        installMessageHandler();
    qWarning("a warning %d", 1);
    qInfo("a plain info");
    qCInfo(lcTestKk) << "info of a kk category";
    qCInfo(lcTestOther) << "info of another category";
    qCWarning(lcTestOther) << "warning of another category";
    qCDebug(lcTestKk) << "debug is off by default";
    qCritical("a critical one");
    return 0;
}

} // namespace

class TestSessionLog : public QObject
{
    Q_OBJECT
private slots:
    // 64.3: bounded by lines, oldest dropped and counted.
    void ringBufferKeepsTheLastLines()
    {
        RingBuffer b(5, 1 << 20);
        for (int i = 0; i < 8; ++i)
            b.append(QtInfoMsg, QStringLiteral("kk.test"), QStringLiteral("m%1").arg(i));
        const QList<Entry> e = b.entries();
        QCOMPARE(e.size(), 5);
        QCOMPARE(e.first().text, QStringLiteral("m3"));
        QCOMPARE(e.last().text, QStringLiteral("m7"));
        QCOMPARE(b.dropped(), qint64(3));
        b.clear();
        QCOMPARE(b.entries().size(), 0);
        QCOMPARE(b.dropped(), qint64(0));
    }

    // 64.3: bounded by bytes as well.
    void ringBufferKeepsAtMostTheByteLimit()
    {
        RingBuffer b(1000, 400);
        for (int i = 0; i < 40; ++i)
            b.append(
                QtWarningMsg, QStringLiteral("kk.test"), QString(30, QLatin1Char('a' + i % 26)));
        QVERIFY(b.bytes() <= 400);
        QVERIFY(b.dropped() > 0);
        QVERIFY(b.entries().size() < 40);
        QCOMPARE(b.entries().last().text, QString(30, QLatin1Char('a' + 39 % 26)));
        QCOMPARE(qint64(b.entries().size()) + b.dropped(), qint64(40));
    }

    // 64.3: the real limits and one huge message.
    void defaultLimitsAndLongMessages()
    {
        QCOMPARE(kMaxLines, 20000);
        QCOMPARE(kMaxBytes, qint64(4 * 1024 * 1024));
        RingBuffer b;
        b.append(QtWarningMsg, QStringLiteral("kk.test"), QString(100000, QLatin1Char('x')));
        QVERIFY(b.entries().first().text.size() <= kMaxMessageChars + 3);
        QVERIFY(b.entries().first().text.endsWith(QStringLiteral("...")));
        for (int i = 0; i < kMaxLines + 10; ++i)
            b.append(QtInfoMsg, QStringLiteral("kk.test"), QStringLiteral("l"));
        QCOMPARE(b.entries().size(), kMaxLines);
        QVERIFY(b.dropped() >= 11);
    }

    // 64.3: what is kept.
    void wantedMessages()
    {
        QVERIFY(wanted(QtWarningMsg, "default"));
        QVERIFY(wanted(QtCriticalMsg, "qt.qpa.xcb"));
        QVERIFY(wanted(QtFatalMsg, nullptr));
        QVERIFY(wanted(QtInfoMsg, "kk.playback"));
        QVERIFY(wanted(QtInfoMsg, "kk.ui.viewer.gl"));
        QVERIFY(wanted(QtInfoMsg, "kinoklip.filedialogs"));
        QVERIFY(!wanted(QtInfoMsg, "qt.qpa.xcb"));
        QVERIFY(!wanted(QtInfoMsg, "default"));
        QVERIFY(!wanted(QtDebugMsg, "kk.media"));
        QVERIFY(!wanted(QtInfoMsg, nullptr));
    }

    // 64.3: record() is the path of lines that are not printed.
    void recordKeepsWantedLinesOnly()
    {
        buffer().clear();
        record(QtInfoMsg, "kk.playback", QStringLiteral("stats: 1"));
        record(QtDebugMsg, "kk.playback", QStringLiteral("not kept"));
        record(QtInfoMsg, "elsewhere", QStringLiteral("not kept either"));
        QCOMPARE(buffer().entries().size(), 1);
        QCOMPARE(buffer().entries().first().category, QStringLiteral("kk.playback"));
        buffer().clear();
    }

    // 64.4: messages are kept and still reach the previous handler, unchanged.
    void handlerKeepsAndChains()
    {
        g_before = qInstallMessageHandler(recordingHandler);
        installMessageHandler();
        installMessageHandler(); // twice: still one layer
        buffer().clear();
        g_seen.clear();

        qWarning("warning %d", 7);
        qCInfo(lcTestKk) << "kk info";
        qCInfo(lcTestOther) << "other info";
        qCWarning(lcTestOther) << "other warning";
        qDebug("plain debug");

        // The previous handler saw every message, in order, with its own type and text.
        QCOMPARE(g_seen.size(), 5);
        QCOMPARE(g_seen[0].text, QStringLiteral("warning 7"));
        QCOMPARE(g_seen[1].category, QStringLiteral("kk.test.sessionlog"));
        QCOMPARE(g_seen[2].text, QStringLiteral("other info"));
        QCOMPARE(g_seen[4].type, QtDebugMsg);

        // The log kept the warnings and the kk info only.
        const QList<Entry> kept = buffer().entries();
        QCOMPARE(kept.size(), 3);
        QCOMPARE(kept[0].text, QStringLiteral("warning 7"));
        QCOMPARE(kept[0].type, QtWarningMsg);
        QCOMPARE(kept[1].category, QStringLiteral("kk.test.sessionlog"));
        QCOMPARE(kept[1].text, QStringLiteral("kk info"));
        QCOMPARE(kept[2].text, QStringLiteral("other warning"));
        QVERIFY(kept[2].milliseconds >= kept[0].milliseconds);
    }

    // 64.4: with Qt's own handler behind it (nothing installed before) stderr is byte for byte
    // what it is without the session log.
    void stderrOutputIsUnchanged()
    {
        auto run = [](const char* mode) {
            QProcess p;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("KK_SESSIONLOG_CHILD"), QString::fromLatin1(mode));
            env.insert(QStringLiteral("QT_MESSAGE_PATTERN"),
                QStringLiteral("%{type}|%{category}|%{message}"));
            env.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
            env.remove(QStringLiteral("QT_LOGGING_RULES"));
            p.setProcessEnvironment(env);
            p.start(QCoreApplication::applicationFilePath(), {});
            p.waitForFinished(30000);
            return p.readAllStandardError();
        };
        const QByteArray plain = run("plain");
        const QByteArray logged = run("logged");
        QVERIFY2(plain.contains("warning|default|a warning 1"), plain.constData());
        QVERIFY(plain.contains("info of a kk category"));
        QVERIFY(!plain.contains("debug is off"));
        QCOMPARE(logged, plain);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (const QByteArray mode = qgetenv("KK_SESSIONLOG_CHILD"); !mode.isEmpty())
        return childMain(mode);
    TestSessionLog test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_sessionlog.moc"
