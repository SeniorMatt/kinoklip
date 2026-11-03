// SPDX-License-Identifier: GPL-3.0-or-later
// test_projectpackage — single-file projects end to end on real projects: Pack, open the
// document through the normal project loader, save without touching the media, Add Media with
// duplicate detection, Unpack, Compact, Verify (and damage detection), cancellation, and an
// archive extracted with a standard tool opening as a linked project.
#include "../media/MediaTestUtil.h"
#include "core/Project.h"
#include "media/MediaInfo.h"
#include "media/MediaSource.h"
#include "pack/EmbeddedMedia.h"
#include "pack/ProjectPackage.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace kk;
using namespace kk::pack;

namespace {

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

/// Paths of the file items of a loaded project's media pool.
QStringList itemPaths(const Project& project)
{
    QStringList paths;
    for (const MediaItem& m : project.mediaPool()->state().items) {
        if (!m.isTimeline())
            paths.append(m.path);
    }
    return paths;
}

/// Entry name of the stored file called @p fileName ("media/<id>/<fileName>"), or empty.
QString entryOf(const QString& container, const QString& fileName)
{
    const auto package = ProjectPackage::open(container, false, nullptr);
    for (const ContainerEntry& e : package->container().entries()) {
        if (e.name.startsWith(QLatin1String("media/"))
            && e.name.endsWith(QLatin1Char('/') + fileName))
            return e.name;
    }
    return {};
}

} // namespace

class TestProjectPackage : public QObject
{
    Q_OBJECT

    QTemporaryDir m_dir;
    QString m_skip;
    QString m_linkedProject; ///< .kkproj using three fixtures and one offline file
    QStringList m_media; ///< the three fixtures

    QString path(const QString& name) const { return m_dir.filePath(name); }

    /// Loads a runtime-form document through the normal project loader.
    bool loadDocument(Project& project, const QByteArray& runtime, QString* error)
    {
        const QString file = path(QStringLiteral("runtime.kkproj"));
        return writeFile(file, runtime) && project.load(file, error);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        if (!test::ensureFixtures(&m_skip))
            return;
        m_media = {
            test::fixture("h264_25fps.mp4"), test::fixture("still.jpg"), test::fixture("sine.mp3")};
        const QString offline = path(QStringLiteral("offline.mp4"));
        QVERIFY(QFile::copy(test::fixture("h264_2997.mp4"), offline));
        Project project;
        const ImportResult imported = project.importMedia(m_media + QStringList {offline});
        QCOMPARE(imported.importedIds.size(), 4);
        QDir().mkpath(path(QStringLiteral("linked")));
        m_linkedProject = path(QStringLiteral("linked/film.kkproj"));
        QString error;
        QVERIFY2(project.save(m_linkedProject, &error), qPrintable(error));
        QVERIFY(QFile::remove(offline)); // offline when packing
    }

    void packAndOpen()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        const QString container = path(QStringLiteral("film.kinoklip"));
        qint64 lastDone = 0;
        qint64 lastTotal = 0;
        const PackResult packed
            = packProject(m_linkedProject, container, [&](qint64 done, qint64 total) {
                  lastDone = done;
                  lastTotal = total;
                  return true;
              });
        QVERIFY2(packed.status == Status::Ok, qPrintable(packed.error));
        QCOMPARE(packed.embedded, 3);
        QCOMPARE(packed.skipped.size(), 1);
        QVERIFY(packed.skipped.front().contains(QLatin1String("offline")));
        QVERIFY(lastTotal > 0);
        QCOMPARE(lastDone, lastTotal);
        QVERIFY(!QFileInfo::exists(container + QStringLiteral(".part")));

        QString error;
        auto package = ProjectPackage::open(container, false, &error);
        QVERIFY2(package, qPrintable(error));
        QCOMPARE(package->media().size(), 3);
        QCOMPARE(package->unusedMedia(), QStringList());
        // Stored form: entry names only (no absolute paths in the document).
        const QJsonObject stored = QJsonDocument::fromJson(package->storedDocument()).object();
        int embedded = 0;
        for (const QJsonValue& v : stored.value(QLatin1String("mediaPool"))
                                       .toObject()
                                       .value(QLatin1String("items"))
                                       .toArray()) {
            const QJsonObject o = v.toObject();
            if (o.contains(QLatin1String("embedded"))) {
                ++embedded;
                QVERIFY(
                    o.value(QLatin1String("path")).toString().startsWith(QLatin1String("media/")));
                QCOMPARE(
                    o.value(QLatin1String("relativePath")), o.value(QLatin1String("embedded")));
            }
        }
        QCOMPARE(embedded, 3);

