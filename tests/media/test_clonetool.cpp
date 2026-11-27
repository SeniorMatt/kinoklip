// SPDX-License-Identifier: GPL-3.0-or-later
// test_clonetool — the Media page's Clone Tool (spec 196): copies with checksums, several
// destinations, the checksum list, no overwriting, the command and the dialog.
#include "app/commands/Command.h"
#include "media/CloneTool.h"
#include "ui/dialogs/CloneToolDialog.h"

#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;

namespace {

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

/// A fake camera card: two clips (one bigger than the 4 MiB copy block) and a sidecar.
void makeCard(const QString& root)
{
    QByteArray big(9 * 1024 * 1024 + 123, 0);
    for (int i = 0; i < big.size(); ++i)
        big[i] = char((i * 31) ^ (i >> 8));
    writeFile(root + QStringLiteral("/A001/C0001.MXF"), big);
    writeFile(root + QStringLiteral("/A001/sub/C0002.MXF"), QByteArray("small clip"));
    writeFile(root + QStringLiteral("/A001/.hidden.xml"), QByteArray("<x/>"));
}

} // namespace

class TestCloneTool : public QObject
{
    Q_OBJECT
private slots:
    void copiesToTwoDestinationsAndVerifies()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString card = dir.filePath(QStringLiteral("card"));
        makeCard(card);
        clone::Job job;
        job.sources = {card + QStringLiteral("/A001")};
        job.destinations = {dir.filePath(QStringLiteral("d1")), dir.filePath(QStringLiteral("d2"))};
        qint64 lastDone = 0;
        qint64 lastTotal = 0;
        const clone::Result r = clone::run(job, [&](qint64 done, qint64 total) {
            lastDone = done;
            lastTotal = total;
            return true;
        });
        QVERIFY2(r.ok, qPrintable(r.failures.join('\n') + r.error));
        QCOMPARE(r.files, 3);
        QCOMPARE(r.verified, 6); // 3 files x 2 destinations
        QCOMPARE(lastDone, lastTotal);
        for (const QString& d : job.destinations) {
            for (const QString& rel : {QStringLiteral("A001/C0001.MXF"),
                     QStringLiteral("A001/sub/C0002.MXF"), QStringLiteral("A001/.hidden.xml")})
                QCOMPARE(
                    readFile(d + QLatin1Char('/') + rel), readFile(card + QLatin1Char('/') + rel));
        }
        // The list is in md5sum format and agrees with a real hash of the copy.
        const QByteArray list = readFile(job.destinations[0] + QStringLiteral("/A001/A001.md5"));
        const QString expected = QString::fromLatin1(QCryptographicHash::hash(
            readFile(card + QStringLiteral("/A001/sub/C0002.MXF")), QCryptographicHash::Md5)
                                                         .toHex());
        QVERIFY(list.contains((expected + QStringLiteral("  sub/C0002.MXF")).toUtf8()));
        // The time of the source is kept.
        QCOMPARE(
            QFileInfo(job.destinations[1] + QStringLiteral("/A001/sub/C0002.MXF")).lastModified(),
            QFileInfo(card + QStringLiteral("/A001/sub/C0002.MXF")).lastModified());
        // md5sum -c agrees (when the tool exists).
        QProcess check;
        check.setWorkingDirectory(job.destinations[1] + QStringLiteral("/A001"));
        check.start(QStringLiteral("md5sum"), {QStringLiteral("-c"), QStringLiteral("A001.md5")});
        if (check.waitForStarted()) {
            QVERIFY(check.waitForFinished());
            QCOMPARE(check.exitCode(), 0);
        }
    }

    void everyChecksumType()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("clip.bin"));
        writeFile(file, QByteArray("hello world"));
        // Known answers for "hello world".
        QCOMPARE(clone::fileChecksum(file, clone::Checksum::Md5),
            QStringLiteral("5eb63bbbe01eeed093cb22bb8f5acdc3"));
        QCOMPARE(clone::fileChecksum(file, clone::Checksum::Sha1),
            QStringLiteral("2aae6c35c94fcfb415dbe95f408b9ce91ee846ed"));
        QCOMPARE(clone::fileChecksum(file, clone::Checksum::Sha256),
            QStringLiteral("b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9"));
        QCOMPARE(clone::fileChecksum(file, clone::Checksum::Crc32), QStringLiteral("0d4a1185"));
        clone::Checksum c = clone::Checksum::Md5;
        QVERIFY(clone::parseChecksum(QStringLiteral("SHA-256"), c));
        QCOMPARE(c, clone::Checksum::Sha256);
        QVERIFY(!clone::parseChecksum(QStringLiteral("xxh"), c));
        // A single file source: the list lies next to the copy.
        clone::Job job;
        job.sources = {file};
        job.destinations = {dir.filePath(QStringLiteral("out"))};
        job.checksum = clone::Checksum::Sha1;
        QVERIFY(clone::run(job).ok);
        QVERIFY(readFile(job.destinations[0] + QStringLiteral("/clip.bin.sha1"))
                    .startsWith("2aae6c35c94fcfb415dbe95f408b9ce91ee846ed  clip.bin"));
    }

    void neverOverwrites()
    {
        QTemporaryDir dir;
        const QString card = dir.filePath(QStringLiteral("card"));
        makeCard(card);
        const QString target = dir.filePath(QStringLiteral("d1"));
        writeFile(target + QStringLiteral("/A001/sub/C0002.MXF"), QByteArray("mine"));
        clone::Job job;
        job.sources = {card + QStringLiteral("/A001")};
        job.destinations = {target};
        const clone::Result r = clone::run(job);
        QVERIFY(!r.ok);
        QCOMPARE(r.failures.size(), 1);
        QVERIFY(r.failures.first().contains(QStringLiteral("already exists")));
        QCOMPARE(readFile(target + QStringLiteral("/A001/sub/C0002.MXF")), QByteArray("mine"));
        QCOMPARE(
            readFile(target + QStringLiteral("/A001/C0001.MXF")).size(), 9 * 1024 * 1024 + 123);
    }

    void jobErrorsAndCancel()
    {
        QTemporaryDir dir;
        clone::Job job;
        QVERIFY(!clone::run(job).error.isEmpty()); // no source
        job.sources = {dir.filePath(QStringLiteral("missing"))};
        job.destinations = {dir.filePath(QStringLiteral("d"))};
        QVERIFY(clone::run(job).error.contains(QStringLiteral("does not exist")));
        const QString card = dir.filePath(QStringLiteral("card"));
        makeCard(card);
        job.sources = {card};
        const clone::Result r = clone::run(job, [](qint64, qint64) { return false; });
        QVERIFY(r.cancelled);
        QVERIFY(!r.ok);
        // A cancelled copy leaves no half-written file (QSaveFile).
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("d/card/A001/C0001.MXF"))));
    }

    void commandCopiesAndReports()
    {
        QTemporaryDir dir;
        const QString card = dir.filePath(QStringLiteral("card"));
        makeCard(card);
        const commands::CommandResult result
            = commands::CommandRegistry::builtin().execute(QStringLiteral("media.clone"),
                {{QStringLiteral("source"), QJsonArray {card}},
                    {QStringLiteral("destination"), QJsonArray {dir.filePath(QStringLiteral("o"))}},
                    {QStringLiteral("checksum"), QStringLiteral("sha256")}});
        QVERIFY2(result.errorCode.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.data.value(QStringLiteral("files")).toInt(), 3);
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("o/card/card.sha256"))));
    }

    void dialogRunsAJob()
    {
        QTemporaryDir dir;
        const QString card = dir.filePath(QStringLiteral("card"));
        makeCard(card);
        CloneToolDialog dialog;
        auto* button = dialog.findChild<QPushButton*>(QStringLiteral("cloneStart"));
        QVERIFY(button);
        QVERIFY(!button->isEnabled()); // nothing added yet
        dialog.addSource(card);
        dialog.addSource(card); // the same folder twice stays one entry
        QVERIFY(!button->isEnabled());
        dialog.addDestination(dir.filePath(QStringLiteral("out1")));
        QVERIFY(button->isEnabled());
        QCOMPARE(dialog.job().sources.size(), 1);
        QSignalSpy spy(&dialog, &CloneToolDialog::cloneFinished);
        QVERIFY(dialog.start());
        QVERIFY(!dialog.start()); // already running
        QVERIFY(spy.wait(20000));
        QVERIFY(dialog.result().ok);
        QCOMPARE(dialog.result().files, 3);
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("out1/card/A001/C0001.MXF"))));
        QVERIFY(button->isEnabled());
    }
};

QTEST_MAIN(TestCloneTool)
#include "test_clonetool.moc"
