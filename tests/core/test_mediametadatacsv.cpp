// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for core/MediaMetadataCsv (spec 166): the Media Pool's clip metadata as a CSV file and
// back, matching by file name and folder, partial files, quoting and bad input.
#include "core/MediaMetadataCsv.h"

#include <QTest>

using namespace kk;
using namespace kk::mediametadata;

namespace {

QString add(MediaPool& pool, const QString& path)
{
    MediaItem item;
    item.path = path;
    item.name = path.section(QLatin1Char('/'), -1);
    item.info.valid = true;
    return pool.addItem(item);
}

} // namespace

class TestMediaMetadataCsv : public QObject
{
    Q_OBJECT
private slots:
    void roundTrip()
    {
        MediaPool a;
        const QString id = add(a, QStringLiteral("/m/day1/A001.mov"));
        add(a, QStringLiteral("/m/day1/B002.mov"));
        MediaMetadata md;
        md.description = QStringLiteral("Wide, \"bay\"\nsecond line");
        md.comments = QStringLiteral("use, this");
        md.keywords = {QStringLiteral("beach"), QStringLiteral("sun")};
        md.scene = QStringLiteral("12");
        md.shot = QStringLiteral("A");
        md.take = QStringLiteral("3");
        md.camera = QStringLiteral("2");
        md.angle = QStringLiteral("B");
        md.goodTake = true;
        md.rating = 4;
        a.setMetadata(id, md);
        a.setClipColor(id, QStringLiteral("orange"));
        a.setFlags(id, 0b101);
        const QString csv = toCsv(a);
        QVERIFY(csv.startsWith(QStringLiteral("File Name,Clip Directory,Description,")));
        QVERIFY(csv.contains(QStringLiteral("\r\n")));

        MediaPool b;
        const QString idb = add(b, QStringLiteral("/m/day1/A001.mov"));
        add(b, QStringLiteral("/m/day1/B002.mov"));
        const ImportReport r = applyCsv(b, csv);
        QVERIFY(r.ok());
        QCOMPARE(r.rows, 2);
        QCOMPARE(r.updated, 1);
        QCOMPARE(r.unchanged, 1);
        const MediaItem* item = b.item(idb);
        QCOMPARE(item->metadata.description, a.item(id)->metadata.description);
        QCOMPARE(item->metadata.keywords, a.item(id)->metadata.keywords);
        QCOMPARE(item->metadata.rating, 4);
        QVERIFY(item->metadata.goodTake);
        QCOMPARE(item->clipColor, QStringLiteral("orange"));
        QCOMPARE(item->flags, quint32(0b101));
        // Importing the same file again changes nothing.
        QCOMPARE(applyCsv(b, csv).updated, 0);
    }

    void partialFileKeepsTheRest()
    {
        MediaPool p;
        const QString id = add(p, QStringLiteral("/m/a.mov"));
        MediaMetadata md;
        md.comments = QStringLiteral("keep me");
        md.scene = QStringLiteral("1");
        p.setMetadata(id, md);
        const ImportReport r = applyCsv(p, QStringLiteral("﻿name,SHOT\n,y\n"));
        QVERIFY(!r.ok()); // no File Name column
        const ImportReport r2 = applyCsv(p,
            QStringLiteral("File Name,shot,Scene,Clip Color\na.mov,7,,Nope\nghost.mov,1,,\n"));
        QCOMPARE(r2.updated, 1);
        QCOMPARE(r2.unmatched, QStringList {QStringLiteral("ghost.mov")});
        QCOMPARE(p.item(id)->metadata.shot, QStringLiteral("7"));
        QCOMPARE(p.item(id)->metadata.scene, QStringLiteral("1")); // empty cell keeps it
        QCOMPARE(p.item(id)->metadata.comments, QStringLiteral("keep me"));
        QVERIFY(p.item(id)->clipColor.isEmpty()); // unknown colour ignored
    }

    void sameNameInTwoFolders()
    {
        MediaPool p;
        const QString one = add(p, QStringLiteral("/m/day1/C.mov"));
        const QString two = add(p, QStringLiteral("/m/day2/C.mov"));
        ImportReport r = applyCsv(p, QStringLiteral("File Name,Scene\nC.mov,5\n"));
        QCOMPARE(r.ambiguous, QStringList {QStringLiteral("C.mov")});
        QCOMPARE(r.updated, 0);
        r = applyCsv(p, QStringLiteral("File Name,Clip Directory,Scene\nC.mov,/m/day2/,5\n"));
        QCOMPARE(r.updated, 1);
        QCOMPARE(p.item(two)->metadata.scene, QStringLiteral("5"));
        QVERIFY(p.item(one)->metadata.scene.isEmpty());
    }

    void badInput()
    {
        MediaPool p;
        QVERIFY(!applyCsv(p, QString()).ok());
        QVERIFY(!applyCsv(p, QStringLiteral("Scene,Shot\n1,2\n")).ok());
        const QList<QStringList> rows
            = parseCsv(QStringLiteral("a,\"b\"\"c\"\r\n\r\nd,\"e\nf\"\r"));
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0], (QStringList {QStringLiteral("a"), QStringLiteral("b\"c")}));
        QCOMPARE(rows[1], (QStringList {QStringLiteral("d"), QStringLiteral("e\nf")}));
    }
};

QTEST_MAIN(TestMediaMetadataCsv)
#include "test_mediametadatacsv.moc"
