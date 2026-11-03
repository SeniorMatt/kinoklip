// SPDX-License-Identifier: GPL-3.0-or-later
// test_folderscan — media::scanMediaFolder() of File > Import Media Folder (spec 33.25 - 33.27):
// natural order, hidden / junk / unsupported files skipped, subfolders on and off, numbered
// stills folded into one image sequence, symlink loops, cancel.
#include "media/FolderScan.h"
#include "media/ImageSequence.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

using namespace kk;
using namespace kk::media;

namespace {

/// Creates an empty file (the scan only looks at names).
void touch(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
}

QStringList names(const QStringList& paths)
{
    QStringList out;
    for (const QString& p : paths)
        out.append(QFileInfo(p).fileName());
    return out;
}

} // namespace

class TestFolderScan : public QObject
{
    Q_OBJECT
private slots:
    void naturalOrder()
    {
        QStringList list {QStringLiteral("clip10.mp4"), QStringLiteral("clip2.mp4"),
            QStringLiteral("Clip1.mp4"), QStringLiteral("clip02.mp4"), QStringLiteral("a.mp4"),
            QStringLiteral("clip.mp4"), QStringLiteral("clip99999999999999999999.mp4"),
            QStringLiteral("clip100000000000000000000.mp4")};
        std::sort(list.begin(), list.end(),
            [](const QString& a, const QString& b) { return naturalLess(a, b); });
        const QStringList expected {QStringLiteral("a.mp4"), QStringLiteral("clip.mp4"),
            QStringLiteral("Clip1.mp4"), QStringLiteral("clip2.mp4"), QStringLiteral("clip02.mp4"),
            QStringLiteral("clip10.mp4"), QStringLiteral("clip99999999999999999999.mp4"),
            QStringLiteral("clip100000000000000000000.mp4")};
        QCOMPARE(list, expected);
        QCOMPARE(naturalCompare(u"x", u"x"), 0);
        QVERIFY(naturalCompare(u"A", u"a") != 0); // total order: case decides a tie
    }

    void hiddenJunkAndUnsupportedAreSkipped()
    {
        QTemporaryDir dir;
        touch(dir.filePath(QStringLiteral("a.mp4")));
        touch(dir.filePath(QStringLiteral("B.MOV"))); // extension case does not matter
        touch(dir.filePath(QStringLiteral(".hidden.mp4")));
        touch(dir.filePath(QStringLiteral(".DS_Store")));
        touch(dir.filePath(QStringLiteral("Thumbs.db")));
        touch(dir.filePath(QStringLiteral("desktop.ini")));
        touch(dir.filePath(QStringLiteral("notes.txt")));
        touch(dir.filePath(QStringLiteral(".cache/inside.mp4")));
        const FolderScanResult r = scanMediaFolder(dir.path());
        QVERIFY(r.ok);
        QCOMPARE(
            names(r.root.files), (QStringList {QStringLiteral("a.mp4"), QStringLiteral("B.MOV")}));
        QCOMPARE(r.skippedHidden, 5); // .hidden.mp4 .DS_Store Thumbs.db desktop.ini .cache
        QCOMPARE(r.skippedUnsupported, 1);
        QVERIFY(r.root.subfolders.empty());
        QVERIFY(isHiddenOrJunkName(u"THUMBS.DB"));
        QVERIFY(!isHiddenOrJunkName(u"thumbs.mp4"));
    }

    void subfoldersOnAndOff()
    {
        QTemporaryDir dir;
        touch(dir.filePath(QStringLiteral("top.wav")));
        touch(dir.filePath(QStringLiteral("Day10/x.mp4")));
        touch(dir.filePath(QStringLiteral("Day2/y.mp4")));
        touch(dir.filePath(QStringLiteral("Day2/Cam B/z.mp4")));
        FolderScanResult r = scanMediaFolder(dir.path());
        QVERIFY(r.ok);
        QCOMPARE(r.root.totalFiles(), 4);
        QCOMPARE(int(r.root.subfolders.size()), 2);
        QCOMPARE(r.root.subfolders[0].name, QStringLiteral("Day2")); // natural order
        QCOMPARE(r.root.subfolders[1].name, QStringLiteral("Day10"));
        QCOMPARE(r.root.subfolders[0].subfolders.at(0).name, QStringLiteral("Cam B"));

        FolderScanOptions top;
        top.includeSubfolders = false;
        r = scanMediaFolder(dir.path(), top);
        QVERIFY(r.ok);
        QCOMPARE(r.root.totalFiles(), 1);
        QVERIFY(r.root.subfolders.empty());
    }