        // Runtime form through the normal loader: embedded paths that exist and probe.
        Project project;
        QVERIFY2(loadDocument(project, package->document(&error), &error), qPrintable(error));
        const QStringList paths = itemPaths(project);
        QCOMPARE(paths.size(), 4);
        int inside = 0;
        for (const QString& p : paths) {
            if (!p.startsWith(container + QLatin1Char('/')))
                continue;
            ++inside;
            QVERIFY2(media::mediaExists(p), qPrintable(p));
            const media::MediaInfo info = media::probe(p);
            QVERIFY2(info.valid, qPrintable(p + QLatin1Char(' ') + info.error));
        }
        QCOMPARE(inside, 3);
        // Probe results equal those of the original files.
        const media::MediaInfo original = media::probe(m_media[0]);
        const QString clipEntry = entryOf(container, QStringLiteral("h264_25fps.mp4"));
        QVERIFY(QRegularExpression(QStringLiteral("^media/[0-9a-f]{8}/h264_25fps\\.mp4$"))
                    .match(clipEntry)
                    .hasMatch());
        const media::MediaInfo packedInfo = media::probe(embeddedPath(container, clipEntry));
        QCOMPARE(packedInfo.durationUs, original.durationUs);
        QCOMPARE(packedInfo.width, original.width);
        QCOMPARE(packedInfo.videoCodec, original.videoCodec);
        QCOMPARE(packedInfo.container, original.container);

