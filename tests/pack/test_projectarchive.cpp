// SPDX-License-Identifier: GPL-3.0-or-later
// test_projectarchive — Export Project Archive / Restore Project Archive (spec 175): the .dra
// folder with the project and its media, optional proxy media and render cache, image
// sequences, LUTs, media inside a single-file project, refusing to overwrite, cancelling, and
// restoring to another place with the media pointing at the copies.
#include "pack/EmbeddedMedia.h"
#include "pack/ProjectArchive.h"
#include "pack/ProjectPackage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::pack;

namespace {

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QJsonObject readJson(const QString& path)
{
    return QJsonDocument::fromJson(readFile(path)).object();
}

QJsonObject item(const QString& id, const QString& path, const QString& relative = {})
{
    QJsonObject o {{QStringLiteral("id"), id}, {QStringLiteral("name"), QFileInfo(path).fileName()},
        {QStringLiteral("path"), path}};
    if (!relative.isEmpty())
        o[QStringLiteral("relativePath")] = relative;
    return o;
}

QJsonArray itemsOf(const QJsonObject& root)
{
    return root.value(QStringLiteral("mediaPool")).toObject().value(QStringLiteral("items")).toArray();
}

} // namespace

class TestProjectArchive : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_project; ///< linked project: a.mp4, b.wav (found through relativePath), a sequence,
                       ///< an offline file, a LUT, a proxy link
    QString m_cacheFile;
    QByteArray m_json;

    QString path(const QString& name) const { return m_dir.filePath(name); }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY(writeFile(path(QStringLiteral("src/a.mp4")), QByteArray(5000, 'a')));
        QVERIFY(writeFile(path(QStringLiteral("src/sub/b.wav")), QByteArray(300, 'b')));
        for (int i = 1; i <= 3; ++i)
            QVERIFY(writeFile(path(QStringLiteral("src/seq/img_%1.png").arg(i, 3, 10, QLatin1Char('0'))),
                QByteArray(40 + i, 's')));
        QVERIFY(writeFile(path(QStringLiteral("src/seq/other.png")), QByteArray(10, 'o')));
        QVERIFY(writeFile(path(QStringLiteral("src/look.cube")), QByteArray("LUT_3D_SIZE 2\n")));
        QVERIFY(writeFile(path(QStringLiteral("proxies/a_proxy.mov")), QByteArray(700, 'p')));
        QVERIFY(writeFile(path(QStringLiteral("cache/0123.mov")), QByteArray(900, 'c')));
        m_cacheFile = path(QStringLiteral("cache/0123.mov"));
        // The project sits next to src/: "b.wav" is only found through its relative path.
        const QString moved = path(QStringLiteral("moved-away/b.wav"));
        const QJsonObject root {
            {QStringLiteral("mediaPool"),
                QJsonObject {{QStringLiteral("items"),
                    QJsonArray {item(QStringLiteral("m1"), path(QStringLiteral("src/a.mp4")),
                                    QStringLiteral("src/a.mp4")),
                        item(QStringLiteral("m2"), moved, QStringLiteral("src/sub/b.wav")),
                        item(QStringLiteral("m3"), path(QStringLiteral("src/seq/img_%03d.png"))),
                        item(QStringLiteral("m4"), path(QStringLiteral("gone/offline.mov"))),
                        // The same file twice: stored once.
                        item(QStringLiteral("m5"), path(QStringLiteral("src/a.mp4"))),
                        QJsonObject {{QStringLiteral("id"), QStringLiteral("t1")},
                            {QStringLiteral("kind"), QStringLiteral("timeline")},
                            {QStringLiteral("path"), QStringLiteral("ignored")}}}}}},
            {QStringLiteral("timelines"),
                QJsonArray {QJsonObject {{QStringLiteral("nodes"),
                    QJsonArray {QJsonObject {
                        {QStringLiteral("lut"), path(QStringLiteral("src/look.cube"))}}}}}}},
            {QStringLiteral("mediaLinks"),
                QJsonArray {QJsonObject {{QStringLiteral("mediaId"), QStringLiteral("m1")},
                    {QStringLiteral("proxy"),
                        QJsonObject {{QStringLiteral("path"),
                                         path(QStringLiteral("proxies/a_proxy.mov"))},
                            {QStringLiteral("format"), QStringLiteral("h264")}}}}}}};
        m_json = QJsonDocument(root).toJson();
        m_project = path(QStringLiteral("film.kkproj"));
        QVERIFY(writeFile(m_project, m_json));
    }

    void exportCopiesProjectAndMedia()
    {
        const QString folder = path(QStringLiteral("out/film.dra"));
        qint64 lastDone = 0;
        qint64 lastTotal = 0;
        const ArchiveResult r = exportArchive(m_json, path(QString()), QStringLiteral("film"), folder,
            {}, [&](qint64 done, qint64 total) {
                lastDone = done;
                lastTotal = total;
                return true;
            });
        QVERIFY2(r.status == Status::Ok, qPrintable(r.error));
        QVERIFY(isArchiveFolder(folder));
        QCOMPARE(r.mediaFiles, 2 + 3); // a.mp4 once, b.wav, three frames
        QCOMPARE(r.lutFiles, 1);
        QCOMPARE(r.proxyFiles, 0);
        QCOMPARE(r.skipped.size(), 1);
        QVERIFY(r.skipped.front().contains(QLatin1String("offline")));
        QVERIFY(lastTotal > 0);
        QCOMPARE(lastDone, lastTotal);
        QCOMPARE(r.projectFile, QDir(folder).absoluteFilePath(QStringLiteral("film.kkproj")));

        // Bytes and modification times are kept.
        const QString copy = QDir(folder).absoluteFilePath(QStringLiteral("Media/001/a.mp4"));
        QCOMPARE(readFile(copy), QByteArray(5000, 'a'));
        QCOMPARE(QFileInfo(copy).lastModified().toMSecsSinceEpoch(),
            QFileInfo(path(QStringLiteral("src/a.mp4"))).lastModified().toMSecsSinceEpoch());
        QVERIFY(QFileInfo(QDir(folder).absoluteFilePath(QStringLiteral("Media/002/b.wav"))).isFile());
        QVERIFY(QFileInfo(QDir(folder).absoluteFilePath(QStringLiteral("Media/003/img_002.png"))).isFile());
        QVERIFY(!QFileInfo(QDir(folder).absoluteFilePath(QStringLiteral("Media/003/other.png"))).exists());
        QVERIFY(QFileInfo(QDir(folder).absoluteFilePath(QStringLiteral("Luts/001/look.cube"))).isFile());
        QVERIFY(!QFileInfo(QDir(folder).absoluteFilePath(QStringLiteral("Proxy"))).exists());

        // The project inside: relative paths into the archive, links without proxies.
        const QJsonObject doc = readJson(r.projectFile);
        const QJsonArray items = itemsOf(doc);
        QCOMPARE(items[0].toObject().value(QStringLiteral("relativePath")).toString(),
            QStringLiteral("Media/001/a.mp4"));
        QCOMPARE(items[0].toObject().value(QStringLiteral("path")).toString(), copy);
        QCOMPARE(items[1].toObject().value(QStringLiteral("relativePath")).toString(),
            QStringLiteral("Media/002/b.wav"));
        QCOMPARE(items[2].toObject().value(QStringLiteral("relativePath")).toString(),
            QStringLiteral("Media/003/img_%03d.png"));
        QCOMPARE(items[3].toObject().value(QStringLiteral("path")).toString(),
            path(QStringLiteral("gone/offline.mov"))); // stays as it was
        QCOMPARE(items[4].toObject().value(QStringLiteral("relativePath")).toString(),
            QStringLiteral("Media/001/a.mp4"));
        QCOMPARE(doc.value(QStringLiteral("timelines")).toArray()[0].toObject()
                     .value(QStringLiteral("nodes")).toArray()[0].toObject()
                     .value(QStringLiteral("lutRelative")).toString(),
            QStringLiteral("Luts/001/look.cube"));
        QVERIFY(doc.value(QStringLiteral("mediaLinks")).toArray().isEmpty());
        QCOMPARE(readJson(QDir(folder).absoluteFilePath(QStringLiteral("archive.json")))
                     .value(QStringLiteral("media")).toInt(),
            5);
    }

    void neverOverwritesAndCancelLeavesNothing()
    {
        const QString folder = path(QStringLiteral("out/film.dra"));
        const ArchiveResult again
            = exportArchive(m_json, path(QString()), QStringLiteral("film"), folder, {});
        QCOMPARE(again.status, Status::Failed);
        QVERIFY(again.error.contains(QLatin1String("already exists")));

        const QString other = path(QStringLiteral("out/cancelled.dra"));
        const ArchiveResult cancelled = exportArchive(m_json, path(QString()), QStringLiteral("film"),
            other, {}, [](qint64, qint64) { return false; });
        QCOMPARE(cancelled.status, Status::Cancelled);
        QVERIFY(!QFileInfo::exists(other));
        // An existing empty folder is used, and stays when cancelled.
        const QString empty = path(QStringLiteral("out/empty.dra"));
        QVERIFY(QDir().mkpath(empty));
        const ArchiveResult cancelled2 = exportArchive(m_json, path(QString()), QStringLiteral("film"),
            empty, {}, [](qint64, qint64) { return false; });
        QCOMPARE(cancelled2.status, Status::Cancelled);
        QVERIFY(QDir(empty).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot));
    }

    void proxyMediaAndRenderCacheOnRequest()
    {
        const QString folder = path(QStringLiteral("out/full.dra"));
        ArchiveOptions options;
        options.proxyMedia = true;
        options.renderCacheFiles = {m_cacheFile};
        const ArchiveResult r
            = exportArchive(m_json, path(QString()), QStringLiteral("film"), folder, options);
        QVERIFY2(r.status == Status::Ok, qPrintable(r.error));
        QCOMPARE(r.proxyFiles, 1);
        QCOMPARE(r.cacheFiles, 1);
        QCOMPARE(readFile(QDir(folder).absoluteFilePath(QStringLiteral("Proxy/001/a_proxy.mov"))),
            QByteArray(700, 'p'));
        QCOMPARE(readFile(QDir(folder).absoluteFilePath(QStringLiteral("Cache/0123.mov"))),
            QByteArray(900, 'c'));
        const QJsonObject link = readJson(r.projectFile)
                                     .value(QStringLiteral("mediaLinks")).toArray()[0].toObject()
                                     .value(QStringLiteral("proxy")).toObject();
        QCOMPARE(link.value(QStringLiteral("archiveRelative")).toString(),
            QStringLiteral("Proxy/001/a_proxy.mov"));
        QCOMPARE(link.value(QStringLiteral("format")).toString(), QStringLiteral("h264"));
    }

    void restoreToAnotherPlace()
    {
        const QString folder = path(QStringLiteral("out/full.dra"));
        const QString target = path(QStringLiteral("restored/film"));
        const QString cache = path(QStringLiteral("newcache"));
        qint64 lastDone = 0;
        qint64 lastTotal = 0;
        const RestoreResult r = restoreArchive(folder, target, cache, [&](qint64 d, qint64 t) {
            lastDone = d;
            lastTotal = t;
            return true;
        });
        QVERIFY2(r.status == Status::Ok, qPrintable(r.error));
        QCOMPARE(r.projectFile, QDir(target).absoluteFilePath(QStringLiteral("film.kkproj")));
        QCOMPARE(r.cacheFiles, 1);
        QCOMPARE(lastDone, lastTotal);
        QCOMPARE(readFile(cache + QStringLiteral("/0123.mov")), QByteArray(900, 'c'));
        QVERIFY(!QFileInfo::exists(target + QStringLiteral("/Cache")));

        const QJsonObject doc = readJson(r.projectFile);
        const QJsonArray items = itemsOf(doc);
        QCOMPARE(items[0].toObject().value(QStringLiteral("path")).toString(),
            QDir(target).absoluteFilePath(QStringLiteral("Media/001/a.mp4")));
        QCOMPARE(readFile(items[1].toObject().value(QStringLiteral("path")).toString()),
            QByteArray(300, 'b'));
        QCOMPARE(items[3].toObject().value(QStringLiteral("path")).toString(),
            path(QStringLiteral("gone/offline.mov")));
        const QJsonObject node = doc.value(QStringLiteral("timelines")).toArray()[0].toObject()
                                     .value(QStringLiteral("nodes")).toArray()[0].toObject();
        QCOMPARE(node.value(QStringLiteral("lut")).toString(),
            QDir(target).absoluteFilePath(QStringLiteral("Luts/001/look.cube")));
        const QJsonObject proxy = doc.value(QStringLiteral("mediaLinks")).toArray()[0].toObject()
                                      .value(QStringLiteral("proxy")).toObject();
        QCOMPARE(proxy.value(QStringLiteral("path")).toString(),
            QDir(target).absoluteFilePath(QStringLiteral("Proxy/001/a_proxy.mov")));
        QVERIFY(!proxy.contains(QStringLiteral("archiveRelative")));

        // Never overwrites a folder that has files; a second restore of the cache keeps files.
        const RestoreResult again = restoreArchive(folder, target, cache);
        QCOMPARE(again.status, Status::Failed);
        QVERIFY(again.error.contains(QLatin1String("already exists")));
        const RestoreResult second = restoreArchive(folder, path(QStringLiteral("restored/two")), cache);
        QVERIFY2(second.status == Status::Ok, qPrintable(second.error));
        QCOMPARE(second.cacheFiles, 0);
    }

    void notAnArchive()
    {
        QVERIFY(!isArchiveFolder(path(QStringLiteral("src"))));
        const RestoreResult r = restoreArchive(path(QStringLiteral("src")), path(QStringLiteral("x")));
        QCOMPARE(r.status, Status::Failed);
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("x"))));
    }

    void mediaInsideASingleFileProject()
    {
        // Pack the linked project (a.mp4, b.wav, ...), then archive the container's runtime
        // document: the media are read out of the container.
        const QString container = path(QStringLiteral("film.kinoklip"));
        const PackResult packed = packProject(m_project, container);
        QVERIFY2(packed.status == Status::Ok, qPrintable(packed.error));
        QString error;
        auto package = ProjectPackage::open(container, false, &error);
        QVERIFY2(package, qPrintable(error));
        const QString folder = path(QStringLiteral("out/from-container.dra"));
        const ArchiveResult r = exportArchive(
            package->document(&error), path(QStringLiteral("elsewhere")), QStringLiteral("film"), folder, {});
        QVERIFY2(r.status == Status::Ok, qPrintable(r.error));
        QCOMPARE(r.lutFiles, 1);
        QCOMPARE(readFile(QDir(folder).absoluteFilePath(QStringLiteral("Media/001/a.mp4"))),
            QByteArray(5000, 'a'));
        const QJsonObject doc = readJson(r.projectFile);
        QVERIFY(!itemsOf(doc)[0].toObject().contains(QStringLiteral("embedded")));
        QVERIFY(!itemsOf(doc)[0].toObject().value(QStringLiteral("path")).toString()
                     .contains(QLatin1String(".kinoklip")));
    }
};

QTEST_MAIN(TestProjectArchive)
#include "test_projectarchive.moc"