    void numberedStillsBecomeOneSequence()
    {
        QTemporaryDir dir;
        for (int i = 1; i <= 5; ++i)
            touch(dir.filePath(QStringLiteral("shot_%1.png").arg(i, 4, 10, QLatin1Char('0'))));
        touch(dir.filePath(QStringLiteral("other.png")));
        touch(dir.filePath(QStringLiteral("IMG_7.jpg"))); // numbered, but alone
        touch(dir.filePath(QStringLiteral("take1.mp4"))); // videos are never folded
        touch(dir.filePath(QStringLiteral("take2.mp4")));
        FolderScanOptions options;
        options.sequenceFrameRate = {24, 1};
        const FolderScanResult r = scanMediaFolder(dir.path(), options);
        QVERIFY(r.ok);
        QCOMPARE(r.root.files.size(), 5);
        QCOMPARE(names(r.root.files.mid(0, 2)),
            (QStringList {QStringLiteral("IMG_7.jpg"), QStringLiteral("other.png")}));
        const QString seq = r.root.files[2];
        QVERIFY2(ImageSequence::isSequencePath(seq), qPrintable(seq));
        QString pattern;
        Rational fps;
        QVERIFY(ImageSequence::parsePath(seq, &pattern, &fps));
        QCOMPARE(QFileInfo(pattern).fileName(), QStringLiteral("shot_%04d.png"));
        QCOMPARE(fps, (Rational {24, 1}));
        QCOMPARE(names(r.root.files.mid(3)),
            (QStringList {QStringLiteral("take1.mp4"), QStringLiteral("take2.mp4")}));

        FolderScanOptions off;
        off.detectSequences = false;
        QCOMPARE(scanMediaFolder(dir.path(), off).root.files.size(), 9);
    }

    void symlinkLoopEnds()
    {
        QTemporaryDir dir;
        touch(dir.filePath(QStringLiteral("a/clip.mp4")));
        // a/back -> the scanned folder itself, a/self -> a: both loops.
        QVERIFY(QFile::link(dir.path(), dir.filePath(QStringLiteral("a/back"))));
        QVERIFY(
            QFile::link(dir.filePath(QStringLiteral("a")), dir.filePath(QStringLiteral("a/self"))));
        // A link to a folder seen nowhere else is followed.
        QTemporaryDir outside;
        touch(outside.filePath(QStringLiteral("linked.mp4")));
        QVERIFY(QFile::link(outside.path(), dir.filePath(QStringLiteral("elsewhere"))));
        const FolderScanResult r = scanMediaFolder(dir.path());
        QVERIFY(r.ok);
        QCOMPARE(r.root.totalFiles(), 2);
        QCOMPARE(r.skippedLinks, 2);
    }

    void cancel()
    {
        QTemporaryDir dir;
        for (int i = 0; i < 300; ++i)
            touch(dir.filePath(QStringLiteral("f%1.mp4").arg(i)));
        int calls = 0;
        const FolderScanResult r = scanMediaFolder(dir.path(), {}, [&](int) {
            ++calls;
            return false;
        });
        QVERIFY(r.cancelled);
        QVERIFY(!r.ok);
        QCOMPARE(calls, 1);
        QCOMPARE(r.root.totalFiles(), 0);
        QVERIFY(!scanMediaFolder(dir.filePath(QStringLiteral("missing"))).ok);
    }
};

QTEST_GUILESS_MAIN(TestFolderScan)
#include "test_folderscan.moc"