        const VerifyResult verified = verifyContainer(container);
        QVERIFY2(verified.ok, qPrintable(verified.problems.join(QLatin1Char('\n'))));
        QCOMPARE(verified.recovery, Recovery::Clean);
    }

    /// Saving a project into the container appends only the document and manifest.
    void saveDoesNotRewriteMedia()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        const QString container = path(QStringLiteral("film.kinoklip"));
        QString error;
        auto package = ProjectPackage::open(container, true, &error);
        QVERIFY2(package, qPrintable(error));
        QHash<QString, qint64> offsets;
        for (const ContainerEntry& e : package->container().entries())
            offsets.insert(e.name, e.dataOffset);
        const QByteArray mediaBytes = readFile(container).left(
            int(package->container().find(QStringLiteral("project.kkproj"))->headerOffset));
        Project project;
        QVERIFY(loadDocument(project, package->document(), &error));
        project.setName(QStringLiteral("Renamed in the container"));
        const QString runtimeCopy = path(QStringLiteral("save.kkproj"));
        QVERIFY(project.saveCopy(runtimeCopy, &error));
        const qint64 before = QFileInfo(container).size();
        QVERIFY2(package->saveDocument(readFile(runtimeCopy), &error), qPrintable(error));
        const qint64 growth = QFileInfo(container).size() - before;
        QVERIFY2(growth < readFile(runtimeCopy).size() + 8192, qPrintable(QString::number(growth)));
        package.reset();
        // Every byte before the old document is unchanged (except the header slots in the first
        // 106 bytes, which every commit updates); media offsets too.
        QCOMPARE(readFile(container).left(mediaBytes.size()).mid(106), mediaBytes.mid(106));
        package = ProjectPackage::open(container, false, &error);
        for (const ContainerEntry& e : package->container().entries()) {
            if (e.name.startsWith(QLatin1String("media/")))
                QCOMPARE(e.dataOffset, offsets.value(e.name));
        }
        Project reloaded;
        QVERIFY(loadDocument(reloaded, package->document(), &error));
        QCOMPARE(reloaded.name(), QStringLiteral("Renamed in the container"));
        QCOMPARE(itemPaths(reloaded), itemPaths(project));
    }

    void addMediaDeduplicates()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        QString error;
        const QString container = path(QStringLiteral("dedupe.kinoklip"));
        auto package = ProjectPackage::create(container, &error);
        QVERIFY2(package, qPrintable(error));
        QVector<ProjectPackage::Added> added;
        QCOMPARE(package->addMedia({m_media[0], m_media[1]}, &added, {}, &error), Status::Ok);
        QCOMPARE(added.size(), 2);
        QVERIFY(!added[0].duplicate && !added[1].duplicate);
        QCOMPARE(added[0].path, embeddedPath(container, added[0].entry));
        const QString clipEntry = added[0].entry;
        const qint64 size = QFileInfo(container).size();
        // The same file again, a copy under another name, and the embedded path itself.
        const QString copy = path(QStringLiteral("copy of clip.mp4"));
        QVERIFY(QFile::copy(m_media[0], copy));
        QCOMPARE(
            package->addMedia({m_media[0], copy, added[1].path}, &added, {}, &error), Status::Ok);
        QCOMPARE(added.size(), 3);
        for (const ProjectPackage::Added& a : std::as_const(added))
            QVERIFY2(a.duplicate, qPrintable(a.source));
        QCOMPARE(added[1].entry, clipEntry);
        QCOMPARE(QFileInfo(container).size(), size); // nothing written
        // Same name, different content: a second entry with a unique name.
        QDir().mkpath(path(QStringLiteral("other")));
        const QString sameName = path(QStringLiteral("other/still.jpg"));
        QVERIFY(QFile::copy(test::fixture("still_alpha.png"), sameName));
        QCOMPARE(package->addMedia({sameName}, &added, {}, &error), Status::Ok);
        QVERIFY(added.front().entry.endsWith(QLatin1String("/still.jpg")));
        QVERIFY(added.front().entry != added[1].entry);
        QVERIFY(!added.front().duplicate);
        // Cancelled: nothing of the call is kept.
        const qint64 beforeCancel = QFileInfo(container).size();
        QCOMPARE(package->addMedia(
                     {test::fixture("h264_60fps.mp4")}, &added,
                     [](qint64, qint64) { return false; }, &error),
            Status::Cancelled);
        QCOMPARE(QFileInfo(container).size(), beforeCancel);
        QCOMPARE(package->media().size(), 3);
        package.reset();
        package = ProjectPackage::open(container, false, &error);
        QVERIFY(package);
        QCOMPARE(package->media().size(), 3);
        QCOMPARE(
            package->record(clipEntry)->originalPath, QFileInfo(m_media[0]).absoluteFilePath());
        // Importing into the media pool by embedded path works like a file.
        Project project;
        const ImportResult imported = project.importMedia({embeddedPath(container, clipEntry)});
        QCOMPARE(imported.importedIds.size(), 1);
    }

    void unpack()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        const QString container = path(QStringLiteral("film.kinoklip"));
        const QString folder = path(QStringLiteral("unpacked"));
        const UnpackResult result = unpackProject(container, folder);
        QVERIFY2(result.status == Status::Ok, qPrintable(result.error));
        QCOMPARE(result.extracted, 3);
        QCOMPARE(result.projectFile, QDir(folder).absoluteFilePath(QStringLiteral("film.kkproj")));
        Project project;
        QString error;
        QVERIFY2(project.load(result.projectFile, &error), qPrintable(error));
        QCOMPARE(project.name(), QStringLiteral("Renamed in the container"));
        int local = 0;
        for (const QString& p : itemPaths(project)) {
            if (!p.startsWith(QDir(folder).absolutePath()))
                continue;
            ++local;
            QVERIFY(QFileInfo(p).isFile());
        }
        QCOMPARE(local, 3);
        for (const QString& original : m_media) {
            const QString out
                = QDir(folder).absoluteFilePath(entryOf(container, QFileInfo(original).fileName()));
            QCOMPARE(readFile(out), readFile(original));
            QCOMPARE(QFileInfo(out).lastModified().toMSecsSinceEpoch(),
                QFileInfo(original).lastModified().toMSecsSinceEpoch());
        }
        // Never overwrites.
        const UnpackResult again = unpackProject(container, folder);
        QCOMPARE(again.status, Status::Failed);
        QVERIFY(again.error.contains(QLatin1String("already exists")));
    }

    /// An archive extracted with a standard unzip tool opens as a normal linked project.
    void extractedWithStandardToolOpens()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (python.isEmpty())
            QSKIP("python3 not available");
        const QString folder = path(QStringLiteral("extracted"));
        QProcess p;
        p.start(python,
            {QStringLiteral("-I"), QStringLiteral("-c"),
                QStringLiteral(
                    "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])"),
                path(QStringLiteral("film.kinoklip")), folder});
        QVERIFY(p.waitForFinished(60000));
        QCOMPARE(p.exitCode(), 0);
        Project project;
        QString error;
        QVERIFY2(
            project.load(folder + QStringLiteral("/project.kkproj"), &error), qPrintable(error));
        int found = 0;
        for (const QString& path : itemPaths(project)) {
            if (path.startsWith(QDir(folder).absolutePath()) && QFileInfo(path).isFile())
                ++found;
        }
        QCOMPARE(found, 3);
    }

    void compactAndVerify()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        QString error;
        const QString container = path(QStringLiteral("compact.kinoklip"));
        QVERIFY(QFile::copy(path(QStringLiteral("film.kinoklip")), container));
        qint64 estimate = 0;
        {
            auto package = ProjectPackage::open(container, true, &error);
            QVERIFY(package);
            // Unused media and a few old document versions.
            QCOMPARE(package->addMedia({test::fixture("h264_60fps.mp4")}, nullptr, {}, &error),
                Status::Ok);
            const QByteArray doc = package->document();
            for (int i = 0; i < 5; ++i)
                QVERIFY(package->saveDocument(doc, &error));
            QCOMPARE(package->unusedMedia(),
                QStringList {entryOf(container, QStringLiteral("h264_60fps.mp4"))});
            estimate = package->compactedSizeEstimate();
        }
        VerifyResult v = verifyContainer(container);
        QVERIFY(v.ok);
        QVERIFY(v.notes.join(QLatin1Char(' ')).contains(QLatin1String("h264_60fps.mp4")));
        const QByteArray documentBefore
            = ProjectPackage::open(container, false, &error)->storedDocument();

        const CompactResult result = compactProject(container);
        QVERIFY2(result.status == Status::Ok, qPrintable(result.error));
        QCOMPARE(result.removedMedia.size(), 1);
        QVERIFY(result.removedMedia.front().endsWith(QLatin1String("/h264_60fps.mp4")));
        QCOMPARE(result.sizeBefore - QFileInfo(test::fixture("h264_60fps.mp4")).size()
                > result.sizeAfter,
            true);
        QCOMPARE(result.sizeAfter, QFileInfo(container).size());
        QVERIFY2(qAbs(result.sizeAfter - estimate) < 4096,
            qPrintable(
                QStringLiteral("estimate %1, actual %2").arg(estimate).arg(result.sizeAfter)));
        qInfo("compact: %lld -> %lld bytes", result.sizeBefore, result.sizeAfter);
        const QStringList leftovers
            = QDir(m_dir.path())
                  .entryList({QStringLiteral(".*compact*")}, QDir::Files | QDir::Hidden);
        QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QLatin1Char(' '))));
        auto package = ProjectPackage::open(container, false, &error);
        QVERIFY(package);
        QCOMPARE(package->storedDocument(), documentBefore);
        QCOMPARE(package->media().size(), 3);
        QCOMPARE(package->container().entries().size(), 6); // mimetype, 3 media, manifest, doc
        v = verifyContainer(container);
        QVERIFY2(v.ok, qPrintable(v.problems.join(QLatin1Char('\n'))));
        QVERIFY(media::probe(
            embeddedPath(container, entryOf(container, QStringLiteral("h264_25fps.mp4"))))
                    .valid);

        // Damage inside a media entry is found by Verify (and refused by Compact).
        const ContainerEntry* e
            = package->container().find(entryOf(container, QStringLiteral("sine.mp3")));
        const qint64 at = e->dataOffset + e->size / 2;
        package.reset();
        {
            QFile f(container);
            QVERIFY(f.open(QIODevice::ReadWrite));
            f.seek(at);
            char c = 0;
            f.getChar(&c);
            f.seek(at);
            f.putChar(char(c ^ 0x55));
        }
        v = verifyContainer(container);
        QVERIFY(!v.ok);
        QVERIFY2(v.problems.join(QLatin1Char(' ')).contains(QLatin1String("sine.mp3")),
            qPrintable(v.problems.join(QLatin1Char(' '))));
        const CompactResult refused = compactProject(container);
        QCOMPARE(refused.status, Status::Failed);
        QVERIFY(refused.error.contains(QLatin1String("CRC")));
        QVERIFY(QFileInfo(container).size() == result.sizeAfter); // the original is untouched
    }

    void repairAfterCrash()
    {
        if (!m_skip.isEmpty())
            QSKIP(qPrintable(m_skip));
        const QString container = path(QStringLiteral("repair.kinoklip"));
        QVERIFY(QFile::copy(path(QStringLiteral("film.kinoklip")), container));
        {
            QFile f(container);
            QVERIFY(f.open(QIODevice::Append));
            f.write(QByteArray(5000, 'x')); // a torn save
        }
        VerifyResult v = verifyContainer(container);
        QVERIFY(v.ok);
        QCOMPARE(v.recovery, Recovery::HeaderSlot);
        QCOMPARE(v.ignoredTailBytes, 5000);
        QString error;
        QVERIFY2(repairContainer(container, &error), qPrintable(error));
        v = verifyContainer(container);
        QVERIFY(v.ok);
        QCOMPARE(v.recovery, Recovery::Clean);
        QCOMPARE(v.ignoredTailBytes, 0);
    }
};

QTEST_GUILESS_MAIN(TestProjectPackage)
#include "test_projectpackage.moc"
